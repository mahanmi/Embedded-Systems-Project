# Experiment 3-4 -- LWT and a broker outage

Run: 2026-08-06T22:24:16Z

## Part 1 -- the will actually fires

The will is published *by the broker*, on behalf of a client that disappeared
without a clean DISCONNECT. Killing the broker cannot demonstrate it, because
then nothing is left to publish it. So the daemon was killed with SIGKILL while
the broker watched, and the retained will appeared on
`status/402170516/home`:

```
status/402170516/home {"status":"offline","student_id":"402170516","reason":"unexpected disconnect (LWT)"}
```

It arrives one keepalive interval (`mqtt_keepalive = 20`) after the process
dies -- that is how long the broker waits before declaring the client gone.
`Restart=always` then brings the daemon back and it republishes a retained
`"online"`, so a subscriber joining later sees the current truth either way.

## Part 2 -- three minutes without a broker

With the broker stopped, the daemon reports `mqtt_connected=false`, keeps
retrying with backoff, and **the web server, API and detector keep working**
(HTTP 200 throughout). When the broker came back the client reconnected by
itself with no intervention.

Transcript: `cmd.log`, captured will: `lwt_capture.txt`.
