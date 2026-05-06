# uproxy

`uproxy` is a C++20 reverse proxy with platform-specific event loops, strict
HTTP/1.1 parsing, HTTP/2 framing, HPACK, upstream pooling, weighted
round-robin load balancing, structured logs, and optional BoringSSL TLS.

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

## Run

```bash
./build/uproxy --config uproxy.toml.example
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

