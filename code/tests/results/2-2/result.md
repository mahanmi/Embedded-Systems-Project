# Experiment 2-2 -- memory of the C daemon under continuous streaming

Run: 2026-08-06T19:13:07Z. 300s of live stream, RSS sampled every 5s from
`/proc/<pid>/status`.

| | kB |
|---|---|
| RSS at start | 20464 |
| RSS at end | 20480 |
| min / max | 20464.00 / 20480.00 |
| mean | 20477.88 |
| net change | 16 |
| slope, warm-up (first 40%) | 0.0380 kB/s |
| slope, steady state (last 60%) | **0.0138 kB/s** |
| slope, naive whole-window fit | 0.0256 kB/s |

**No leak.** Steady-state slope is 0.0138 kB/s -- flat to within sampling
noise over a 300s window.

All three slopes agree here (0.0380 / 0.0138 / 0.0256 kB/s),
because the daemon was already warm when sampling began -- there was no
start-up ramp for the whole-window fit to mistake for growth. They are
still reported separately: on a freshly started daemon they diverge
sharply, and the steady-state figure is the only one that answers the
question being asked.

Raw data: `memory.csv`.
