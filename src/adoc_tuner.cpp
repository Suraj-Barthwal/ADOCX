// adoc_tuner.cpp
// ─────────────────────────────────────────────────────────────────────────────
// ADOC Overflow Detector, AIMD Decision Logic, and Dynamic Parameter Adjuster.
//
// Paper reference: §4 "ADOC Design"
//   - §4.1 Monitoring  → MetricsCollector (Tw = 1s)
//   - §4.2 Detecting   → DetectOverflow()  – three overflow types, L0O > RDO > MMO
//   - §4.3 AIMD        → ApplyAction()     – additive increase / multiplicative decrease
//   - §4.4 Adjusting   → ApplyThreads() / ApplyBatchMb() – RocksDB public API
//
// All RocksDB knob changes use the PUBLIC API only:
//   db->SetDBOptions({"max_background_jobs": ...})   → thread count
//   db->SetOptions({"write_buffer_size": ...,        → batch size
//                   "target_file_size_base": ...})
// Both are documented as hot-changeable since RocksDB 5.x.
// ─────────────────────────────────────────────────────────────────────────────

#include "adoc_tuner.h"

#include <rocksdb/options.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>

namespace adoc {

// ── Constructor / Destructor ──────────────────────────────────────────────────

AdocTuner::AdocTuner(rocksdb::DB*       db,
                     MetricsCollector*  collector,
                     StallListener*     listener,
                     const TunerConfig& cfg)
    : db_(db), collector_(collector), listener_(listener), cfg_(cfg),
      cur_threads_(cfg.initial_threads),
      cur_batch_mb_val_(cfg.initial_batch_mb) {}

AdocTuner::~AdocTuner() { Stop(); }

// ── Start / Stop ──────────────────────────────────────────────────────────────

void AdocTuner::Start(std::chrono::steady_clock::time_point run_start) {
    run_start_ = run_start;
    running_.store(true);
    thread_ = std::thread(&AdocTuner::TuningLoop, this);
}

void AdocTuner::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

// ── Overflow detection (paper §4.2) ──────────────────────────────────────────
// Priority: L0O > RDO > MMO (same as the paper).

OverflowType AdocTuner::DetectOverflow(const MetricSnapshot& snap) const {
    // L0O: L0 file count at or above threshold
    if (snap.l0_files >= cfg_.l0_overflow_files)
        return OverflowType::L0O;

    // RDO: pending compaction bytes at or above soft threshold.
    // We also treat any active RocksDB stall (stall_state != "none") as an
    // RDO signal when pending bytes are non-zero, because RocksDB's own
    // soft-limit has already fired – meaning our threshold was set too high.
    uint64_t pending_bytes =
        snap.pending_compaction_mb * 1024ULL * 1024ULL;
    bool rocksdb_stalling = (snap.stall_state != "none" &&
                             snap.stall_state != "mmo_stall");
    if (pending_bytes >= cfg_.rdo_soft_bytes ||
        (rocksdb_stalling && snap.pending_compaction_mb > 0))
        return OverflowType::RDO;

    // MMO: active memtable is full while an immutable one is waiting to flush
    if (snap.immutable_memtables >= cfg_.mmo_imm_threshold && snap.flush_pending)
        return OverflowType::MMO;

    return OverflowType::None;
}

// ── AIMD action (paper §4.3) ──────────────────────────────────────────────────
// Returns a human-readable description of what changed.

std::string AdocTuner::ApplyAction(OverflowType ov) {
    int    old_t = cur_threads_.load();
    double old_b;
    { std::lock_guard<std::mutex> lk(batch_mu_); old_b = cur_batch_mb_val_; }
    int    new_t = old_t;
    double new_b = old_b;

    switch (ov) {
        case OverflowType::L0O:
            // Increase threads (additive), batch unchanged
            new_t = std::min(old_t + cfg_.thread_step, cfg_.max_threads);
            break;

        case OverflowType::RDO:
            // Increase threads (additive), decrease batch (multiplicative)
            new_t = std::min(old_t + cfg_.thread_step, cfg_.max_threads);
            new_b = std::max(old_b / 2.0, cfg_.min_batch_mb);
            break;

        case OverflowType::MMO:
            // Decrease threads (multiplicative), increase batch (additive)
            new_t = std::max(static_cast<int>(std::ceil(old_t / 2.0)),
                             cfg_.min_threads);
            new_b = std::min(old_b + cfg_.batch_step_mb, cfg_.max_batch_mb);
            break;

        default:
            return "-";
    }

    std::ostringstream desc;
    desc << OverflowName(ov);
    bool changed = false;

    if (new_t != old_t) {
        auto s = ApplyThreads(new_t);
        if (s.ok()) {
            cur_threads_.store(new_t);
            desc << " threads:" << old_t << "->" << new_t;
            changed = true;
        } else {
            desc << " threads_err:" << s.ToString();
        }
    }

    if (std::fabs(new_b - old_b) > 0.5) {
        auto s = ApplyBatchMb(new_b);
        if (s.ok()) {
            { std::lock_guard<std::mutex> lk(batch_mu_); cur_batch_mb_val_ = new_b; }
            desc << " batch:" << old_b << "->" << new_b << "MB";
            changed = true;
        } else {
            desc << " batch_err:" << s.ToString();
        }
    }

    if (!changed) desc << " (clamped)";

    // Keep flush threads at ~1/4 of total (paper §4.4 recommendation)
    // We achieve this by relying on RocksDB's internal split of
    // max_background_jobs into flush and compaction jobs.
    // No explicit flush-thread override needed here.

    return desc.str();
}

// ── Apply parameters to RocksDB at runtime ───────────────────────────────────

rocksdb::Status AdocTuner::ApplyThreads(int n) {
    // max_background_jobs controls the total pool (flush + compaction).
    // Dynamically changeable via SetDBOptions since RocksDB 5.x.
    rocksdb::Status s = db_->SetDBOptions(
        {{"max_background_jobs", std::to_string(n)}});
    if (!s.ok()) {
        std::cerr << "[ADOC] SetDBOptions(max_background_jobs=" << n
                  << ") failed: " << s.ToString() << "\n";
    }
    return s;
}

rocksdb::Status AdocTuner::ApplyBatchMb(double mb) {
    // write_buffer_size   = size of one individual memtable
    // target_file_size_base = target L1 SST size (ADOC batch size maps to both)
    uint64_t bytes = static_cast<uint64_t>(mb * 1024 * 1024);
    rocksdb::Status s = db_->SetOptions(
        {{"write_buffer_size",    std::to_string(bytes)},
         {"target_file_size_base", std::to_string(bytes)}});
    if (!s.ok()) {
        std::cerr << "[ADOC] SetOptions(write_buffer_size=" << bytes
                  << ") failed: " << s.ToString() << "\n";
    }
    return s;
}

// ── Background tuning loop ────────────────────────────────────────────────────

void AdocTuner::TuningLoop() {
    auto next_tick = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(cfg_.sample_interval_ms);

    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next_tick);
        next_tick += std::chrono::milliseconds(cfg_.sample_interval_ms);

