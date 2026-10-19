#include "workload.h"
#include <algorithm>

namespace adoc {

Workload::Workload(WorkloadConfig cfg) : cfg_(cfg) {}

double Workload::input_bytes(int window, double tw) const {
    double rate = cfg_.input_rate;
    if (cfg_.warmup > 0 && window < cfg_.warmup) {

        double frac = (double)(window + 1) / (double)cfg_.warmup;
        rate = cfg_.input_rate * std::min(1.0, frac);
    }
    return rate * tw;
}

}
