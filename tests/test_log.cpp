#include "uproxy/log.h"

#include "test_util.h"

using namespace uproxy;

int main_log_tests() {
    check(parse_log_level("debug") == LogLevel::Debug);
    check(log_level_name(LogLevel::Warn) == "warn");
    return 0;
}
