## Summary

- 

## Validation

- [ ] `make test`
- [ ] `make`
- [ ] `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && ninja -C build`
- [ ] `ctest --test-dir build --output-on-failure`

## Release Boundaries

- [ ] README still states the validated path accurately.
- [ ] TLS, HTTP/2, HPACK, and pooling claims are not overstated.
- [ ] Generated build artifacts and local credentials are not committed.
