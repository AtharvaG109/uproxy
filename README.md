# uproxy

`uproxy` is a C++20 reverse proxy foundation with platform-specific event loops,
strict HTTP/1.1 parsing, HTTP/2 framing, HPACK table handling, upstream
selection, structured logs, and a cleartext HTTP/1.1 forwarding path.

## Status

This repository is not yet a production-ready full-spec reverse proxy. The
validated path is cleartext HTTP/1.1 client traffic forwarded to an HTTP/1.1
upstream. TLS termination, HPACK Huffman coding, complete HTTP/2 stream-state
handling, and pooled upstream reuse remain release blockers before this should
be exposed to untrusted production traffic.

Current local validation:

- `make test`
- `make`
- `clang-format --dry-run --Werror`
- CMake Debug build and `ctest`
- CMake ASan/UBSan build and `ctest`
- CMake Release build and `ctest`

## Build

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
ctest --test-dir build --output-on-failure
```

On systems without CMake/Ninja installed:

```bash
make test
make
```

## Architecture Notes

See `docs/architecture.md` for the validated request path, release boundaries, and proof gates.

## Run

```bash
./build/uproxy --config uproxy.toml.example
```

For the validated cleartext path:

```bash
python3 -m http.server 3000 --bind 127.0.0.1
./build/uproxy --no-tls --listen-port 8080 --upstream 127.0.0.1:3000
curl -i http://127.0.0.1:8080/
```

## Benchmarks

Benchmarks must be recorded from a release build with a real upstream:

```bash
python3 -m http.server 3000
./build-release/uproxy --upstream 127.0.0.1:3000
wrk -t4 -c100 -d30s http://localhost:8080/
```

Target envelope:

| Metric | Target |
| --- | --- |
| Throughput, small responses | > 50,000 req/sec |
| p50 added latency | < 0.5 ms |
| p99 latency | < 5 ms |
| Memory per connection | < 256 KB |
| CPU overhead vs direct | < 5% |
