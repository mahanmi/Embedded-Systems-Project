# Experiment 2-1 -- CPU temperature in three states

Run: 2026-07-27T16:18:31Z. Sampling every 30s for 300s per state.

| state | temp min | temp max | temp mean | CPU% mean | fps mean |
|---|---|---|---|---|---|
| idle | 56.92 | 59.07 | 57.70 | 4.00 | 0.00 |
| stream_only | 56.38 | 58.00 | 57.16 | 10.68 | 4.80 |
| stream_detect | 60.15 | 63.38 | 61.58 | 43.17 | 7.49 |

Throttle bitmask observed: `0x50005 `

Bit 0 of that mask is under-voltage and bit 2 is active throttling, so
these temperatures were recorded while the SoC was already being capped by
an inadequate supply. The *relative* ordering of the three states is still
meaningful; the absolute ceiling would be higher on a compliant PSU.

Raw data: `thermal.csv`.
