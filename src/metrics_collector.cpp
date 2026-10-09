// metrics_collector.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Implementation of the Metrics Collector Engine (paper §3, Tw = 1s window).
//
// Key design choices:
//  - All reads from RocksDB are non-blocking GetProperty calls; they never
//    stall the sampling thread.
//  - The sampling thread sleeps for cfg_.sample_interval_ms between rounds so
//    it never burns a core.
//  - Latency samples are bucketed into 7 fixed bins (sub-ms to >500ms) using
//    a spin-protected array – low overhead, no heap allocation per sample.
//  - CPU / RSS are read via getrusage (POSIX) and /proc/self/status (Linux).
// ─────────────────────────────────────────────────────────────────────────────

#include "metrics_collector.h"

#include <rocksdb/statistics.h>

#include <sys/resource.h>  // getrusage

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace adoc {

// ── helpers ───────────────────────────────────────────────────────────────────

static uint64_t GetUInt64Property(rocksdb::DB* db,
                                   const std::string& prop) {
    uint64_t v = 0;
    db->GetAggregatedIntProperty(prop, &v);
    return v;
}

// Returns peak resident set size in KB.
// POSIX: ru_maxrss is in bytes on macOS, kilobytes on Linux.
static uint64_t PeakRSSKB() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    // macOS: ru_maxrss is in bytes
    return static_cast<uint64_t>(ru.ru_maxrss) / 1024;
#else
    // Linux: ru_maxrss is in kilobytes
    return static_cast<uint64_t>(ru.ru_maxrss);
#endif
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

MetricsCollector::MetricsCollector(rocksdb::DB* db,
                                   std::shared_ptr<rocksdb::Statistics> stats,
                                   const CollectorConfig& cfg)
    : db_(db), stats_(stats), cfg_(cfg) {
    if (!cfg_.csv_path.empty()) {
        csv_file_.open(cfg_.csv_path, std::ios::out | std::ios::trunc);
        if (!csv_file_.is_open()) {
            std::cerr << "[MetricsCollector] WARNING: cannot open CSV file: "
                      << cfg_.csv_path << "\n";
        } else {
            WriteCSVHeader();
        }
    }
}

MetricsCollector::~MetricsCollector() {
    Stop();
    if (csv_file_.is_open()) csv_file_.close();
}

// ── Start / Stop ──────────────────────────────────────────────────────────────

void MetricsCollector::Start() {
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    thread_ = std::thread(&MetricsCollector::SamplingLoop, this);
}

void MetricsCollector::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

// ── External state setters (called from writer threads / tuner) ───────────────

void MetricsCollector::RecordOp(uint64_t count) {
    ops_done_.fetch_add(count, std::memory_order_relaxed);
}

void MetricsCollector::SetLastAction(const std::string& action) {
    std::lock_guard<std::mutex> lk(state_mu_);
    last_action_ = action;
}

void MetricsCollector::SetStallState(const std::string& state) {
    std::lock_guard<std::mutex> lk(state_mu_);
    stall_state_ = state;
}

void MetricsCollector::RecordLatency(uint64_t ns) {
    // Find the right bucket (branchless linear scan over 6 edges)
    int bucket = 6;
    for (int i = 0; i < 6; ++i) {
        if (ns < LAT_EDGES[i]) { bucket = i; break; }
    }
    std::lock_guard<std::mutex> lk(lat_mu_);
    lat_counts_[bucket]++;
}

void MetricsCollector::SetCurrentParams(int threads, double batch_mb) {
    std::lock_guard<std::mutex> lk(state_mu_);
    cur_threads_  = threads;
    cur_batch_mb_ = batch_mb;
}

// ── Sampling loop ─────────────────────────────────────────────────────────────

void MetricsCollector::SamplingLoop() {
    auto next_tick = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(cfg_.sample_interval_ms);

    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next_tick);
        next_tick += std::chrono::milliseconds(cfg_.sample_interval_ms);

        auto now     = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();

        // Per-window throughput: ops delta since last sample / interval_s
        uint64_t current_ops = ops_done_.load(std::memory_order_relaxed);
        uint64_t prev_ops    = prev_ops_.exchange(current_ops, std::memory_order_relaxed);
        double   interval_s  = cfg_.sample_interval_ms / 1000.0;
        double   tput_kops   = static_cast<double>(current_ops - prev_ops) /
                               interval_s / 1000.0;

        MetricSnapshot snap = DoCollect(elapsed, tput_kops);

        {
            std::lock_guard<std::mutex> lk(latest_mu_);
            latest_ = snap;
        }
        {
            std::lock_guard<std::mutex> lk(history_mu_);
            history_.push_back(snap);
        }
        if (csv_file_.is_open()) WriteCSVRow(snap);
    }
}

// ── Core snapshot collection ──────────────────────────────────────────────────

