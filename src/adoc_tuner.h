#pragma once
// adoc_tuner.h
// ─────────────────────────────────────────────────────────────────────────────
// ADOC core logic: Overflow Detector + AIMD Decision Logic + Dynamic Adjuster.
//
// Paper §4 describes three overflow types and three AIMD responses:
//
//   MMO (Memtable Overflow)  → decrease threads, increase batch size
//   L0O (Level-0 Overflow)   → increase threads (batch unchanged)
//   RDO (RDB / compaction)   → increase threads, decrease batch size
//
// Priority when multiple overflows occur in one Tw window: L0O > RDO > MMO.
//
// AIMD parameters (additive increase / multiplicative decrease):
//   threads:   +2 (AI), ×0.5 (MD), clamped to [min_threads, max_threads]
//   batch_mb:  +64 MB (AI), ×0.5 (MD), clamped to [min_batch_mb, max_batch_mb]
//
// The tuner runs a background thread that samples every Tw seconds (matching
// the MetricsCollector's interval) and calls db->SetDBOptions / SetOptions.
// ─────────────────────────────────────────────────────────────────────────────

#include "metrics_collector.h"
#include "stall_listener.h"

#include <rocksdb/db.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace adoc {

// ── Overflow type enum ────────────────────────────────────────────────────────
enum class OverflowType { None, MMO, L0O, RDO };

inline const char* OverflowName(OverflowType t) {
    switch (t) {
        case OverflowType::MMO: return "MMO";
        case OverflowType::L0O: return "L0O";
        case OverflowType::RDO: return "RDO";
        default:                return "none";
    }
}

// ── Tuner configuration ───────────────────────────────────────────────────────
struct TunerConfig {
    // AIMD clamp ranges
    int    min_threads{2};
    int    max_threads{20};
    double min_batch_mb{64.0};
    double max_batch_mb{512.0};

    // AIMD step sizes
    int    thread_step{2};     // additive increase for threads
    double batch_step_mb{64.0}; // additive increase for batch size

    // Overflow thresholds – "laptop" profile defaults
    // These match paper §5 scaled for a single machine with <50GB ingest.
    uint64_t l0_overflow_files{10};                // L0O: L0 file count
    uint64_t rdo_soft_bytes{50ULL*1024*1024};      // RDO soft: 50 MB (laptop)
    uint64_t rdo_hard_bytes{100ULL*1024*1024};     // RDO hard: 100 MB (laptop)
    uint64_t mmo_imm_threshold{1};                 // MMO: >=1 immutable MT

    // Paper profile thresholds (overrides above when --profile paper)
    // l0_overflow_files = 20, rdo_soft = 64GB, rdo_hard = 128GB

    // Sampling interval (should match MetricsCollector)
    int sample_interval_ms{1000};

    // Initial values (set from CLI or defaults)
    int    initial_threads{4};
    double initial_batch_mb{64.0};
};

// ── Per-action log entry ──────────────────────────────────────────────────────
struct TunerAction {
    double      elapsed_s{0.0};
    OverflowType overflow_type{OverflowType::None};
    int         old_threads{0};
    int         new_threads{0};
    double      old_batch_mb{0.0};
    double      new_batch_mb{0.0};
    std::string description;
};

// ── Tuner class ───────────────────────────────────────────────────────────────
class AdocTuner {
public:
    AdocTuner(rocksdb::DB* db,
              MetricsCollector* collector,
              StallListener*    listener,
              const TunerConfig& cfg);
    ~AdocTuner();

    void Start(std::chrono::steady_clock::time_point run_start);
    void Stop();

    // Summary accessors
    std::vector<TunerAction> Actions() const;
    int  CurrentThreads() const;
    double CurrentBatchMb() const;

    // Action count by type
    int CountActions(OverflowType t) const;

    // Detailed stall tracking (tuner's own records, per type)
    double TotalStallS(const std::string& type) const; // "l0","rdo","mmo","all"
    uint64_t StallCount(const std::string& type) const;

private:
    void TuningLoop();

    // Detect highest-priority overflow this window
    OverflowType DetectOverflow(const MetricSnapshot& snap) const;

    // Apply AIMD action and return a description string
    std::string ApplyAction(OverflowType ov);

    // Apply params to RocksDB at runtime
    rocksdb::Status ApplyThreads(int n);
    rocksdb::Status ApplyBatchMb(double mb);

    rocksdb::DB*       db_;
    MetricsCollector*  collector_;
    // listener_ kept for future direct queries; currently stall state is
    // delivered via collector_->SetStallState() callback wired in main.cpp.
    [[maybe_unused]] StallListener* listener_;
    TunerConfig        cfg_;

    std::atomic<int>     cur_threads_;
    // std::atomic<double> is not guaranteed lock-free in C++17 on all archs.
    // We use a plain double guarded by batch_mu_ instead.
    double               cur_batch_mb_val_;
    mutable std::mutex   batch_mu_;

    std::chrono::steady_clock::time_point run_start_;

    std::thread       thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex           actions_mu_;
    std::vector<TunerAction>     actions_;

    // Per-type stall durations tracked inside the tuner loop
    // (stall is "on" while the last snapshot had stall_state != "none")
    std::string  prev_stall_{"none"};
    std::chrono::steady_clock::time_point stall_start_;
    double stall_dur_l0_{0.0}, stall_dur_rdo_{0.0},
           stall_dur_mmo_{0.0}, stall_dur_total_{0.0};
    uint64_t stall_cnt_l0_{0}, stall_cnt_rdo_{0},
             stall_cnt_mmo_{0}, stall_cnt_total_{0};
    mutable std::mutex stall_stat_mu_;
};

} // namespace adoc
