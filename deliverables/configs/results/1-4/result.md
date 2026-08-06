# Experiment 1-4 -- http to https redirect

Run: 2026-08-06T19:48:06Z

```
$ curl -i http://mahan.local/
HTTP/1.1 301 Moved Permanently
HTTP/1.1 301 Moved Permanently
Date: Fri, 05 Jun 2026 16:23:38 GMT
Date: Fri, 05 Jun 2026 16:23:38 GMT
Location: https://mahan.local/
Location: https://mahan.local/
Content-Type: text/html; charset=utf-8
Content-Type: text/html; charset=utf-8
```

Status **301**, `Location: https://mahan.local/`. Following it lands on `200 https://mahan.local/`.

301 (permanent) rather than 302 is deliberate: it tells the browser to stop
issuing the cleartext request at all on later visits, so the plaintext port is
used exactly once per client.

For the browser screenshot the brief asks for, open `http://mahan.local/` with
devtools on the Network tab and photograph the 301 entry.
