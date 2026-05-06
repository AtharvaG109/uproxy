CXX ?= clang++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O0 -g -Iinclude
COMMON_SRCS = src/result.cpp src/fd.cpp src/buffer.cpp src/config.cpp src/http1.cpp src/http2.cpp src/hpack.cpp src/tls.cpp src/upstream_pool.cpp src/load_balancer.cpp src/proxy.cpp src/log.cpp
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	PLATFORM_SRCS = src/event_loop_kqueue.cpp
else
	PLATFORM_SRCS = src/event_loop_epoll.cpp
endif

.PHONY: all test clean

all: build/uproxy

build:
	mkdir -p build tests

build/uproxy: build $(COMMON_SRCS) $(PLATFORM_SRCS) src/main.cpp
	$(CXX) $(CXXFLAGS) $(COMMON_SRCS) $(PLATFORM_SRCS) src/main.cpp -o $@

tests/run_tests: build $(COMMON_SRCS) $(PLATFORM_SRCS) tests/*.cpp
	$(CXX) $(CXXFLAGS) $(COMMON_SRCS) $(PLATFORM_SRCS) tests/*.cpp -o $@

test: tests/run_tests
	./tests/run_tests

clean:
	rm -rf build tests/run_tests

