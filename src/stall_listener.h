#pragma once
// stall_listener.h
// ─────────────────────────────────────────────────────────────────────────────
// RocksDB EventListener that tracks write-stall events.
//
// Paper §3: the three overflow types are detected from stall conditions:
//   MMO (Memtable overflow): WriteStallCondition::kDelayed or kStopped
//         while "rocksdb.num-immutable-mem-table" > 0
//   L0O  (Level-0 overflow): stall because of L0 file count
//   RDO  (RDB overflow): stall because of pending compaction bytes
//
// RocksDB calls OnStallConditionsChanged on the thread that changes the
// condition, which may be a background thread – so we only store an
// atomic tag and return immediately.
// ─────────────────────────────────────────────────────────────────────────────

#include <rocksdb/listener.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace adoc {

// ── Per-stall-event record ────────────────────────────────────────────────────
struct StallEvent {
    std::string condition; // "none", "delay_l0", "stop_l0",
                           // "delay_compaction", "stop_compaction",
                           // "delay_memtable", "stop_memtable", "unknown"
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    bool active{true};     // true while the stall is still ongoing
};

// ── Aggregated stall statistics ───────────────────────────────────────────────
struct StallStats {
    // Total stall duration in seconds (sum of all completed events)
    double total_stall_s{0.0};

    // Per-type counts and durations
    uint64_t count_mt{0};   // memtable / MMO
    uint64_t count_l0{0};   // L0 file count / L0O
    uint64_t count_ps{0};   // pending compaction / RDO

    double dur_mt_s{0.0};
    double dur_l0_s{0.0};
    double dur_ps_s{0.0};

    // Current (live) stall tag
    std::string current{"none"};
};

// ── EventListener subclass ────────────────────────────────────────────────────
class StallListener : public rocksdb::EventListener {
public:
    // Optional callback invoked on every stall state change.
    // Signature: (new_condition_string)
    using StateChangeCb = std::function<void(const std::string&)>;

    explicit StallListener(StateChangeCb cb = nullptr);

    // ── EventListener overrides ───────────────────────────────────────────────
    void OnStallConditionsChanged(
        const rocksdb::WriteStallInfo& info) override;

    // ── Query helpers (thread-safe) ───────────────────────────────────────────

    // Human-readable tag for the current stall state ("none" if not stalled)
    std::string CurrentState() const;

    // Full aggregated statistics computed from the event log
    StallStats GetStats() const;

    // All recorded events (copy)
    std::vector<StallEvent> Events() const;

private:
    // Converts RocksDB WriteStallCondition to our tag string
    static std::string ConditionToTag(rocksdb::WriteStallCondition c);

    mutable std::mutex        mu_;
    std::string               current_tag_{"none"};
    std::vector<StallEvent>   events_;
    StateChangeCb             cb_;
};

} // namespace adoc
