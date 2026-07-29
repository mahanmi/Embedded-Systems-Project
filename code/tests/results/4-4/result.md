# Experiment 4-4 -- adaptive thermal management

Run: 2026-07-27T08:27:12Z. Thresholds `thermal_high_c = 70.0`, `thermal_low_c = 65.0`.

Load was applied to all four cores for three minutes while the daemon's own
telemetry was sampled every 5 s.

| | |
|---|---|
| peak CPU temperature under full load | 68.76 C |
| highest thermal level at the configured 70.0 C setpoint | 0 |
| temporary setpoint (phase B) | 66.8 C up / 62.8 C down |
| highest thermal level at that setpoint | 3 |
| target_fps once engaged | 2 |
| target_net_input once engaged | 192 |

**The 70 C setpoint is unreachable on this board.** Four cores of stress-ng for
three minutes peaked at 68.76 C, because the under-voltage condition documented
in 2-1 caps the SoC before its own thermal limit becomes relevant. A control loop
whose setpoint the plant cannot reach cannot be verified by waiting, so the
setpoint was moved to the plant: thresholds were lowered to 66.8 C / 62.8 C,
the load re-applied, and the governor then behaved exactly as designed --
thermal level 3, frame rate cut to 2, network input 192 -- before the
original values were restored. What this demonstrates is the mechanism; what it
does not demonstrate is the board reaching 70 C, which it cannot on this supply.

The governor uses separate rise and fall thresholds (70.0 C up, 65.0 C down)
rather than one. With a single threshold the system would oscillate: the moment
it lowered the frame rate the temperature would drop below the line, it would
restore full rate, heat up again, and flap indefinitely -- changing resolution
several times a minute. The 5 C gap makes each
step commit.

Note that this board is already being throttled by an inadequate power supply
(under-voltage), which caps the SoC before the governor's own thresholds do --
so the temperatures here are lower than the silicon would otherwise reach.

Per-sample data: `thermal_response.csv`; governor log: `cmd.log`.
