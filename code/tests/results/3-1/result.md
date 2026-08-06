# Experiment 3-1 -- detection accuracy by lighting condition

Updated: 2026-08-06T21:21:41Z. Each row is one run; ground truth asserted at the
command line, samples taken every 3 s from `/api/v1/persons`.

| condition | ground truth | correct / total | accuracy | mean peak confidence |
|---|---|---|---|---|
| daylight | present | 20 / 20 | 100% | 0.98 |
| artificial | present | 20 / 20 | 100% | 0.97 |
| lowlight | present | 6 / 20 | 30% | 0.76 |
| backlight | present | 20 / 20 | 100% | 0.91 |

Backlight is the condition that breaks it, and the reason is exposure, not
the model: the sensor meters for the bright background, the subject falls
into silhouette, and an SSD trained on well-exposed people has almost no
gradient left to key on. Low light degrades differently -- sensor noise
rises and the network's confidence sags rather than collapsing outright.

Raw samples: `lighting.csv`.