MetricSnapshot MetricsCollector::DoCollect(double elapsed_s,
                                            double throughput_kops) {
    MetricSnapshot s;
    s.elapsed_s      = elapsed_s;
    s.ops_done       = ops_done_.load(std::memory_order_relaxed);
    s.num_ops        = cfg_.num_ops;
    s.progress_pct   = (cfg_.num_ops > 0)
                       ? (100.0 * s.ops_done / cfg_.num_ops)
                       : 0.0;
    s.throughput_kops = throughput_kops;

    // RocksDB properties ─────────────────────────────────────────────────────
    s.l0_files = GetUInt64Property(db_, "rocksdb.num-files-at-level0");
    {
        uint64_t pending_bytes = GetUInt64Property(
            db_, "rocksdb.estimate-pending-compaction-bytes");
        s.pending_compaction_mb = pending_bytes / (1024 * 1024);
    }
    s.immutable_memtables = GetUInt64Property(
        db_, "rocksdb.num-immutable-mem-table");
    {
        uint64_t fp = 0;
        db_->GetAggregatedIntProperty("rocksdb.mem-table-flush-pending", &fp);
        s.flush_pending = (fp != 0);
    }

    // Stats tickers for write-amplification ──────────────────────────────────
    if (stats_) {
        s.flush_bytes   = stats_->getTickerCount(rocksdb::FLUSH_WRITE_BYTES);
        s.compact_bytes = stats_->getTickerCount(rocksdb::COMPACT_WRITE_BYTES);
    }
    s.user_bytes_ingested =
        s.ops_done * (cfg_.key_size + cfg_.value_size);

    // Dynamic params ──────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        s.stall_state  = stall_state_;
        s.last_action  = last_action_;
        s.cur_threads  = cur_threads_;
        s.cur_batch_mb = cur_batch_mb_;
    }

    // Latency histogram ───────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(lat_mu_);
        s.lat_buckets = lat_counts_;
    }

    // CPU / RSS ───────────────────────────────────────────────────────────────
    {
        struct rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
        s.cpu_user_s = ru.ru_utime.tv_sec +
                       ru.ru_utime.tv_usec / 1'000'000.0;
        s.cpu_sys_s  = ru.ru_stime.tv_sec +
                       ru.ru_stime.tv_usec / 1'000'000.0;
        s.peak_rss_kb = PeakRSSKB();
    }

    return s;
}

// ── Public: one-shot collection (called after workload ends) ──────────────────

MetricSnapshot MetricsCollector::CollectOnce() {
    auto now     = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time_).count();
    uint64_t current_ops = ops_done_.load(std::memory_order_relaxed);
    double tput = 0.0;
    if (elapsed > 0) tput = static_cast<double>(current_ops) / elapsed / 1000.0;
    return DoCollect(elapsed, tput);
}

// ── Accessors ─────────────────────────────────────────────────────────────────

MetricSnapshot MetricsCollector::Latest() const {
    std::lock_guard<std::mutex> lk(latest_mu_);
    return latest_;
}

std::vector<MetricSnapshot> MetricsCollector::History() const {
    std::lock_guard<std::mutex> lk(history_mu_);
    return history_;
}

// ── CSV ───────────────────────────────────────────────────────────────────────

void MetricsCollector::WriteCSVHeader() {
    csv_file_ << "elapsed_s,ops_done,progress_pct,throughput_kops,"
                 "l0_files,pending_compaction_mb,immutable_memtables,"
                 "flush_pending,cur_threads,cur_batch_mb,"
                 "stall_state,last_action,"
                 "lat_lt1ms,lat_1_5ms,lat_5_10ms,lat_10_50ms,"
                 "lat_50_100ms,lat_100_500ms,lat_gt500ms,"
                 "flush_bytes,compact_bytes,user_bytes_ingested,"
                 "cpu_user_s,cpu_sys_s,peak_rss_kb\n";
    csv_file_.flush();
}

void MetricsCollector::WriteCSVRow(const MetricSnapshot& s) {
    csv_file_ << std::fixed << std::setprecision(3)
              << s.elapsed_s          << ','
              << s.ops_done           << ','
              << s.progress_pct       << ','
              << s.throughput_kops    << ','
              << s.l0_files           << ','
              << s.pending_compaction_mb << ','
              << s.immutable_memtables << ','
              << (s.flush_pending ? 1 : 0) << ','
              << s.cur_threads        << ','
              << s.cur_batch_mb       << ','
              << s.stall_state        << ','
              << s.last_action        << ',';
    for (int i = 0; i < 7; ++i)
        csv_file_ << s.lat_buckets[i] << (i < 6 ? ',' : ',');
    csv_file_ << s.flush_bytes          << ','
              << s.compact_bytes        << ','
              << s.user_bytes_ingested  << ','
              << s.cpu_user_s           << ','
              << s.cpu_sys_s            << ','
              << s.peak_rss_kb          << '\n';
    csv_file_.flush();
}

} // namespace adoc
