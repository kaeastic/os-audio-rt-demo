// rt_audio_demo.c
// OS project: audio-like producer/consumer with semaphores, mutex,
// optional priority inheritance, and different scheduling policies.
// Generates a WAV file so you can HEAR glitches for bad configurations.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ========================= CONFIG DEFAULTS =========================

#define DEFAULT_SAMPLE_RATE         48000      // 48 kHz
#define DEFAULT_FRAMES_PER_PERIOD   480        // 10ms at 48kHz
#define DEFAULT_BUFFER_DEPTH        4          // number of periods in ring buffer
#define DEFAULT_RUN_SECONDS         10         // length of experiment

#define DEFAULT_MISS_THRESHOLD_NS   500000LL   // 0.5 ms late = "miss"

// For priority inheritance on the mutex
#define DEFAULT_USE_PI              1          // 1 = enable PTHREAD_PRIO_INHERIT

// Max total periods we expect in the run (safe upper bound)
#define MAX_TOTAL_PERIODS(sample_rate, run_seconds, frames_per_period) \
    ((int)((sample_rate) * (run_seconds) / (frames_per_period) + 100))

// ========================== GLOBAL CONFIG ==========================

typedef struct {
    int sample_rate;
    int frames_per_period;
    int buffer_depth;
    int run_seconds;
    long long miss_threshold_ns;
    int use_pi;        // 0 or 1

    int policy;        // SCHED_OTHER / SCHED_FIFO / SCHED_RR
    int prio_prod;     // RT priority for producer (if RT policy)
    int prio_cons;     // RT priority for consumer
    int prio_dist;     // RT priority for disturber
} config_t;

static config_t g_cfg = {
    .sample_rate       = DEFAULT_SAMPLE_RATE,
    .frames_per_period = DEFAULT_FRAMES_PER_PERIOD,
    .buffer_depth      = DEFAULT_BUFFER_DEPTH,
    .run_seconds       = DEFAULT_RUN_SECONDS,
    .miss_threshold_ns = DEFAULT_MISS_THRESHOLD_NS,
    .use_pi            = DEFAULT_USE_PI,
    .policy            = SCHED_OTHER, // default: normal scheduling
    .prio_prod         = 70,
    .prio_cons         = 60,
    .prio_dist         = 10
};

// ============================== DATA ===============================

typedef struct {
    int16_t **data;    // [buffer_depth][frames_per_period]
    int head;          // next write index
    int tail;          // next read index
} ring_buffer_t;

static ring_buffer_t g_ring;

// semaphores and mutex
static sem_t sem_empty;   // counts empty slots
static sem_t sem_full;    // counts full slots
static pthread_mutex_t buf_mutex;

// control + stats
static volatile bool g_stop = false;
static volatile int g_producer_misses = 0;

// We'll dump consumed audio into this for WAV writing
static int16_t *g_output_buffer = NULL;
static int      g_output_buffer_capacity_periods = 0;
static volatile int g_output_periods = 0;

// =========================== TIME HELPERS ==========================

