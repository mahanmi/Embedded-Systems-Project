# Experiment 2-3 -- 50 concurrent requests to /api/v1/telemetry

Run: 2026-07-27T15:26:44Z.

| | |
|---|---|
| requests / HTTP 200 | 50 / 50 |
| baseline latency (unloaded) | 0.098686s |
| min / mean / max under load | 0.278s / 0.994s / 1.541s |
| p95 | 1.467s |
| slowdown vs baseline | 10.1x |
| CPU temperature | 65.53 C -> 65.53 C |

Latency rises by about 10.1x, and no request is dropped or refused.
The daemon serves each connection from a small thread pool, so 50 arrivals
queue rather than fan out into 50 threads -- which is why the tail grows
while the error count stays at zero. Each request also re-reads /proc and
/sys, so the work is genuinely repeated per call and not served from cache.

Raw timings: `latencies.txt` (http_code, total, connect, starttransfer).
