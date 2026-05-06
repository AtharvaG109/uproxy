add_executable(uproxy_tests
    main.cpp
    test_result.cpp
    test_buffer.cpp
    test_http1_parser.cpp
    test_http2_framing.cpp
    test_hpack.cpp
    test_config.cpp
    test_load_balancer.cpp
    test_log.cpp
)
target_link_libraries(uproxy_tests PRIVATE uproxy_core)
add_test(NAME uproxy_tests COMMAND uproxy_tests)
