# Experiment 2-3 -- 50 concurrent requests to /api/v1/telemetry

Run: 2026-08-06T19:08:02Z.

| | |
|---|---|
| requests / HTTP 200 | 50 / 50 |
| baseline latency (unloaded) | 0.090481s |
| min / mean / max under load | 0.345s / 0.818s / 1.307s |
| p95 | 1.233s |
| slowdown vs baseline | 9.0x |
| CPU temperature | 62.84 C -> 63.38 C |

Latency rises by about 9.0x, and no request is dropped or refused.
The daemon serves each connection from a small thread pool, so 50 arrivals
queue rather than fan out into 50 threads -- which is why the tail grows
while the error count stays at zero. Each request also re-reads /proc and
/sys, so the work is genuinely repeated per call and not served from cache.

Raw timings: `latencies.txt` (http_code, total, connect, starttransfer).
