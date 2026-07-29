# Experiment 1-5 -- the self-signed certificate

Run: 2026-07-27T04:26:02Z

```
subject=CN=402170516, O=Sharif University of Technology, OU=Department of Electrical Engineering, L=Tehran, C=IR
issuer=CN=402170516, O=Sharif University of Technology, OU=Department of Electrical Engineering, L=Tehran, C=IR
notBefore=Jul 27 01:48:07 2026 GMT
notAfter=Oct 29 01:48:07 2028 GMT
SAN: IP Address:192.168.100.26, IP Address:127.0.0.1, DNS:mahan, DNS:mahan.local, DNS:localhost
```

**CN = 402170516**, which is the student number the brief requires.

Subject and issuer are identical, which is what makes it self-signed and why a
browser shows a warning. The SAN entry carries the board's address: modern
browsers ignore CN for hostname matching and read SAN only, so the certificate
needs both -- CN to satisfy the brief, SAN to be usable.

The PEM itself is saved as `guardian.crt`; screenshot the padlock dialog for
the report.