        // Read the latest snapshot from the collector (non-blocking)
        MetricSnapshot snap = collector_->Latest();
        double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_start_).count();

        // ── Stall duration tracking ───────────────────────────────────────────
        // We track when stall begins and ends using the stall_state field.
        {
            std::lock_guard<std::mutex> lk(stall_stat_mu_);
            const std::string& cur_stall = snap.stall_state;

            if (prev_stall_ == "none" && cur_stall != "none") {
                // Stall starts
                stall_start_ = std::chrono::steady_clock::now();
            } else if (prev_stall_ != "none" && cur_stall == "none") {
                // Stall ends – attribute to type
                double dur = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - stall_start_).count();
                stall_dur_total_ += dur;
                stall_cnt_total_++;

                // Classify by the overflow type that was active at stall start
                // (we store the type in the stall_state tag set by the detector)
                if (prev_stall_.find("l0") != std::string::npos) {
                    stall_dur_l0_ += dur; stall_cnt_l0_++;
                } else if (prev_stall_.find("rdo") != std::string::npos ||
                           prev_stall_.find("ps") != std::string::npos) {
                    stall_dur_rdo_ += dur; stall_cnt_rdo_++;
                } else {
                    stall_dur_mmo_ += dur; stall_cnt_mmo_++;
                }
            }
            prev_stall_ = cur_stall;
        }

        // ── Overflow detection ────────────────────────────────────────────────
        OverflowType ov = DetectOverflow(snap);
        if (ov == OverflowType::None) continue;

        // Build a rich stall tag so metrics_collector shows the type
        std::string rich_tag;
        switch (ov) {
            case OverflowType::L0O: rich_tag = "l0_stall";  break;
            case OverflowType::RDO: rich_tag = "rdo_stall"; break;
            case OverflowType::MMO: rich_tag = "mmo_stall"; break;
            default: break;
        }
        collector_->SetStallState(rich_tag);

        // ── AIMD action ───────────────────────────────────────────────────────
        std::string desc = ApplyAction(ov);
        collector_->SetLastAction(desc);
        {
            double bm;
            { std::lock_guard<std::mutex> lk(batch_mu_); bm = cur_batch_mb_val_; }
            collector_->SetCurrentParams(cur_threads_.load(), bm);
        }

        // Log the action
        TunerAction act;
        act.elapsed_s      = elapsed_s;
        act.overflow_type  = ov;
        act.old_threads    = snap.cur_threads;
        act.new_threads    = cur_threads_.load();
        act.old_batch_mb   = snap.cur_batch_mb;
        { std::lock_guard<std::mutex> lk(batch_mu_); act.new_batch_mb = cur_batch_mb_val_; }
        act.description    = desc;

        {
            std::lock_guard<std::mutex> lk(actions_mu_);
            actions_.push_back(act);
        }

        std::cout << "[ADOC t=" << elapsed_s << "s] " << desc << "\n";
    }
}

// ── Accessors ─────────────────────────────────────────────────────────────────

std::vector<TunerAction> AdocTuner::Actions() const {
    std::lock_guard<std::mutex> lk(actions_mu_);
    return actions_;
}

int AdocTuner::CurrentThreads() const {
    return cur_threads_.load();
}

double AdocTuner::CurrentBatchMb() const {
    std::lock_guard<std::mutex> lk(batch_mu_);
    return cur_batch_mb_val_;
}

int AdocTuner::CountActions(OverflowType t) const {
    std::lock_guard<std::mutex> lk(actions_mu_);
    int c = 0;
    for (const auto& a : actions_)
        if (a.overflow_type == t) c++;
    return c;
}

double AdocTuner::TotalStallS(const std::string& type) const {
    std::lock_guard<std::mutex> lk(stall_stat_mu_);
    if (type == "l0")  return stall_dur_l0_;
    if (type == "rdo") return stall_dur_rdo_;
    if (type == "mmo") return stall_dur_mmo_;
    return stall_dur_total_;
}

uint64_t AdocTuner::StallCount(const std::string& type) const {
    std::lock_guard<std::mutex> lk(stall_stat_mu_);
    if (type == "l0")  return stall_cnt_l0_;
    if (type == "rdo") return stall_cnt_rdo_;
    if (type == "mmo") return stall_cnt_mmo_;
    return stall_cnt_total_;
}

} // namespace adoc