static inline int64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// sleep until absolute time t_ns (CLOCK_MONOTONIC)
static void sleep_until_ns(int64_t t_ns) {
    struct timespec ts;
    ts.tv_sec  = t_ns / 1000000000LL;
    ts.tv_nsec = t_ns % 1000000000LL;
    // ignore EINTR for simplicity
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

// ======================= AUDIO GENERATION ==========================

// Generate one period of sine wave into dest[]
static void generate_sine_period(int16_t *dest, int frames, double *phase, int sample_rate) {
    const double freq = 440.0;  // A4
    const double two_pi = 2.0 * M_PI;
    double phase_inc = two_pi * freq / (double)sample_rate;

    for (int i = 0; i < frames; ++i) {
        *phase += phase_inc;
        if (*phase > two_pi) *phase -= two_pi;
        // scale for safety
        double s = sin(*phase) * 8000.0;  // ~25% of int16 range
        dest[i] = (int16_t) s;
    }
}

// ========================= THREAD FUNCTIONS ========================

static void* producer_thread(void *arg) {
    (void)arg;
    printf("[producer] started\n");

    double phase = 0.0;
    int sr   = g_cfg.sample_rate;
    int fpp  = g_cfg.frames_per_period;
    long long miss_thresh = g_cfg.miss_threshold_ns;

    int64_t period_ns = (int64_t)fpp * 1000000000LL / sr;
    int64_t start     = nsec_now();
    int64_t next_deadline = start + period_ns;

    while (!g_stop) {
        // sleep until next period
        sleep_until_ns(next_deadline);
        int64_t woke = nsec_now();
        int64_t lateness = woke - next_deadline;
        if (lateness > miss_thresh) {
            __sync_fetch_and_add(&g_producer_misses, 1);
        }

        // Wait for an empty slot
        if (sem_wait(&sem_empty) != 0) {
            if (errno == EINTR) continue;
            perror("sem_wait(sem_empty)");
            break;
        }

        // Lock buffer and write one period
        if (pthread_mutex_lock(&buf_mutex) != 0) {
            perror("pthread_mutex_lock");
            break;
        }

        int idx = g_ring.head;
        int16_t *slot = g_ring.data[idx];

        // If we were very late, write SILENCE instead of tone to make a glitch audible
        if (lateness > miss_thresh) {
            memset(slot, 0, sizeof(int16_t) * fpp);
        } else {
            generate_sine_period(slot, fpp, &phase, sr);
        }

        g_ring.head = (g_ring.head + 1) % g_cfg.buffer_depth;
        pthread_mutex_unlock(&buf_mutex);

        // Signal that one more period is available
        sem_post(&sem_full);

        // set next deadline
        next_deadline += period_ns;
    }

    printf("[producer] exiting\n");
    return NULL;
}

static void* consumer_thread(void *arg) {
    (void)arg;
    printf("[consumer] started\n");

    int fpp   = g_cfg.frames_per_period;
    int16_t *local_buf = malloc(sizeof(int16_t) * fpp);
    if (!local_buf) {
        perror("malloc local_buf");
        return NULL;
    }

    while (!g_stop) {
        int rc = sem_wait(&sem_full);
        if (rc != 0) {
            if (errno == EINTR) continue;
            perror("sem_wait(sem_full)");
            break;
        }

        if (pthread_mutex_lock(&buf_mutex) != 0) {
            perror("pthread_mutex_lock");
            break;
        }

        int idx = g_ring.tail;
        memcpy(local_buf, g_ring.data[idx], sizeof(int16_t) * fpp);
        g_ring.tail = (g_ring.tail + 1) % g_cfg.buffer_depth;

        pthread_mutex_unlock(&buf_mutex);
        sem_post(&sem_empty);

        // Copy into output buffer for WAV
        int p = g_output_periods;
        if (p < g_output_buffer_capacity_periods) {
            memcpy(&g_output_buffer[p * fpp], local_buf, sizeof(int16_t) * fpp);
            __sync_fetch_and_add(&g_output_periods, 1);
        } else {
            // overflow, just drop extra
        }
    }

    free(local_buf);
    printf("[consumer] exiting\n");
    return NULL;
}

// Disturber thread: simulates a low-priority task that sometimes
// holds the mutex, causing priority inversion if PI is off.
static void* disturber_thread(void *arg) {
    (void)arg;
    printf("[disturber] started\n");

    // Light-weight disturber for WSL: just sleep a bit in a loop,
    // so we don't starve producer/consumer.
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1 * 1000 * 1000 }; // 1ms
    while (!g_stop) {
        nanosleep(&ts, NULL);
    }

    printf("[disturber] exiting\n");
    return NULL;
}


// ========================= SCHEDULING HELPERS ======================

static void set_thread_sched(pthread_t th, int policy, int prio, const char *name) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;

    int rc = pthread_setschedparam(th, policy, &sp);
    if (rc != 0) {
        fprintf(stderr, "[warn] pthread_setschedparam(%s) failed: %s\n",
                name, strerror(rc));
        fprintf(stderr, "       (Run with sudo or give CAP_SYS_NICE, "
                        "or leave policy=SCHED_OTHER.)\n");
    } else {
        printf("[%s] set policy=%d prio=%d\n", name, policy, prio);
    }
}

// ========================== WAV WRITER =============================

// Simple 16-bit mono PCM WAV header
#pragma pack(push, 1)
typedef struct {
    char     riff[4];       // "RIFF"
    uint32_t chunk_size;
    char     wave[4];       // "WAVE"
    char     fmt_[4];       // "fmt "
    uint32_t subchunk1_size;// 16
    uint16_t audio_format;  // 1 = PCM
    uint16_t num_channels;  // 1 mono
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;// 16
    char     data[4];       // "data"
    uint32_t data_size;
} wav_header_t;
#pragma pack(pop)

