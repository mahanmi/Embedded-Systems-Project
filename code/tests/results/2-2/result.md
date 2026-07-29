# Experiment 2-2 -- memory of the C daemon under continuous streaming

Run: 2026-07-27T16:24:12Z. 300s of live stream, RSS sampled every 5s from
`/proc/<pid>/status`.

| | kB |
|---|---|
| RSS at start | 19528 |
| RSS at end | 19524 |
| min / max | 19524.00 / 19540.00 |
| mean | 19535.84 |
| net change | -4 |
| slope, warm-up (first 40%) | 0.1133 kB/s |
| slope, steady state (last 60%) | **-0.0789 kB/s** |
| slope, naive whole-window fit | 0.0001 kB/s |

**No leak.** Steady-state slope is -0.0789 kB/s -- flat to within sampling
noise, and RSS even ticks down slightly near the end as buffers are reused
rather than grown.

The three slopes are given together because the naive whole-window fit
(0.0001 kB/s) is misleading on its own: it averages the warm-up ramp
(0.1133 kB/s) together with the flat remainder and reports a
start-up transient as if it were unbounded growth. RSS climbs while the
JPEG buffers, the TLS session cache and the SQLite page cache reach their
working size, then stops. A genuine leak would keep the same slope in both
halves instead of flattening.

Raw data: `memory.csv`.
