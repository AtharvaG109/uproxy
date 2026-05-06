#include "uproxy/result.h"

#include "test_util.h"

using namespace uproxy;

namespace {

Result<int> double_positive(int x) {
    if (x < 0) {
        return Result<int>::err(Error::from_code(ErrCode::ConfigInvalid, "negative"));
    }
    return Result<int>::ok(x * 2);
}

void test_result() {
    check(Result<int>::ok(42).value() == 42);
    check(Result<int>::err(Error::from_code(ErrCode::Timeout)).is_err());
    check(Result<int>::ok(5).map([](int x) { return x * 2; }).value() == 10);
    check(Result<int>::ok(5).and_then(double_positive).value() == 10);
    check(Result<int>::ok(-1).and_then(double_positive).is_err());
}

} // namespace

int main_result_tests() {
    test_result();
    return 0;
}
