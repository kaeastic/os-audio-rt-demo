## Real-Time Audio Scheduling Demo

Operating Systems Project – Kaeleigh Wren, Brian Fife, Kaden Bach  
`rt_audio_demo.c` showcases how Linux scheduling choices influence real-time audio pipelines by pairing a periodic producer with a consumer and an optional “disturber” thread.

- Producer generates a 440 Hz sine wave every audio period (default 10 ms at 48 kHz) and tracks deadline misses. Late periods write silence so glitches are audible.
- Consumer drains a ring buffer protected by mutex + semaphores and accumulates samples for writing to a 16-bit mono WAV file.
- Disturber simulates background load; disabling priority inheritance makes mutex contention audible.
- Command-line flags expose scheduling policy (`SCHED_OTHER`, `SCHED_FIFO`, `SCHED_RR`), priority inheritance, buffer depth, lateness threshold, and audio format parameters so you can hear how each knob affects glitch resilience.

### Why It Matters
Real audio engines must never under-run their buffers. By running this demo with different scheduler policies, buffer sizes, and mutex protocols, you can:
- Measure producer deadline misses,
- Inspect the generated WAV,
- Build intuition for how RT policies, priority inheritance, and synchronization choices interact.

### Build
Requirements: Linux/WSL2, `gcc`, POSIX threads.

```sh
gcc -O2 -Wall -pthread rt_audio_demo.c -o rt_audio_demo -lm
```

Artifacts:
- `rt_audio_demo.c` – source
- `rt_audio_demo` – executable

### Quick Runs
- Baseline (normal scheduling, PI off, shallow buffer):
  ```sh
  ./rt_audio_demo --policy=other --pi=0 --bufdepth=2 --run=5 --out=test.wav
  ```
- Real-time FIFO with priority inheritance (needs `sudo` or `CAP_SYS_NICE`):
  ```sh
  sudo ./rt_audio_demo --policy=fifo --pi=1 --bufdepth=2 --run=5 --out=fifo_pi.wav
  ```
- Open the WAV from WSL:
  ```sh
  explorer.exe fifo_pi.wav
  ```

### Command-Line Options
```
--policy=other|fifo|rr     Scheduling policy
--pi=0|1                   Enable priority inheritance on the mutex
--run=N                    Run duration in seconds
--bufdepth=N               Ring buffer depth (number of periods)
--fpp=N                    Frames per period (default 480)
--sr=N                     Sample rate (default 48000)
--miss_ns=N                Deadline miss threshold in nanoseconds
--out=filename.wav         Output WAV filename
```

### Suggested Experiments
1. **Glitchy baseline** – `./rt_audio_demo --policy=other --pi=0 --bufdepth=1 --miss_ns=200000 --run=5 --out=glitchy.wav`  
   Expect many producer misses and audible pops.

2. **Smooth RT case** – `sudo ./rt_audio_demo --policy=fifo --pi=1 --bufdepth=4 --run=5 --out=smooth.wav`  
   Expect zero misses and a continuous tone.

### Architecture Overview
- **Producer thread** sleeps with `clock_nanosleep(TIMER_ABSTIME)`, enforces deadlines, fills the ring, and marks late periods with silence.
- **Consumer thread** drains the ring, copies samples into a large output buffer, and feeds the WAV writer.
- **Disturber thread** adds contention to highlight priority inversion effects.
- **Synchronization** uses `sem_empty`, `sem_full`, and a mutex that optionally enables `PTHREAD_PRIO_INHERIT`.
- **Metrics**: deadline misses counted atomically, output duration derived from consumed periods, final audio written via a simple 16-bit PCM WAV header.

Listen to the resulting WAVs to correlate policy choices with audible quality and missed deadlines.

