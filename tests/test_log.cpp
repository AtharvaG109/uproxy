#include "uproxy/log.h"

#include <cassert>

using namespace uproxy;

int main_log_tests() {
    assert(parse_log_level("debug") == LogLevel::Debug);
    assert(log_level_name(LogLevel::Warn) == "warn");
    return 0;
}
