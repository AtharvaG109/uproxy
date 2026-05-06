#pragma once

#include <cstdlib>

inline void check(bool condition) {
    if (!condition) {
        std::abort();
    }
}
