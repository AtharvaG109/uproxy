#include "uproxy/config.h"

#include <cassert>

using namespace uproxy;

int main_config_tests() {
    ProxyConfig cfg;
    cfg.tls.enabled = false;
    cfg.upstreams.push_back({"backend", "127.0.0.1", 3000, 1, true});
    assert(config_validate(std::move(cfg)).is_ok());

    ProxyConfig bad;
    bad.tls.enabled = false;
    assert(config_validate(std::move(bad)).is_err());
    return 0;
}
