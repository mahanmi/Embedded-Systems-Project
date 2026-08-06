# Experiment 3-5 -- detection to MQTT latency

Run: 2026-08-06T22:47:04Z. 10 rising-edge events (an empty scene becoming occupied).

| | ms |
|---|---|
| mean | 1517 |
| standard deviation | 33 |
| min | 1465 |
| max | 1561 |

Measured from the timestamp the board stamped on the detection to the arrival of
the payload on the laptop, corrected for a measured clock offset of
`-5369066.594859s` (median of five round-trip estimates; last RTT 2907.3 ms). The
correction matters: the two ends keep independent clocks, and at this scale an
uncorrected NTP disagreement would be larger than the quantity being measured.

**Clock warning:** the board's clock is 5369067s off this laptop's -- not NTP jitter, an unsynced clock. Check `timedatectl status` on the board; every latency figure below is corrected by this offset and only as trustworthy as it is.

Only 0 -> N transitions are timed. Counting every `persons` message would time
"still two people in the room" as though someone had just walked in, and would
report a latency far lower than the truth.

This is the **publish path**: inference, JSON assembly, MQTT publish at QoS 1,
broker, and the hop back. It excludes the interval between the person physically
entering and the frame being inferred, which on this board can reach one
motion-gate period -- the honest total for a human walking in is this figure plus
that wait, and both are reported rather than folded together.

Raw samples: `latency.csv`.