static int write_wav(const char *filename, const int16_t *samples,
                     int total_samples, int sample_rate) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("fopen wav");
        return -1;
    }

    wav_header_t hdr;
    memcpy(hdr.riff, "RIFF", 4);
    memcpy(hdr.wave, "WAVE", 4);
    memcpy(hdr.fmt_, "fmt ", 4);
    memcpy(hdr.data, "data", 4);

    hdr.subchunk1_size = 16;
    hdr.audio_format   = 1;
    hdr.num_channels   = 1;
    hdr.sample_rate    = sample_rate;
    hdr.bits_per_sample= 16;
    hdr.block_align    = hdr.num_channels * hdr.bits_per_sample / 8;
    hdr.byte_rate      = hdr.sample_rate * hdr.block_align;
    hdr.data_size      = total_samples * hdr.block_align;
    hdr.chunk_size     = 36 + hdr.data_size;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        perror("fwrite header");
        fclose(f);
        return -1;
    }

    if (fwrite(samples, sizeof(int16_t), total_samples, f) != (size_t)total_samples) {
        perror("fwrite samples");
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

// ========================== ARG PARSING ============================

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --policy=other|fifo|rr     Scheduling policy (default: other)\n");
    printf("  --pi=0|1                   Use priority inheritance on mutex (default: 1)\n");
    printf("  --run=N                    Run time in seconds (default: %d)\n", DEFAULT_RUN_SECONDS);
    printf("  --bufdepth=N               Buffer depth (periods) (default: %d)\n", DEFAULT_BUFFER_DEPTH);
    printf("  --fpp=N                    Frames per period (default: %d)\n", DEFAULT_FRAMES_PER_PERIOD);
    printf("  --sr=N                     Sample rate (default: %d)\n", DEFAULT_SAMPLE_RATE);
    printf("  --miss_ns=N                Miss threshold in ns (default: %lld)\n",
           (long long)DEFAULT_MISS_THRESHOLD_NS);
    printf("  --out=filename.wav         Output WAV file (default: out.wav)\n");
}

static const char *g_out_filename = "out.wav";

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--policy=", 9) == 0) {
            const char *val = argv[i] + 9;
            if (strcmp(val, "other") == 0) g_cfg.policy = SCHED_OTHER;
            else if (strcmp(val, "fifo") == 0) g_cfg.policy = SCHED_FIFO;
            else if (strcmp(val, "rr") == 0) g_cfg.policy = SCHED_RR;
            else {
                fprintf(stderr, "Unknown policy: %s\n", val);
                exit(1);
            }
        } else if (strncmp(argv[i], "--pi=", 5) == 0) {
            g_cfg.use_pi = atoi(argv[i] + 5) ? 1 : 0;
        } else if (strncmp(argv[i], "--run=", 6) == 0) {
            g_cfg.run_seconds = atoi(argv[i] + 6);
        } else if (strncmp(argv[i], "--bufdepth=", 11) == 0) {
            g_cfg.buffer_depth = atoi(argv[i] + 11);
        } else if (strncmp(argv[i], "--fpp=", 6) == 0) {
            g_cfg.frames_per_period = atoi(argv[i] + 6);
        } else if (strncmp(argv[i], "--sr=", 5) == 0) {
            g_cfg.sample_rate = atoi(argv[i] + 5);
        } else if (strncmp(argv[i], "--miss_ns=", 10) == 0) {
            g_cfg.miss_threshold_ns = atoll(argv[i] + 10);
        } else if (strncmp(argv[i], "--out=", 6) == 0) {
            g_out_filename = argv[i] + 6;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }
}

// ============================== MAIN ===============================

