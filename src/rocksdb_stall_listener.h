#ifndef ROCKSDB_STALL_LISTENER_H
#define ROCKSDB_STALL_LISTENER_H

#include <rocksdb/listener.h>
#include <atomic>
#include <chrono>

namespace adoc {

struct RealStallStats {
    std::atomic<long> delayed_count{0};
    std::atomic<long> stopped_count{0};
    std::atomic<long> stalled_ms{0};
    std::atomic<long> flushes{0};
    std::atomic<long> compactions{0};
};

class AdocStallListener : public rocksdb::EventListener {
public:
    void OnStallConditionsChanged(const rocksdb::WriteStallInfo& info) override;
    void OnFlushCompleted(rocksdb::DB*, const rocksdb::FlushJobInfo&) override;
    void OnCompactionCompleted(rocksdb::DB*, const rocksdb::CompactionJobInfo&) override;

    const RealStallStats& stats() const { return stats_; }

private:
    RealStallStats stats_;

    std::chrono::steady_clock::time_point stall_start_{};
    bool in_stall_ = false;
};

}

#endif
