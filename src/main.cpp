// main.cpp
// ─────────────────────────────────────────────────────────────────────────────
// ADOC-Rocks Phase 2 – CLI entry point.
//
// Subcommands:
//   run     – single mode (default or adoc), fixed-data workload
//   compare – runs default then ADOC with identical parameters N times,
//             alternating order, then prints a side-by-side summary table
//   report  – print a summary from a saved CSV file
//
// Fair-comparison design (see project spec):
//   - Both modes use the same --num-ops, same seed, same key/value sizes.
//   - The timer stops at the last write ACK, never at a wall-clock limit.
//   - --max-duration is a safety-only timeout.
//   - DB directory is wiped before each run.
//   - Disk-space check before opening the DB.
//   - Paired seeds: repetition i uses seed (base_seed + i - 1) for BOTH modes.
//
// Live terminal output (refreshed every second by a separate display thread):
//   time(s) | progress% | throughput kOps/s | L0 files | pending MB |
//   imm MTs | threads | batch MB | stall | last action
// ─────────────────────────────────────────────────────────────────────────────

#include "adoc_tuner.h"
#include "metrics_collector.h"
#include "stall_listener.h"
#include "workload.h"

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>

#include <sys/stat.h>   // mkdir
#include <sys/types.h>
#include <unistd.h>     // getpid

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Global stop flag (set by Ctrl+C handler)
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_stop{false};
static void SigHandler(int) { g_stop.store(true); }

// ─────────────────────────────────────────────────────────────────────────────
// Helper: wipe a directory
// ─────────────────────────────────────────────────────────────────────────────
static void WipeDir(const std::string& path) {
    if (fs::exists(path)) {
        std::cout << "[Setup] Removing existing DB at: " << path << "\n";
        fs::remove_all(path);
    }
    fs::create_directories(path);
}

// ─────────────────────────────────────────────────────────────────────────────
// RocksDB options builder
// ─────────────────────────────────────────────────────────────────────────────
struct RocksConfig {
    int    initial_threads{4};
    double initial_batch_mb{64.0};
    bool   profile_paper{false};   // true → paper thresholds
    // ADOC thresholds (laptop profile defaults)
    uint64_t l0_slowdown{10};
    uint64_t l0_stop{20};
    uint64_t pending_soft_gb{1};
    uint64_t pending_hard_gb{2};
    // Block cache
    size_t   block_cache_mb{256};
};

static rocksdb::Options BuildOptions(
    const RocksConfig& rc,
    std::shared_ptr<rocksdb::Statistics>& out_stats) {

    rocksdb::Options opts;
    opts.create_if_missing = true;
    opts.error_if_exists   = false;

    // Statistics (needed for flush/compact byte tickers)
    out_stats = rocksdb::CreateDBStatistics();
    opts.statistics = out_stats;

    // Background threads
    opts.max_background_jobs = rc.initial_threads;

    // Memtable / batch size
    uint64_t batch_bytes =
        static_cast<uint64_t>(rc.initial_batch_mb * 1024 * 1024);
    opts.write_buffer_size    = batch_bytes;
    opts.target_file_size_base = batch_bytes;

    // Allow up to 4 immutable memtables so writes don't stall too early
    opts.max_write_buffer_number     = 4;
    opts.min_write_buffer_number_to_merge = 1;

    // L0 thresholds (triggers stalls – scaled for laptop / paper profile)
    if (rc.profile_paper) {
        opts.level0_slowdown_writes_trigger = 20;
        opts.level0_stop_writes_trigger     = 36;
        opts.soft_pending_compaction_bytes_limit =
            64ULL * 1024 * 1024 * 1024;  // 64 GB
        opts.hard_pending_compaction_bytes_limit =
            128ULL * 1024 * 1024 * 1024; // 128 GB
    } else {
        // Laptop profile: scaled-down thresholds to trigger stalls on small data
        opts.level0_slowdown_writes_trigger =
            static_cast<int>(rc.l0_slowdown);
        opts.level0_stop_writes_trigger     =
            static_cast<int>(rc.l0_stop);
        opts.soft_pending_compaction_bytes_limit =
            rc.pending_soft_gb * 1024ULL * 1024 * 1024;
        opts.hard_pending_compaction_bytes_limit =
            rc.pending_hard_gb * 1024ULL * 1024 * 1024;
    }

    // Compression: snappy (same for both modes)
    opts.compression = rocksdb::kSnappyCompression;

    // Block cache (same for both modes)
    rocksdb::BlockBasedTableOptions tbo;
    tbo.block_cache = rocksdb::NewLRUCache(rc.block_cache_mb * 1024 * 1024);
    tbo.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    opts.table_factory.reset(
        rocksdb::NewBlockBasedTableFactory(tbo));

    // WAL: enabled (fair comparison)
    opts.manual_wal_flush = false;
    opts.use_fsync        = false;

    return opts;
}

// ─────────────────────────────────────────────────────────────────────────────
// RunSummary – collected at the end of each mode run
// ─────────────────────────────────────────────────────────────────────────────
struct RunSummary {
    std::string mode;           // "default" or "adoc"
    uint64_t    seed{0};
    uint64_t    num_ops{0};
    double      total_gb{0.0};

