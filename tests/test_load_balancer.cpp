#include "uproxy/load_balancer.h"

#include <cassert>
#include <vector>

using namespace uproxy;

int main_load_balancer_tests() {
    std::vector<UpstreamConfig> cfgs{{"A", "127.0.0.1", 1, 5, true},
                                     {"B", "127.0.0.1", 2, 3, true},
                                     {"C", "127.0.0.1", 3, 2, true}};
    WeightedRoundRobin lb(std::move(cfgs));
    std::vector<std::string> seq;
    for (int i = 0; i < 10; ++i) {
        seq.push_back(lb.next()->name);
    }
    assert((seq == std::vector<std::string>{"A", "B", "C", "A", "A", "B", "A", "C", "B", "A"}));
    return 0;
}
