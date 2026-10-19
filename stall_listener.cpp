#include "stall_listener.h"

namespace adoc {

const char* to_string(StallType s) {
    switch (s) {
        case StallType::Memtable:    return "MT-stall";
        case StallType::Level0:      return "L0-stall";
        case StallType::PendingSize: return "PS-stall";
        default:                     return "None";
    }
}

StallListener::StallListener(StallThresholds t) : t_(t) {}

StallType StallListener::classify(const SystemState& s) const {
    if (s.active_memtable_full_before_flush)        return StallType::Memtable;
    if (s.num_l0_files > t_.l0_slowdown)            return StallType::Level0;
    if (s.redundant_data_bytes > t_.pending_bytes)  return StallType::PendingSize;
    return StallType::None;
}

StallType StallListener::on_window(const SystemState& s, double tw) {
    StallType st = classify(s);
    const bool rising = (st != StallType::None) && (st != prev_);

    switch (st) {
        case StallType::Memtable:
            if (rising) { ++stats_.mt_count; }
            stats_.mt_sec += tw;
            break;
        case StallType::Level0:
            if (rising) { ++stats_.l0_count; }
            stats_.l0_sec += tw;
            break;
        case StallType::PendingSize:
            if (rising) { ++stats_.ps_count; }
            stats_.ps_sec += tw;
            break;
        default:
            break;
    }
    prev_ = st;
    return st;
}

}