    // Primary metrics (fixed-data comparison)
    double   elapsed_s{0.0};
    double   avg_tput_kops{0.0};
    double   tput_stddev{0.0};
    double   stall_total_s{0.0};
    double   stall_pct{0.0};
    uint64_t stall_count_l0{0};
    uint64_t stall_count_rdo{0};
    uint64_t stall_count_mmo{0};
    double   stall_s_l0{0.0};
    double   stall_s_rdo{0.0};
    double   stall_s_mmo{0.0};
    uint64_t p99_lat_us{0};
    uint64_t max_lat_us{0};
    bool     timed_out{false};
    double   drain_s{0.0};

    // Secondary metrics
    uint64_t flush_bytes{0};
    uint64_t compact_bytes{0};
    uint64_t user_bytes{0};
    double   write_amp{0.0};
    uint64_t db_size_bytes{0};
    double   space_amp{0.0};
    double   cpu_user_s{0.0};
    double   cpu_sys_s{0.0};
    uint64_t peak_rss_kb{0};

    // ADOC-specific
    int tuner_actions_l0{0};
    int tuner_actions_rdo{0};
    int tuner_actions_mmo{0};

    std::string csv_path;
};

// ─────────────────────────────────────────────────────────────────────────────
// Latency percentile helper
// ─────────────────────────────────────────────────────────────────────────────
// Computes p99 and max from the cumulative histogram buckets stored in the
// final MetricSnapshot. Bucket mid-points (µs):
//   0:<1ms  1:3ms  2:7ms  3:30ms  4:75ms  5:300ms  6:750ms
static constexpr std::array<uint64_t, 7> LAT_MID_US{
    500, 3000, 7000, 30000, 75000, 300000, 750000
};

static std::pair<uint64_t,uint64_t>
LatPercentilesUS(const std::array<uint64_t,7>& counts) {
    uint64_t total = 0;
    for (auto c : counts) total += c;
    if (total == 0) return {0, 0};

    uint64_t p99_thresh = static_cast<uint64_t>(total * 0.99);
    uint64_t running    = 0;
    uint64_t p99_us     = 0;
    uint64_t max_us     = 0;
    for (int i = 0; i < 7; ++i) {
        if (counts[i] == 0) continue;
        running += counts[i];
        max_us   = LAT_MID_US[i];
        if (p99_us == 0 && running >= p99_thresh)
            p99_us = LAT_MID_US[i];
    }
    return {p99_us, max_us};
}

