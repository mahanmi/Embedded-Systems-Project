# Experiment 3-6 -- unauthorised MQTT access

Broker: mosquitto on the laptop, `allow_anonymous false` + password file + ACL.
Run: 2026-07-27T04:22:42Z

4 of 4 unauthorised attempts were refused.

Every attempt fails at CONNECT with `Connection Refused: not authorised.`,
except the ACL case, where the connection is allowed but the publish is
discarded because `guardian.acl` grants the viewer account read-only access.

Control: the board's own authorised client stayed connected for the whole run,
so the refusals are the broker rejecting bad credentials, not the broker being
unavailable.

Raw transcript: `cmd.log`.
