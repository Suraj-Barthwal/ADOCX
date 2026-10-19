#ifndef METRICS_COLLECTOR_H
#define METRICS_COLLECTOR_H

#include "adoc_tuner.h"

namespace adoc {

struct EngineParams {
    double device_bw;
    double l0l1_bw_fraction = 0.90;
    double deep_efficiency  = 0.70;
    double redundancy_ratio = 0.40;
    int    max_threads      = 32;
};

struct Metrics {
    SystemState state;
    double buffered_bytes = 0.0;
    double cpu_percent    = 0.0;
    double disk_percent   = 0.0;
    double flush_rate     = 0.0;
    double l0l1_rate      = 0.0;
    double deep_rate      = 0.0;
};

class MetricsCollector {
public:
    explicit MetricsCollector(EngineParams p);

    Metrics collect(const TuningKnobs& knobs, double input_bytes, double tw);

private:
    static double flush_efficiency(uint64_t batch_size);

    EngineParams p_;
    double mem_   = 0.0;
    double l0_    = 0.0;
    double redun_ = 0.0;
};

}

#endif
