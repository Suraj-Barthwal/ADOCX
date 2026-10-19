#ifndef STALL_LISTENER_H
#define STALL_LISTENER_H

#include "adoc_tuner.h"

namespace adoc {

enum class StallType { None, Memtable, Level0, PendingSize };
const char* to_string(StallType s);

struct StallThresholds {
    int      l0_slowdown  = 20;
    uint64_t pending_bytes = 64ULL * GB;
};

struct StallStats {
    int mt_count = 0, l0_count = 0, ps_count = 0;
    double mt_sec = 0.0, l0_sec = 0.0, ps_sec = 0.0;

    int    total_count() const { return mt_count + l0_count + ps_count; }
    double total_sec() const { return mt_sec + l0_sec + ps_sec; }
};

class StallListener {
public:
    explicit StallListener(StallThresholds t = StallThresholds());

    StallType on_window(const SystemState& s, double tw);

    const StallStats& stats() const { return stats_; }

private:
    StallType classify(const SystemState& s) const;

    StallThresholds t_;
    StallStats      stats_;
    StallType       prev_ = StallType::None;
};

}

#endif
