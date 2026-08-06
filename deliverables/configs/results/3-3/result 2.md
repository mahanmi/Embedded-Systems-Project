# Experiment 3-3 -- detector input resolution sweep

Run: 2026-08-06T22:15:55Z. 300s per level after a 30 s settling period.

| net input | mean FPS | mean inference | temp at 5 min | daemon RSS | detection rate | mean peak conf |
|---|---|---|---|---|---|---|
| 300 px | 6.71 | 1444 ms | 69.83 C | 23628 kB | 100% | 1.00 |
| 224 px | 6.67 | 859 ms | 67.68 C | 23628 kB | 100% | 0.92 |
| 160 px | 6.66 | 466 ms | 67.14 C | 24504 kB | 100% | 0.94 |

A person was in frame for the whole sweep, so the detection-rate and
confidence columns are comparable across the three levels. They remain a
*relative* indicator, not an absolute accuracy: there is no labelled ground
truth here, only the same scene measured three times.

Inference cost scales with the square of the input edge, so 224 px is
roughly 56% of the work of 300 px and 160 px about
28%. The measured inference times above are what to check that
prediction against -- they include fixed per-frame costs (JPEG decode, resize,
overlay) that do not shrink, so the speed-up is always less than the ratio.

Raw data: `sweep.csv`.
