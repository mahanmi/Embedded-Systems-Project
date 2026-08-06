# Experiment 1-2 -- SIGKILL the web server

Run: 2026-08-06T19:48:06Z

| | |
|---|---|
| PID before `kill -9` | 4223 |
| PID after restart | 5874 |
| systemd restart counter | 0 -> 1 |
| time to a new process | ~10s |

`kill -9` cannot be trapped, so the process dies without cleanup. systemd sees
the unit fail and applies `Restart=always` with `RestartSec`, which is why the
gap is a few seconds rather than instant. The restart counter incrementing is
what distinguishes a genuine restart from the process never having died.

The journal extract in `cmd.log` is the screenshot the brief asks for.
