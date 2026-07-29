# Experiment 1-2 -- SIGKILL the web server

Run: 2026-07-27T04:25:58Z

| | |
|---|---|
| PID before `kill -9` | 21711 |
| PID after restart | 22213 |
| systemd restart counter | 1 -> 2 |
| time to a new process | ~11s |

`kill -9` cannot be trapped, so the process dies without cleanup. systemd sees
the unit fail and applies `Restart=always` with `RestartSec`, which is why the
gap is a few seconds rather than instant. The restart counter incrementing is
what distinguishes a genuine restart from the process never having died.

The journal extract in `cmd.log` is the screenshot the brief asks for.
