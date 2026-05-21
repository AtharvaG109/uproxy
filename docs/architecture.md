# uproxy Architecture Notes

`uproxy` is currently a C++20 reverse-proxy foundation, not a production-ready full-spec proxy. The validated data path is cleartext HTTP/1.1 client traffic forwarded to an HTTP/1.1 upstream.

## Validated Path

```text
HTTP/1.1 client -> listener -> parser -> upstream selector -> HTTP/1.1 upstream
```

- The parser rejects malformed request lines and unsupported body modes with clear errors.
- The event-loop layer has platform-specific implementations for macOS `kqueue` and Linux `epoll`.
- The upstream path can be exercised locally with `python3 -m http.server`, `./build/uproxy --no-tls`, and `curl`.

## Release Boundaries

The following areas are intentionally treated as release blockers before exposing the proxy to untrusted production traffic:

- TLS termination validated end to end after handshake.
- HPACK Huffman decoding coverage beyond table behavior.
- Complete HTTP/2 stream-state handling under realistic request/response flows.
- Pooled upstream reuse with stale-connection recovery under load.

## Proof Gates

Use these gates before claiming a release-quality change:

```bash
make test
make
clang-format --dry-run --Werror src/*.cpp include/**/*.hpp tests/*.cpp
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
ctest --test-dir build --output-on-failure
```

Benchmarks should be recorded only from a release build and paired with the exact correctness gate that passed for the same revision.
