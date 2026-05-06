cmake_minimum_required(VERSION 3.20)
project(uproxy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

set(UPROXY_COMPILE_OPTIONS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -fno-omit-frame-pointer
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND UPROXY_COMPILE_OPTIONS -O0 -g)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    list(APPEND UPROXY_COMPILE_OPTIONS -O2 -DNDEBUG)
endif()

set(UPROXY_LINK_OPTIONS)
if(ENABLE_ASAN)
    list(APPEND UPROXY_COMPILE_OPTIONS -fsanitize=address,undefined -g)
    list(APPEND UPROXY_LINK_OPTIONS -fsanitize=address,undefined)
endif()
if(ENABLE_TSAN)
    list(APPEND UPROXY_COMPILE_OPTIONS -fsanitize=thread -g)
    list(APPEND UPROXY_LINK_OPTIONS -fsanitize=thread)
endif()

function(uproxy_apply_options target_name)
    target_compile_options(${target_name} PRIVATE ${UPROXY_COMPILE_OPTIONS})
    if(UPROXY_LINK_OPTIONS)
        target_link_options(${target_name} PRIVATE ${UPROXY_LINK_OPTIONS})
    endif()
endfunction()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_SRCS src/event_loop_epoll.cpp)
    add_compile_definitions(UPROXY_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_SRCS src/event_loop_kqueue.cpp)
    add_compile_definitions(UPROXY_MACOS)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

set(UPROXY_SRCS
    src/result.cpp
    src/fd.cpp
    src/buffer.cpp
    src/config.cpp
    src/http1.cpp
    src/http2.cpp
    src/hpack.cpp
    src/tls.cpp
    src/upstream_pool.cpp
    src/load_balancer.cpp
    src/proxy.cpp
    src/log.cpp
    ${PLATFORM_SRCS}
)

add_library(uproxy_core STATIC ${UPROXY_SRCS})
target_include_directories(uproxy_core PUBLIC include)
uproxy_apply_options(uproxy_core)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boringssl/CMakeLists.txt")
    add_subdirectory(third_party/boringssl)
    target_include_directories(uproxy_core PRIVATE third_party/boringssl/include)
    target_link_libraries(uproxy_core PUBLIC ssl crypto)
    target_compile_definitions(uproxy_core PUBLIC UPROXY_HAS_BORINGSSL=1)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlplusplus/CMakeLists.txt")
    add_subdirectory(third_party/tomlplusplus)
    target_link_libraries(uproxy_core PUBLIC tomlplusplus::tomlplusplus)
    target_compile_definitions(uproxy_core PUBLIC UPROXY_HAS_TOMLPLUSPLUS=1)
endif()

add_executable(uproxy src/main.cpp)
target_link_libraries(uproxy PRIVATE uproxy_core)
uproxy_apply_options(uproxy)

option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
