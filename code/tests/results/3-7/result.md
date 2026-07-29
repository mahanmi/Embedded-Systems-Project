# Experiment 3-7 -- unauthorised SSH access

Policy in force (`sshd -T`): `PasswordAuthentication no`,
`PermitRootLogin no`, `AllowUsers mahan`, `MaxAuthTries 3`.
Run: 2026-07-27T04:22:52Z

3 of 3 unauthorised attempts were refused.

All three fail with `Permission denied (publickey).` -- sshd never offers a
password prompt, so there is nothing to brute-force. `cmd.log` pairs each
client-side refusal with the matching sshd journal entry.

Control: the authorised key logged in during the same run, so the refusals are
policy, not an unreachable board.

Raw transcript: `cmd.log`.
