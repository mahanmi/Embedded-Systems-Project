# Experiment 3-3 -- detector input resolution sweep

Run: 2026-07-27T16:00:58Z. 300s per level after a 30 s settling period.

| net input | mean FPS | mean inference | temp at 5 min | daemon RSS | detection rate | mean peak conf |
|---|---|---|---|---|---|---|
| 300 px | 7.32 | 1374 ms | 64.99 C | 22464 kB | 100% | 0.95 |
| 224 px | 6.97 | 812 ms | 65.53 C | 22903 kB | 97% | 0.96 |
| 160 px | 6.41 | 452 ms | 62.30 C | 23127 kB | 100% | 0.91 |

**The accuracy columns are not usable for this run** -- nobody was asserted to
be in frame, so a 0% detection rate means only that the room was empty. Re-run
with `--person` while standing in view to populate them. FPS, inference time,
temperature and memory above are unaffected and remain valid.

Inference cost scales with the square of the input edge, so 224 px is
roughly 56% of the work of 300 px and 160 px about
28%. The measured inference times above are what to check that
prediction against -- they include fixed per-frame costs (JPEG decode, resize,
overlay) that do not shrink, so the speed-up is always less than the ratio.

Raw data: `sweep.csv`.
