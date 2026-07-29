# Experiment 1-4 -- http to https redirect

Run: 2026-07-27T04:25:58Z

```
$ curl -i http://192.168.100.26/
HTTP/1.1 301 Moved Permanently
HTTP/1.1 301 Moved Permanently
Date: Mon, 27 Jul 2026 04:25:58 GMT
Date: Mon, 27 Jul 2026 04:25:58 GMT
Location: https://192.168.100.26/
Location: https://192.168.100.26/
Content-Type: text/html; charset=utf-8
Content-Type: text/html; charset=utf-8
```

Status **301**, `Location: https://192.168.100.26/`. Following it lands on `200 https://192.168.100.26/`.

301 (permanent) rather than 302 is deliberate: it tells the browser to stop
issuing the cleartext request at all on later visits, so the plaintext port is
used exactly once per client.

For the browser screenshot the brief asks for, open `http://192.168.100.26/` with
devtools on the Network tab and photograph the 301 entry.