// ─────────────────────────────────────────────────────────────────────────────
// Live terminal display thread
// ─────────────────────────────────────────────────────────────────────────────
static void LiveDisplay(adoc::MetricsCollector* mc,
                        const std::atomic<bool>& done) {
    // Header
    std::cout << "\n\033[1m"  // bold
              << std::setw(6)  << "t(s)"
              << std::setw(8)  << "prog%"
              << std::setw(12) << "kOps/s"
              << std::setw(7)  << "L0"
              << std::setw(12) << "pend_MB"
              << std::setw(6)  << "imm"
              << std::setw(8)  << "thds"
              << std::setw(10) << "batch_MB"
              << std::setw(12) << "stall"
              << "  last_action"
              << "\033[0m\n";
    std::cout << std::string(110, '-') << "\n";

    while (!done.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        adoc::MetricSnapshot s = mc->Latest();

        std::cout << '\r'
                  << std::fixed << std::setprecision(1)
                  << std::setw(6)  << s.elapsed_s
                  << std::setw(8)  << s.progress_pct
                  << std::setw(12) << s.throughput_kops
                  << std::setw(7)  << s.l0_files
                  << std::setw(12) << s.pending_compaction_mb
                  << std::setw(6)  << s.immutable_memtables
                  << std::setw(8)  << s.cur_threads
                  << std::setw(10) << s.cur_batch_mb
                  << std::setw(12) << s.stall_state.substr(0, 11)
                  << "  " << s.last_action.substr(0, 30)
                  << std::flush;
    }
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// DB size on disk helper
// ─────────────────────────────────────────────────────────────────────────────
static uint64_t DBSizeBytes(const std::string& path) {
    uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(
             path, fs::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file()) {
            total += entry.file_size();
        }
    }
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// Single run (one mode, one repetition)
// ─────────────────────────────────────────────────────────────────────────────
static RunSummary DoRun(const std::string& mode,
                        uint64_t           seed,
                        uint64_t           num_ops,
                        uint32_t           key_size,
                        uint32_t           value_size,
                        int                max_duration_s,
                        const std::string& db_path,
                        const std::string& csv_path,
                        const RocksConfig& rc,
                        adoc::WorkloadConfig::Type wtype,
                        int                num_writer_threads) {
    RunSummary summary;
    summary.mode    = mode;
    summary.seed    = seed;
    summary.num_ops = num_ops;
    summary.total_gb = static_cast<double>(num_ops) *
                       (key_size + value_size) / (1024.0*1024.0*1024.0);
    summary.csv_path = csv_path;

    std::cout << "\n========================================================\n"
              << " Mode: " << mode << "  seed=" << seed
              << "  num_ops=" << num_ops
              << "  total_gb=" << std::fixed << std::setprecision(2)
              << summary.total_gb << "\n"
              << " DB: " << db_path << "\n"
              << "========================================================\n";

    // ── Disk-space check ─────────────────────────────────────────────────────
    // The DB can occupy 3× the ingested data (space amplification).
    uint64_t ingest_bytes =
        num_ops * static_cast<uint64_t>(key_size + value_size);
    uint64_t required = 3 * ingest_bytes;
    if (!adoc::CheckDiskSpace(fs::path(db_path).parent_path().string(),
                               required)) {
        throw std::runtime_error("Insufficient disk space. Aborting.");
    }

    // ── Wipe DB ──────────────────────────────────────────────────────────────
    WipeDir(db_path);

    // ── Build RocksDB options ─────────────────────────────────────────────────
    std::shared_ptr<rocksdb::Statistics> stats;
    rocksdb::Options opts = BuildOptions(rc, stats);

    // ── Metrics collector ─────────────────────────────────────────────────────
    // We create the collector before opening the DB so we can wire the stall
    // listener callback to it.  The collector only needs the DB pointer for
    // GetProperty calls, which we set after opening.
    adoc::CollectorConfig cc;
    cc.num_ops            = num_ops;
    cc.key_size           = key_size;
    cc.value_size         = value_size;
    cc.sample_interval_ms = 1000;
    cc.csv_path           = csv_path;

    // Placeholder collector with nullptr db – we will swap the db ptr below.
    // Since CollectorConfig is set now, we defer construction until after open.

    // ── Stall listener ────────────────────────────────────────────────────────
    // EventListeners must be in opts.listeners BEFORE DB::Open (public API).
    // We use a shared_ptr<StallListener> so the callback can safely reference
    // collector (which lives on the stack for the duration of the run).
    // We open the DB once with the listener already registered.
    //
    // Two-step: open DB with a no-op listener first to create the DB files,
    // then immediately reopen with the real wired listener.
    // This lets us avoid constructing collector before the DB path exists.
    //
    // Step 1: open with a dummy listener to create/verify DB structure.
    {
        auto dummy = std::make_shared<rocksdb::EventListener>();
        opts.listeners.push_back(dummy);
        std::unique_ptr<rocksdb::DB> tmp;
        rocksdb::Status s1 = rocksdb::DB::Open(opts, db_path, &tmp);
        if (!s1.ok())
            throw std::runtime_error("RocksDB initial open failed: " + s1.ToString());
        // tmp destroyed here → DB closed cleanly
    }
    opts.listeners.clear();

    // Step 2: build the wired stall listener with a deferred callback.
    // The callback is filled in after the collector is constructed (step 4).
    // A shared_ptr<function> bridge lets the listener hold a stable target
    // even though the collector is not yet alive at listener-construction time.
    std::shared_ptr<adoc::StallListener> stall_listener2;

    // We build the listener with a deferred callback that looks up the collector
    // via a shared pointer we fill in after construction.
    auto cb_target = std::make_shared<std::function<void(const std::string&)>>();
    stall_listener2 = std::make_shared<adoc::StallListener>(
        [cb_target](const std::string& tag) {
            if (*cb_target) (*cb_target)(tag);
        });
    opts.listeners.push_back(stall_listener2);

    // Step 3: reopen with the real listener.
    std::unique_ptr<rocksdb::DB> db;
    {
        rocksdb::Status s2 = rocksdb::DB::Open(opts, db_path, &db);
        if (!s2.ok())
            throw std::runtime_error("RocksDB final open failed: " + s2.ToString());
    }

    // Step 4: construct the collector now that we have a live DB pointer.
    adoc::MetricsCollector collector(db.get(), stats, cc);
    collector.SetCurrentParams(rc.initial_threads, rc.initial_batch_mb);

    // Wire the stall listener callback to the real collector.
    *cb_target = [&collector](const std::string& tag) {
        collector.SetStallState(tag);
    };

    // ── ADOC tuner (only in adoc mode) ────────────────────────────────────────
    std::unique_ptr<adoc::AdocTuner> tuner;
    if (mode == "adoc") {
        adoc::TunerConfig tc;
        tc.min_threads      = 2;
        tc.max_threads      = rc.initial_threads * 4;
        tc.min_batch_mb     = 32.0;
        tc.max_batch_mb     = 512.0;
        tc.initial_threads  = rc.initial_threads;
        tc.initial_batch_mb = rc.initial_batch_mb;
        // Thresholds mirror RocksDB options for consistent triggering
        if (rc.profile_paper) {
            tc.l0_overflow_files = 20;
            tc.rdo_soft_bytes    = 64ULL*1024*1024*1024;
            tc.rdo_hard_bytes    = 128ULL*1024*1024*1024;
        } else {
            tc.l0_overflow_files = rc.l0_slowdown;
            // The CLI --pending-soft-gb / --pending-hard-gb set RocksDB's own
            // stall thresholds (in GB). The ADOC tuner uses its own independent
            // thresholds (defaults: 50 MB / 100 MB) so it reacts *before*
            // RocksDB hard-stops. We keep the tuner defaults here; the
            // rocksdb_stalling fallback in DetectOverflow() handles cases where
            // RocksDB fires before the tuner threshold is reached.
            // (Leave tc.rdo_soft_bytes / rdo_hard_bytes at their defaults.)
        }
        tc.sample_interval_ms = 1000;

        tuner = std::make_unique<adoc::AdocTuner>(
            db.get(), &collector, stall_listener2.get(), tc);
    }

    // ── Workload config ───────────────────────────────────────────────────────
    adoc::WorkloadConfig wc;
    wc.num_ops            = num_ops;
    wc.max_duration_s     = max_duration_s;
    wc.key_size           = key_size;
    wc.value_size         = value_size;
    wc.seed               = seed;
    wc.num_writer_threads = num_writer_threads;
    wc.type               = wtype;

    adoc::WorkloadRunner runner(
        db.get(), wc,
        [&collector](uint64_t n) { collector.RecordOp(n); },
        [&collector](uint64_t ns) { collector.RecordLatency(ns); });

    // ── Start everything ──────────────────────────────────────────────────────
    auto run_start = std::chrono::steady_clock::now();
    collector.Start();
    if (tuner) tuner->Start(run_start);
    collector.SetCurrentParams(rc.initial_threads, rc.initial_batch_mb);

    // ── Live display thread ───────────────────────────────────────────────────
    std::atomic<bool> display_done{false};
    std::thread display_thread(LiveDisplay, &collector, std::ref(display_done));

    // ── Run workload (blocking) ───────────────────────────────────────────────
    adoc::RunResult wr = runner.Run();

    // ── Stop everything ───────────────────────────────────────────────────────
    display_done.store(true);
    if (tuner) tuner->Stop();
    collector.Stop();
    if (display_thread.joinable()) display_thread.join();

    // ── Final snapshot ────────────────────────────────────────────────────────
    adoc::MetricSnapshot final_snap = collector.CollectOnce();
    auto history = collector.History();

    // ── Fill RunSummary ───────────────────────────────────────────────────────
    summary.elapsed_s  = wr.elapsed_s;
    summary.drain_s    = wr.drain_s;
    summary.timed_out  = wr.timed_out;
    summary.avg_tput_kops = (wr.elapsed_s > 0)
        ? static_cast<double>(wr.ops_written) / wr.elapsed_s / 1000.0
        : 0.0;

    // Throughput std-dev across 1-second windows
    {
        std::vector<double> tputs;
        tputs.reserve(history.size());
        for (const auto& snap : history)
            tputs.push_back(snap.throughput_kops);
        if (tputs.size() > 1) {
            double mean = 0.0;
            for (double v : tputs) mean += v;
            mean /= tputs.size();
            double var = 0.0;
            for (double v : tputs) var += (v - mean) * (v - mean);
            summary.tput_stddev = std::sqrt(var / tputs.size());
        }
    }

    // Stall statistics
    if (tuner) {
        summary.stall_s_l0  = tuner->TotalStallS("l0");
        summary.stall_s_rdo = tuner->TotalStallS("rdo");
        summary.stall_s_mmo = tuner->TotalStallS("mmo");
        summary.stall_count_l0  = tuner->StallCount("l0");
        summary.stall_count_rdo = tuner->StallCount("rdo");
        summary.stall_count_mmo = tuner->StallCount("mmo");
        summary.stall_total_s   = tuner->TotalStallS("all");

        summary.tuner_actions_l0  = tuner->CountActions(adoc::OverflowType::L0O);
        summary.tuner_actions_rdo = tuner->CountActions(adoc::OverflowType::RDO);
        summary.tuner_actions_mmo = tuner->CountActions(adoc::OverflowType::MMO);
    } else {
        // For default mode, derive stall stats from the stall_listener directly
        adoc::StallStats ss = stall_listener2->GetStats();
        summary.stall_total_s   = ss.total_stall_s;
        summary.stall_count_l0  = ss.count_l0;
        summary.stall_count_rdo = ss.count_ps;  // generic: all mapped to ps
        summary.stall_count_mmo = ss.count_mt;
        summary.stall_s_l0      = ss.dur_l0_s;
        summary.stall_s_rdo     = ss.dur_ps_s;
        summary.stall_s_mmo     = ss.dur_mt_s;
    }

    if (summary.elapsed_s > 0)
        summary.stall_pct = 100.0 * summary.stall_total_s / summary.elapsed_s;

    // Latency percentiles from the final cumulative histogram
    auto [p99, maxl] = LatPercentilesUS(final_snap.lat_buckets);
    summary.p99_lat_us = p99;
    summary.max_lat_us = maxl;

    // Write amplification
    summary.flush_bytes   = final_snap.flush_bytes;
    summary.compact_bytes = final_snap.compact_bytes;
    summary.user_bytes    = final_snap.user_bytes_ingested;
    if (summary.user_bytes > 0) {
        summary.write_amp =
            static_cast<double>(summary.flush_bytes + summary.compact_bytes) /
            summary.user_bytes;
    }

    // Space amplification
    summary.db_size_bytes = DBSizeBytes(db_path);
    if (summary.user_bytes > 0) {
        summary.space_amp =
            static_cast<double>(summary.db_size_bytes) / summary.user_bytes;
    }

    // CPU / RSS from final snapshot
    summary.cpu_user_s  = final_snap.cpu_user_s;
    summary.cpu_sys_s   = final_snap.cpu_sys_s;
    summary.peak_rss_kb = final_snap.peak_rss_kb;

    return summary;
}

// ─────────────────────────────────────────────────────────────────────────────
// Print a single RunSummary to stdout
// ─────────────────────────────────────────────────────────────────────────────
static void PrintSummary(const RunSummary& s) {
    auto line = [](int w, const std::string& k, const std::string& v) {
        std::cout << "  " << std::left << std::setw(w) << k << " " << v << "\n";
    };

    std::cout << "\n──────────────────────────────────────────────────────\n";
    std::cout << " Run Summary  [mode=" << s.mode << "  seed=" << s.seed << "]\n";
    std::cout << "──────────────────────────────────────────────────────\n";
    std::cout << " DATA\n";
    line(28, "  num_ops:", std::to_string(s.num_ops));
    line(28, "  total_gb:",
         std::to_string(s.total_gb).substr(0,6));
    line(28, "  timed_out:", s.timed_out ? "YES (see --max-duration)" : "no");
    std::cout << " PRIMARY METRICS\n";
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << s.elapsed_s << " s";
        line(28, "  completion_time:", oss.str());
    }
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << s.avg_tput_kops
            << " kOps/s  (σ=" << s.tput_stddev << ")";
        line(28, "  avg_throughput:", oss.str());
    }
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << s.stall_total_s
            << " s  (" << s.stall_pct << "% of runtime)";
        line(28, "  stall_total:", oss.str());
    }
    {
        std::ostringstream oss;
        oss << "L0:" << s.stall_count_l0
            << "(" << std::fixed << std::setprecision(1) << s.stall_s_l0 << "s)"
            << "  PS/RDO:" << s.stall_count_rdo
            << "(" << s.stall_s_rdo << "s)"
            << "  MT:" << s.stall_count_mmo
            << "(" << s.stall_s_mmo << "s)";
        line(28, "  stall_by_type:", oss.str());
    }
    {
        std::ostringstream oss;
        oss << "p99=" << s.p99_lat_us << " µs  max=" << s.max_lat_us << " µs";
        line(28, "  write_latency:", oss.str());
    }
    std::cout << " SECONDARY METRICS\n";
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << s.write_amp << "x"
            << "  (flush=" << s.flush_bytes/1024/1024 << " MB"
            << "  compact=" << s.compact_bytes/1024/1024 << " MB"
            << "  user=" << s.user_bytes/1024/1024 << " MB)";
        line(28, "  write_amplification:", oss.str());
    }
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << s.space_amp << "x"
            << "  (db=" << s.db_size_bytes/1024/1024 << " MB)";
        line(28, "  space_amplification:", oss.str());
    }
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1)
            << s.cpu_user_s << "s user  " << s.cpu_sys_s << "s sys";
        line(28, "  cpu_time:", oss.str());
    }
    {
        std::ostringstream oss;
        oss << s.peak_rss_kb / 1024 << " MB";
        line(28, "  peak_rss:", oss.str());
    }
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << s.drain_s << " s";
        line(28, "  drain_time:", oss.str());
    }
    if (s.mode == "adoc") {
        std::cout << " ADOC TUNER\n";
        line(28, "  actions_L0O:", std::to_string(s.tuner_actions_l0));
        line(28, "  actions_RDO:", std::to_string(s.tuner_actions_rdo));
        line(28, "  actions_MMO:", std::to_string(s.tuner_actions_mmo));
    }
    if (!s.csv_path.empty())
        line(28, "  csv:", s.csv_path);
    std::cout << "──────────────────────────────────────────────────────\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Compare mode: side-by-side summary table
