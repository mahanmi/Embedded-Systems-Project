# Experiment 2-1 -- CPU temperature in three states

Run: 2026-08-06T19:30:12Z. Sampling every 30s for 300s per state.

| state | temp min | temp max | temp mean | CPU% mean | fps mean |
|---|---|---|---|---|---|
| idle | 59.07 | 60.69 | 59.85 | 1.43 | 0.00 |
| stream_only | 60.15 | 60.69 | 60.39 | 8.12 | 6.29 |
| stream_detect | 61.22 | 62.84 | 62.30 | 29.29 | 6.45 |

Throttle bitmask observed: `0xd0000 0xd0005 0xd0008 `

Bit 0 of that mask is under-voltage and bit 2 is active throttling, so
these temperatures were recorded while the SoC was already being capped by
an inadequate supply. The *relative* ordering of the three states is still
meaningful; the absolute ceiling would be higher on a compliant PSU.

Raw data: `thermal.csv`.
