#include "rocksdb_stall_listener.h"

namespace adoc {

void AdocStallListener::OnStallConditionsChanged(const rocksdb::WriteStallInfo& info) {
    using WSC = rocksdb::WriteStallCondition;
    const WSC cur = info.condition.cur;
    const bool now_stalled = (cur == WSC::kDelayed) || (cur == WSC::kStopped);
    const auto now = std::chrono::steady_clock::now();

    if (now_stalled && !in_stall_) {

        in_stall_ = true;
        stall_start_ = now;
        if (cur == WSC::kStopped) stats_.stopped_count.fetch_add(1);
        else                      stats_.delayed_count.fetch_add(1);
    } else if (!now_stalled && in_stall_) {

        in_stall_ = false;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - stall_start_).count();
        stats_.stalled_ms.fetch_add(static_cast<long>(ms));
    } else if (now_stalled && in_stall_ && cur == WSC::kStopped) {

        stats_.stopped_count.fetch_add(1);
    }
}

void AdocStallListener::OnFlushCompleted(rocksdb::DB*, const rocksdb::FlushJobInfo&) {
    stats_.flushes.fetch_add(1);
}

void AdocStallListener::OnCompactionCompleted(rocksdb::DB*, const rocksdb::CompactionJobInfo&) {
    stats_.compactions.fetch_add(1);
}

}