int main(int argc, char **argv) {
    parse_args(argc, argv);

    printf("=== RT Audio Demo ===\n");
    printf("Policy: %s\n",
           (g_cfg.policy == SCHED_OTHER) ? "SCHED_OTHER" :
           (g_cfg.policy == SCHED_FIFO)  ? "SCHED_FIFO" :
           (g_cfg.policy == SCHED_RR)    ? "SCHED_RR"   : "UNKNOWN");
    printf("Use PI: %d\n", g_cfg.use_pi);
    printf("Run seconds: %d\n", g_cfg.run_seconds);
    printf("Sample rate: %d\n", g_cfg.sample_rate);
    printf("Frames per period: %d\n", g_cfg.frames_per_period);
    printf("Buffer depth: %d periods\n", g_cfg.buffer_depth);
    printf("Miss threshold: %lld ns\n", (long long)g_cfg.miss_threshold_ns);
    printf("Output WAV: %s\n", g_out_filename);

    // Allocate ring buffer
    g_ring.data = malloc(sizeof(int16_t*) * g_cfg.buffer_depth);
    if (!g_ring.data) {
        perror("malloc ring.data");
        return 1;
    }
    for (int i = 0; i < g_cfg.buffer_depth; ++i) {
        g_ring.data[i] = malloc(sizeof(int16_t) * g_cfg.frames_per_period);
        if (!g_ring.data[i]) {
            perror("malloc ring slot");
            return 1;
        }
    }
    g_ring.head = 0;
    g_ring.tail = 0;

    // Allocate output buffer
    g_output_buffer_capacity_periods = MAX_TOTAL_PERIODS(
        g_cfg.sample_rate, g_cfg.run_seconds, g_cfg.frames_per_period);
    g_output_buffer = malloc(sizeof(int16_t) *
                             g_output_buffer_capacity_periods *
                             g_cfg.frames_per_period);
    if (!g_output_buffer) {
        perror("malloc output_buffer");
        return 1;
    }
    g_output_periods = 0;

    // Init semaphores
    if (sem_init(&sem_empty, 0, g_cfg.buffer_depth) != 0) {
        perror("sem_init(empty)");
        return 1;
    }
    if (sem_init(&sem_full, 0, 0) != 0) {
        perror("sem_init(full)");
        return 1;
    }

    // Init mutex with optional priority inheritance
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    if (g_cfg.use_pi) {
        int rc = pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
        if (rc != 0) {
            perror("pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)");
            fprintf(stderr, "Falling back to default mutex protocol.\n");
        } else {
            printf("[main] using PTHREAD_PRIO_INHERIT\n");
        }
    } else {
        printf("[main] NOT using priority inheritance\n");
    }
    pthread_mutex_init(&buf_mutex, &mattr);

    // Create threads
    pthread_t prod_th, cons_th, dist_th;
    if (pthread_create(&prod_th, NULL, producer_thread, NULL) != 0) {
        perror("pthread_create(producer)");
        return 1;
    }
    if (pthread_create(&cons_th, NULL, consumer_thread, NULL) != 0) {
        perror("pthread_create(consumer)");
        return 1;
    }
    if (pthread_create(&dist_th, NULL, disturber_thread, NULL) != 0) {
        perror("pthread_create(disturber)");
        return 1;
    }

    // Set scheduling (if policy is RT, this requires privileges)
    if (g_cfg.policy != SCHED_OTHER) {
        set_thread_sched(prod_th, g_cfg.policy, g_cfg.prio_prod, "producer");
        set_thread_sched(cons_th, g_cfg.policy, g_cfg.prio_cons, "consumer");
        set_thread_sched(dist_th, g_cfg.policy, g_cfg.prio_dist, "disturber");
    }

    // Run experiment
    sleep(g_cfg.run_seconds);
    g_stop = true;

    // Wake threads if they're blocked on semaphores
    for (int i = 0; i < g_cfg.buffer_depth + 2; ++i) {
        sem_post(&sem_full);
        sem_post(&sem_empty);
    }

    // Join threads
    pthread_join(prod_th, NULL);
    pthread_join(cons_th, NULL);
    pthread_join(dist_th, NULL);

    // Stats
    int periods = g_output_periods;
    int total_samples = periods * g_cfg.frames_per_period;

    printf("=== Stats ===\n");
    printf("Producer misses (woke > %.3f ms late): %d\n",
           g_cfg.miss_threshold_ns / 1e6, g_producer_misses);
    printf("Output periods: %d (~%.2f seconds of audio)\n",
           periods, (double)total_samples / g_cfg.sample_rate);

    // Write WAV
    if (periods > 0) {
        if (write_wav(g_out_filename, g_output_buffer,
                      total_samples, g_cfg.sample_rate) == 0) {
            printf("Wrote WAV: %s\n", g_out_filename);
        } else {
            fprintf(stderr, "Failed to write WAV\n");
        }
    } else {
        printf("No audio written (0 periods)\n");
    }

    // Cleanup
    sem_destroy(&sem_empty);
    sem_destroy(&sem_full);
    pthread_mutex_destroy(&buf_mutex);

    for (int i = 0; i < g_cfg.buffer_depth; ++i) {
        free(g_ring.data[i]);
    }
    free(g_ring.data);
    free(g_output_buffer);

    printf("Done.\n");
    return 0;
}
