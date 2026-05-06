#include <cstdio>

int main_result_tests();
int main_buffer_tests();
int main_http1_tests();
int main_http2_tests();
int main_hpack_tests();
int main_config_tests();
int main_load_balancer_tests();
int main_log_tests();

int main() {
    main_result_tests();
    main_buffer_tests();
    main_http1_tests();
    main_http2_tests();
    main_hpack_tests();
    main_config_tests();
    main_load_balancer_tests();
    main_log_tests();
    std::puts("uproxy tests passed");
    return 0;
}