// ─────────────────────────────────────────────────────────────────────────────
static void PrintCompareTable(const std::vector<RunSummary>& defaults,
                               const std::vector<RunSummary>& adocs) {
    auto mean = [](const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        double s = 0; for (double x : v) s += x; return s / v.size();
    };
    auto stddev = [&](const std::vector<double>& v) {
        if (v.size() < 2) return 0.0;
        double m = mean(v), s = 0;
        for (double x : v) s += (x-m)*(x-m);
        return std::sqrt(s / v.size());
    };
    auto pct_imp = [](double def, double adoc) -> std::string {
        if (def <= 0) return "N/A";
        double imp = (def - adoc) / def * 100.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        if (imp >= 0) oss << "+" << imp << "% better";
        else          oss << imp << "% worse";
        return oss.str();
    };

    // Collect arrays
    std::vector<double> def_time, adoc_time;
    std::vector<double> def_tput, adoc_tput;
    std::vector<double> def_stall, adoc_stall;
    std::vector<double> def_p99, adoc_p99;
    std::vector<double> def_cpu, adoc_cpu;
    std::vector<double> def_samp, adoc_samp;

    for (const auto& r : defaults) {
        def_time.push_back(r.elapsed_s);
        def_tput.push_back(r.avg_tput_kops);
        def_stall.push_back(r.stall_total_s);
        def_p99.push_back(static_cast<double>(r.p99_lat_us));
        def_cpu.push_back(r.cpu_user_s + r.cpu_sys_s);
        def_samp.push_back(r.space_amp);
    }
    for (const auto& r : adocs) {
        adoc_time.push_back(r.elapsed_s);
        adoc_tput.push_back(r.avg_tput_kops);
        adoc_stall.push_back(r.stall_total_s);
        adoc_p99.push_back(static_cast<double>(r.p99_lat_us));
        adoc_cpu.push_back(r.cpu_user_s + r.cpu_sys_s);
        adoc_samp.push_back(r.space_amp);
    }

    std::cout << "\n"
              << "╔══════════════════════════════════════════════════════════════╗\n"
              << "║            COMPARISON SUMMARY (Fixed-Data)                  ║\n"
              << "╚══════════════════════════════════════════════════════════════╝\n\n";

    std::cout << std::left
              << std::setw(28) << "Metric"
              << std::setw(22) << "Default (mean±σ)"
              << std::setw(22) << "ADOC (mean±σ)"
              << "Improvement\n";
    std::cout << std::string(84, '-') << "\n";

    auto row = [&](const std::string& name,
                   const std::vector<double>& d,
                   const std::vector<double>& a,
                   const std::string& unit,
                   bool lower_is_better = true) {
        std::ostringstream ds, as;
        ds << std::fixed << std::setprecision(2)
           << mean(d) << "±" << stddev(d) << unit;
        as << std::fixed << std::setprecision(2)
           << mean(a) << "±" << stddev(a) << unit;
        double md = mean(d), ma = mean(a);
        std::string imp;
        if (lower_is_better) imp = pct_imp(md, ma);
        else {
            // higher is better (throughput): positive = adoc is better
            if (md <= 0) imp = "N/A";
            else {
                double diff = (ma - md) / md * 100.0;
                std::ostringstream o;
                o << std::fixed << std::setprecision(1);
                if (diff >= 0) o << "+" << diff << "% better";
                else           o << diff << "% worse";
                imp = o.str();
            }
        }
        std::cout << std::left
                  << std::setw(28) << name
                  << std::setw(22) << ds.str()
                  << std::setw(22) << as.str()
                  << imp << "\n";
    };

    row("completion_time(s)",     def_time,  adoc_time,  "s",   true);
    row("avg_throughput(kOps/s)", def_tput,  adoc_tput,  "",    false);
    row("stall_total(s)",         def_stall, adoc_stall, "s",   true);
    row("p99_latency(µs)",        def_p99,   adoc_p99,   "µs",  true);
    row("cpu_total(s)",           def_cpu,   adoc_cpu,   "s",   true);
    row("space_amp(x)",           def_samp,  adoc_samp,  "x",   true);

    std::cout << "\n  Note: improvement% = (default - adoc) / default × 100\n"
              << "  +N% = ADOC is N% better.  −N% = ADOC is worse.\n"
              << "  CPU time trade-off is expected if ADOC does more compaction.\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Report subcommand: print summary from a CSV
// ─────────────────────────────────────────────────────────────────────────────
static void CmdReport(const std::string& csv_path) {
    std::ifstream f(csv_path);
    if (!f.is_open()) {
        std::cerr << "Cannot open CSV: " << csv_path << "\n";
        return;
    }
    std::string line;
    int row = 0;
    double last_elapsed = 0, last_tput = 0, last_l0 = 0;
    uint64_t ops = 0;
    while (std::getline(f, line)) {
        if (row == 0) { row++; continue; }  // header
        // Parse only the columns we need for the summary
        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> cols;
        while (std::getline(ss, tok, ',')) cols.push_back(tok);
        if (cols.size() < 4) continue;
        last_elapsed = std::stod(cols[0]);
        ops          = std::stoull(cols[1]);
        last_tput    = std::stod(cols[3]);
        if (cols.size() > 4) last_l0 = std::stod(cols[4]);
        row++;
    }
    double avg_tput = (last_elapsed > 0 && ops > 0)
                      ? ops / last_elapsed / 1000.0 : last_tput;
    std::cout << "\n── Report: " << csv_path << " ──\n"
              << "  rows sampled     : " << (row-1) << "\n"
              << "  final elapsed(s) : " << last_elapsed << "\n"
              << "  ops completed    : " << ops << "\n"
              << "  avg tput (kOps/s): " << avg_tput << "\n"
              << "  final L0 files   : " << last_l0 << "\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// CLI parsing helpers
// ─────────────────────────────────────────────────────────────────────────────

static void Usage() {
    std::cout << R"(
ADOC-Rocks Phase 2  –  Fixed-Data Write-Stall Mitigation Benchmark

Usage:
  adocrocks run [options]
  adocrocks compare [options]
  adocrocks report --csv FILE

── run ──────────────────────────────────────────────────────────────────────
  --mode       {default|adoc}       (default: default)
  --workload   {fillrandom|readwhilewriting|ycsb-a} (default: fillrandom)
  --profile    {paper|laptop}       (default: laptop)
  --num-ops    N                    fixed number of write ops to perform
  --total-gb   G                    alternative: ingest this many GB (converted to ops)
  --size       {small|medium|large} preset (overrides --num-ops)
  --seed       N                    PRNG seed (default: 42)
  --max-duration N                  safety timeout in seconds (default: 3600)
  --threads    N                    initial RocksDB background threads (default: 4)
  --batch-mb   N                    initial write_buffer_size in MB (default: 64)
  --db-path    PATH                 (default: /tmp/adocdb)
  --value-size N                    bytes per value (default: 1000)
  --key-size   N                    bytes per key (default: 16)
  --writer-threads N                number of writer threads (default: 1)
  --csv        PATH                 CSV output file (default: results_<mode>_<seed>.csv)
  --l0-slowdown N                   L0 slowdown threshold files (laptop profile)
  --l0-stop    N                    L0 stop threshold files (laptop profile)
  --pending-soft-gb N               pending compaction soft limit GB
  --pending-hard-gb N               pending compaction hard limit GB

── compare ──────────────────────────────────────────────────────────────────
  Same options as run, plus:
  --repeat N                        number of repetition pairs (default: 3)

  Both default and ADOC use the same --num-ops, --seed, --key-size,
  --value-size, and --db-path (separate subdirectories are created).
  Seeds are paired: repetition i uses seed (base_seed + i - 1) for BOTH.
  Run order alternates: [default, adoc], [adoc, default], [default, adoc].

── report ───────────────────────────────────────────────────────────────────
  --csv  FILE    print summary from a saved CSV file

── Disk-space presets (key+value = 1016 bytes each) ─────────────────────────
  --size small   →  5,000,000 ops  (~4.7 GB ingest, needs ~20 GB free)
  --size medium  → 20,000,000 ops  (~18.9 GB ingest, needs ~75 GB free)
  --size large   → 50,000,000 ops  (~47.2 GB ingest, needs ~180 GB free)
)";
}

struct CliArgs {
    std::string subcommand;
    std::string mode{"default"};
    std::string workload{"fillrandom"};
    std::string profile{"laptop"};
    uint64_t    num_ops{0};
    double      total_gb{0.0};
    std::string size_preset;
    uint64_t    seed{42};
    int         max_duration{3600};
    int         initial_threads{4};
    double      batch_mb{64.0};
    std::string db_path{"/tmp/adocdb"};
    int         value_size{1000};
    int         key_size{16};
    int         writer_threads{1};
    std::string csv_path;
    int         repeat{3};

    // Threshold overrides
    uint64_t l0_slowdown{10};
    uint64_t l0_stop{20};
    uint64_t pending_soft_gb{1};
    uint64_t pending_hard_gb{2};
};

static CliArgs ParseArgs(int argc, char** argv) {
    CliArgs a;
    if (argc < 2) { Usage(); exit(0); }
    a.subcommand = argv[1];
    if (a.subcommand == "--help" || a.subcommand == "-h") { Usage(); exit(0); }

    for (int i = 2; i < argc; ++i) {
        std::string key = argv[i];
        auto next = [&]() -> std::string {
            if (i+1 >= argc) { std::cerr << "Missing value for " << key << "\n"; exit(1); }
            return argv[++i];
        };
        if      (key == "--mode")          a.mode           = next();
        else if (key == "--workload")      a.workload       = next();
        else if (key == "--profile")       a.profile        = next();
        else if (key == "--num-ops")       a.num_ops        = std::stoull(next());
        else if (key == "--total-gb")      a.total_gb       = std::stod(next());
        else if (key == "--size")          a.size_preset    = next();
        else if (key == "--seed")          a.seed           = std::stoull(next());
        else if (key == "--max-duration")  a.max_duration   = std::stoi(next());
        else if (key == "--threads")       a.initial_threads = std::stoi(next());
        else if (key == "--batch-mb")      a.batch_mb       = std::stod(next());
        else if (key == "--db-path")       a.db_path        = next();
        else if (key == "--value-size")    a.value_size     = std::stoi(next());
        else if (key == "--key-size")      a.key_size       = std::stoi(next());
        else if (key == "--writer-threads") a.writer_threads = std::stoi(next());
        else if (key == "--csv")           a.csv_path       = next();
        else if (key == "--repeat")        a.repeat         = std::stoi(next());
        else if (key == "--l0-slowdown")   a.l0_slowdown    = std::stoull(next());
        else if (key == "--l0-stop")       a.l0_stop        = std::stoull(next());
        else if (key == "--pending-soft-gb") a.pending_soft_gb = std::stoull(next());
        else if (key == "--pending-hard-gb") a.pending_hard_gb = std::stoull(next());
        else { std::cerr << "Unknown flag: " << key << "\n"; Usage(); exit(1); }
    }

    // Resolve --size preset
    uint64_t bytes_per_op = static_cast<uint64_t>(a.key_size + a.value_size);
    if (!a.size_preset.empty()) {
        if      (a.size_preset == "small")  a.num_ops = 5'000'000;
        else if (a.size_preset == "medium") a.num_ops = 20'000'000;
        else if (a.size_preset == "large")  a.num_ops = 50'000'000;
        else {
            std::cerr << "--size must be small|medium|large\n"; exit(1);
        }
    }
    // Resolve --total-gb
    if (a.total_gb > 0 && a.num_ops == 0) {
        a.num_ops = static_cast<uint64_t>(
            a.total_gb * 1024 * 1024 * 1024 / bytes_per_op);
    }
    // Default fallback
    if (a.num_ops == 0) a.num_ops = 5'000'000;

    return a;
}

static adoc::WorkloadConfig::Type ParseWorkloadType(const std::string& s) {
    if (s == "readwhilewriting")  return adoc::WorkloadConfig::Type::readwhilewriting;
    if (s == "ycsb-a")            return adoc::WorkloadConfig::Type::ycsba;
    return adoc::WorkloadConfig::Type::fillrandom;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    std::signal(SIGINT,  SigHandler);
    std::signal(SIGTERM, SigHandler);

    CliArgs a = ParseArgs(argc, argv);

    // ── report subcommand ─────────────────────────────────────────────────────
    if (a.subcommand == "report") {
        if (a.csv_path.empty()) {
            std::cerr << "report requires --csv FILE\n"; return 1;
        }
        CmdReport(a.csv_path);
        return 0;
    }

    // ── build shared RocksConfig ──────────────────────────────────────────────
    RocksConfig rc;
    rc.initial_threads  = a.initial_threads;
    rc.initial_batch_mb = a.batch_mb;
    rc.profile_paper    = (a.profile == "paper");
    rc.l0_slowdown      = a.l0_slowdown;
    rc.l0_stop          = a.l0_stop;
    rc.pending_soft_gb  = a.pending_soft_gb;
    rc.pending_hard_gb  = a.pending_hard_gb;

    adoc::WorkloadConfig::Type wtype = ParseWorkloadType(a.workload);

    // ── Print run configuration preamble ─────────────────────────────────────
    double ingest_gb = static_cast<double>(a.num_ops) *
                       (a.key_size + a.value_size) / (1024.0*1024.0*1024.0);
    std::cout << "\n══════════════════════════════════════════════\n"
              << " ADOC-Rocks  subcommand=" << a.subcommand << "\n"
              << " num_ops=" << a.num_ops
              << "  ingest_gb=" << std::fixed << std::setprecision(2) << ingest_gb
              << "  seed=" << a.seed
              << "  profile=" << a.profile << "\n"
              << " key=" << a.key_size << "B  value=" << a.value_size << "B"
              << "  writer_threads=" << a.writer_threads << "\n"
              << "══════════════════════════════════════════════\n";

    // ── run subcommand ────────────────────────────────────────────────────────
    if (a.subcommand == "run") {
        std::string csv = a.csv_path.empty()
            ? "results_" + a.mode + "_s" + std::to_string(a.seed) + ".csv"
            : a.csv_path;

        try {
            RunSummary s = DoRun(
                a.mode, a.seed, a.num_ops,
                static_cast<uint32_t>(a.key_size),
                static_cast<uint32_t>(a.value_size),
                a.max_duration, a.db_path, csv, rc, wtype, a.writer_threads);
            PrintSummary(s);
        } catch (const std::exception& e) {
            std::cerr << "[FATAL] " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── compare subcommand ────────────────────────────────────────────────────
    if (a.subcommand == "compare") {
        std::vector<RunSummary> defaults_results, adoc_results;
        int N = a.repeat;

        // Alternating run order: rep1→[default,adoc] rep2→[adoc,default] …
        // This reduces thermal / disk warm-up bias across repetitions.
        for (int rep = 1; rep <= N; ++rep) {
            uint64_t rep_seed = a.seed + static_cast<uint64_t>(rep - 1);

            // Determine run order for this repetition
            bool def_first = (rep % 2 == 1);
            std::vector<std::string> order =
                def_first ? std::vector<std::string>{"default","adoc"}
                           : std::vector<std::string>{"adoc","default"};

            for (const auto& mode : order) {
                if (g_stop.load()) goto compare_done;

                std::string db = a.db_path + "/" + mode + "_rep" + std::to_string(rep);
                std::string csv = "results_" + mode
                                  + "_rep" + std::to_string(rep)
                                  + "_s"   + std::to_string(rep_seed) + ".csv";
                std::cout << "\n[Compare] rep=" << rep
                          << "/" << N
                          << "  mode=" << mode
                          << "  seed=" << rep_seed << "\n";
                try {
                    RunSummary s = DoRun(
                        mode, rep_seed, a.num_ops,
                        static_cast<uint32_t>(a.key_size),
                        static_cast<uint32_t>(a.value_size),
                        a.max_duration, db, csv, rc, wtype, a.writer_threads);
                    if (mode == "default") defaults_results.push_back(s);
                    else                   adoc_results.push_back(s);
                    PrintSummary(s);
                } catch (const std::exception& e) {
                    std::cerr << "[FATAL] " << e.what() << "\n";
                    goto compare_done;
                }
            }
        }
    compare_done:
        if (!defaults_results.empty() && !adoc_results.empty()) {
            PrintCompareTable(defaults_results, adoc_results);
        } else {
            std::cout << "[Compare] Not enough results to print table.\n";
        }
        return 0;
    }

    std::cerr << "Unknown subcommand: " << a.subcommand
              << "  (use run, compare, or report)\n";
    Usage();
    return 1;
}
