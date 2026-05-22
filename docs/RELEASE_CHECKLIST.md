# Release Checklist

Use this checklist before publishing uproxy changes or updating readiness claims.

## Local Gates

```bash
make test
make
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
ctest --test-dir build --output-on-failure
```

When sanitizer tooling is available:

```bash
cmake -B build-asan -G Ninja -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
ninja -C build-asan
ctest --test-dir build-asan --output-on-failure
```

## Boundary Review

- The validated production claim remains cleartext HTTP/1.1 forwarding only.
- TLS termination, HPACK Huffman, complete HTTP/2 stream state, and pooled upstream reuse remain documented as blockers until fully validated.
- Generated build directories, binaries, credentials, and benchmark output are not committed.

## GitHub Readiness

- CI passes on Linux and macOS.
- Submodules are initialized and tracked intentionally.
- README and `docs/architecture.md` agree on release boundaries.
