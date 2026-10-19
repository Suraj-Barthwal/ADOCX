#ifndef WORKLOAD_H
#define WORKLOAD_H

namespace adoc {

struct WorkloadConfig {
    const char* name   = "fillrandom";
    double input_rate  = 0.0;
    int    windows     = 200;
    int    warmup      = 0;
    int    key_bytes   = 16;
    int    value_bytes = 1000;
};

class Workload {
public:
    explicit Workload(WorkloadConfig cfg);

    double input_bytes(int window, double tw) const;

    const WorkloadConfig& config() const { return cfg_; }
    int windows() const { return cfg_.windows; }

private:
    WorkloadConfig cfg_;
};

}

#endif
