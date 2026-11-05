# HTTP / SQLi latency — where the time goes, and what Pentest removes

Bench: `bench/http_speed.pen`, runner `bench/run_http_speed.sh` (N≥30 runs,
`taskset -c 0`, min/mean reported — the §9 methodology of
[docs/performance/optimization.md](../docs/performance/optimization.md)).
Lab: `examples/sqli_lab.py` on `http://127.0.0.1:8080`.

## Baseline numbers (this host, loopback lab)

| Measurement | Value | Meaning |
|---|---|---|
| first `http_get` | ~5.2 ms | one-time libcurl init + DNS + TCP connect (+TLS on https) |
| keep-alive `http_get` #2..6 | ~0.5–0.7 ms | **~10× faster** — the handshake is skipped |
| steady-state `http_get` | ~0.72 ms/call (200 calls) | warm persistent handle |
| steady-state `http_post` | ~0.91 ms/call (100 calls) | warm persistent handle |
| `url_encode` (40-char payload) | ~850 ns/call | one direct C call, O(n), one allocation |

## What removes latency from the language side

1. **One persistent libcurl handle per process** (`g_curl`). After the first
   request, every `http_get`/`http_post` reuses the TCP connection (and TLS
   session) — keep-alive, HTTP/2–3 via ALPN, TCP fast open. First call pays
   the handshake; all later calls skip it (that is the ~10× above).
2. **One direct C call per builtin.** No dispatch table, no dynamic lookup;
   builtins a program never calls emit nothing. `url_encode` is a single
   `cs_builtin_url_encode` call — sub-microsecond per call.
3. **Single growable response buffer.** Response bytes accumulate in one
   `cs_buf`; the result is built with one `cs_text_from_buf` — no per-chunk
   `cs_value` allocation.
4. **Native compile.** `clang -std=c11 -O3 -march=native -lcurl` — the loop
   around 200 requests is straight C, no interpreter.

## How to read a number

Per §9: claim an effect as real only at ≥ ~1.5×, min-of-N with interleaved
order and `taskset -c 0`; sub-10% deltas from ordinary builds are code-layout
artifacts until rebuilt with `-falign-functions=64`. Network numbers here are
loopback; real-target latency is dominated by the network, which is exactly
why the keep-alive handshake saving (first-call vs warm-call) is the part the
language controls.
