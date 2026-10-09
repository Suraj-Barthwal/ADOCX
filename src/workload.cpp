// workload.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Deterministic workload runner with fixed-ops termination.
//
// FAIR COMPARISON requirement:
//   - Both default and ADOC modes write EXACTLY the same number of ops.
//   - Same seed → same key sequence → same bytes on disk.
//   - Timer starts at the first write; stops when the last write is ACKed.
//   - WaitForCompact is called after the timer stops, and its duration is
//     reported as "drain_s" – a separate metric, not included in elapsed_s.
//
// Key generation:
//   We use std::mt19937_64 (a fast, deterministic Mersenne Twister).
//   Each thread is seeded with (base_seed + thread_id), so:
//     - Single-thread runs are 100% identical across default/ADOC.
//     - Multi-thread runs have the same key distribution but different order.
// ─────────────────────────────────────────────────────────────────────────────

#include "workload.h"

#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <sys/statvfs.h>  // statvfs for disk space check

#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace adoc {

// ── MakeKey ───────────────────────────────────────────────────────────────────
// Converts a 64-bit integer to a zero-padded hex string of exactly key_size
// bytes (padded with ASCII '0' on the left; truncated on the right if
// key_size < 16). This guarantees:
//   - Fixed size (RocksDB skips key-size variance).
//   - 1-to-1 mapping from rng_value to key bytes (no hash collision).

std::string MakeKey(uint64_t rng_value, uint32_t key_size) {
    // Format as 16-char hex then truncate / pad to key_size
    std::array<char, 17> buf{};
    std::snprintf(buf.data(), buf.size(), "%016llx",
                  static_cast<unsigned long long>(rng_value));

    std::string key;
    key.resize(key_size, '0');
    // Copy up to min(16, key_size) hex chars, then pad the rest with '0'
    uint32_t copy_len = std::min(key_size, 16u);
    std::memcpy(key.data(), buf.data(), copy_len);
    return key;
}

// ── CheckDiskSpace ────────────────────────────────────────────────────────────
// Returns true if free space on the filesystem containing 'path' is at least
// required_bytes * 1.2 (20% safety headroom, as specified).

bool CheckDiskSpace(const std::string& path, uint64_t required_bytes) {
    // Walk up the path until we find an existing directory to stat.
    // The DB directory itself may not exist yet (it is created by WipeDir).
    struct statvfs sv{};
    std::string check_path = path;
    while (!check_path.empty() && check_path != "/") {
        if (statvfs(check_path.c_str(), &sv) == 0) break;
        // Remove last path component
        auto pos = check_path.rfind('/');
        check_path = (pos == std::string::npos) ? "" : check_path.substr(0, pos);
    }
    if (check_path.empty() || statvfs(check_path.c_str(), &sv) != 0) {
        std::cerr << "[DiskCheck] Cannot stat filesystem at: " << path
                  << " – proceeding without check.\n";
        return true;  // permissive: don't abort if stat fails
    }

    uint64_t free_bytes = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
    uint64_t needed     = static_cast<uint64_t>(required_bytes * 1.2);

    if (free_bytes < needed) {
        std::cerr << "\n[ABORT] Not enough free disk space.\n"
                  << "  Required (ingest × 3 + 20% headroom): "
                  << needed / (1024*1024*1024) << " GB\n"
                  << "  Available on " << path << ": "
                  << free_bytes / (1024*1024*1024) << " GB\n"
                  << "  Free up space or use --size small / --num-ops N.\n\n";
        return false;
    }
    return true;
}

// ── WorkloadRunner ────────────────────────────────────────────────────────────

WorkloadRunner::WorkloadRunner(rocksdb::DB*          db,
                               const WorkloadConfig& cfg,
                               OpCallback            on_op_done,
                               LatCallback           on_latency)
    : db_(db), cfg_(cfg),
      on_op_done_(std::move(on_op_done)),
      on_latency_(std::move(on_latency)) {}

// ── RequestStop ───────────────────────────────────────────────────────────────

void WorkloadRunner::RequestStop() {
    stop_requested_.store(true);
}

// ── WriterThread ──────────────────────────────────────────────────────────────
// Each thread writes a share of the total ops.
// seed = base_seed + thread_id for reproducibility.

void WorkloadRunner::WriterThread(int thread_id) {
    // Per-thread PRNG – seeded deterministically
    std::mt19937_64 rng(cfg_.seed + static_cast<uint64_t>(thread_id));

    // Pre-allocated value buffer (all same character – realistic for benchmarks)
    std::string value(cfg_.value_size, 'v');

    rocksdb::WriteOptions wo;
    wo.disableWAL = false;  // WAL on: fair comparison (same durability setting)

    const uint64_t total = cfg_.num_ops;
    const int      nw    = cfg_.num_writer_threads;

    while (true) {
        // Atomic counter determines who writes the next op.
        // fetch_add returns the value BEFORE the increment, so the first
        // thread gets 0, the second gets 1, etc.  When my_op >= total the
        // quota is exhausted and this thread exits without writing.
        // We do NOT subtract back because the counter is only used for
        // progress tracking (RecordOp is called separately per write) and
        // must not underflow when multiple threads over-shoot simultaneously.
        uint64_t my_op = ops_done_.fetch_add(1, std::memory_order_relaxed);
        if (my_op >= total) break;

        if (stop_requested_.load(std::memory_order_relaxed)) break;

        // Check safety timeout
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        if (elapsed >= cfg_.max_duration_s) {
            stop_requested_.store(true);
            break;
        }

        // Generate key
        uint64_t rng_val = rng();
        std::string key  = MakeKey(rng_val, cfg_.key_size);

        // Measure write latency
        auto t0 = std::chrono::steady_clock::now();
        rocksdb::Status s = db_->Put(wo, key, value);
        auto t1 = std::chrono::steady_clock::now();

        if (!s.ok()) {
            std::cerr << "[Writer " << thread_id << "] Put failed: "
                      << s.ToString() << "\n";
        }

        uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t1 - t0).count();
        if (on_latency_) on_latency_(lat_ns);
        if (on_op_done_) on_op_done_(1);
    }

    (void)nw;  // suppress unused warning; each thread self-terminates via counter
}

// ── ReaderThread ──────────────────────────────────────────────────────────────
// Used for readwhilewriting and ycsb-a.
// Generates random read keys from the SAME key space using a separate PRNG
// offset (seed + 1000 + thread_id to avoid collision with writer seeds).

void WorkloadRunner::ReaderThread(int thread_id) {
    std::mt19937_64 rng(cfg_.seed + 1000ULL +
                        static_cast<uint64_t>(thread_id));
    rocksdb::ReadOptions ro;
    std::string value;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        uint64_t rng_val = rng();
        std::string key  = MakeKey(rng_val, cfg_.key_size);
        // Ignore not-found errors (key may not exist yet)
        db_->Get(ro, key, &value);
    }
}

// ── Run ───────────────────────────────────────────────────────────────────────
// Blocking. Returns RunResult after workload completes.

RunResult WorkloadRunner::Run() {
    ops_done_.store(0);
    stop_requested_.store(false);

    // Start the clock at the first write
    start_time_ = std::chrono::steady_clock::now();

    // Spawn writer threads
    for (int i = 0; i < cfg_.num_writer_threads; ++i) {
        workers_.emplace_back(&WorkloadRunner::WriterThread, this, i);
    }

    // Optionally spawn reader threads for mixed workloads
    int num_readers = 0;
    if (cfg_.type == WorkloadConfig::Type::readwhilewriting ||
        cfg_.type == WorkloadConfig::Type::ycsba) {
        // Use one reader thread per writer thread (50/50 target)
        num_readers = cfg_.num_writer_threads;
        for (int i = 0; i < num_readers; ++i) {
            workers_.emplace_back(&WorkloadRunner::ReaderThread, this,
                                  cfg_.num_writer_threads + i);
        }
    }

    // Wait for all writers to finish (readers exit via stop_requested_)
    // Writers self-terminate when ops_done_ hits num_ops.
    // We poll with a short sleep to detect the writer completion,
    // then set stop_requested_ to stop any reader threads.
    for (int i = 0; i < cfg_.num_writer_threads; ++i) {
        workers_[i].join();
    }

    auto write_end = std::chrono::steady_clock::now();
    stop_requested_.store(true);  // signal reader threads to stop

    // Wait for reader threads
    for (int i = cfg_.num_writer_threads;
         i < static_cast<int>(workers_.size()); ++i) {
        workers_[i].join();
    }
    workers_.clear();

    // Check if we hit the safety timeout
    bool timed_out = ops_done_.load() < cfg_.num_ops;

    double elapsed_s = std::chrono::duration<double>(
        write_end - start_time_).count();

    // Drain: wait for background compaction to finish.
    // WaitForCompactOptions::flush (not flush_before_wait) is the field name
    // in RocksDB >= 8.x; flush_before_wait was renamed in earlier versions.
    auto drain_start = std::chrono::steady_clock::now();
    rocksdb::WaitForCompactOptions wco;
    wco.flush = true;  // flush memtable first, then wait
    rocksdb::Status drain_s = db_->WaitForCompact(wco);
    if (!drain_s.ok()) {
        std::cerr << "[Workload] WaitForCompact failed: "
                  << drain_s.ToString() << "\n";
    }
    auto drain_end = std::chrono::steady_clock::now();
    double drain_s_time = std::chrono::duration<double>(
        drain_end - drain_start).count();

    RunResult result;
    // Cap at num_ops: with multiple writer threads, ops_done_ can overshoot
    // by at most (num_writer_threads - 1).
    result.ops_written = std::min(ops_done_.load(), cfg_.num_ops);
    result.elapsed_s   = elapsed_s;
    result.drain_s     = drain_s_time;
    result.timed_out   = timed_out;
    return result;
}

// ── RunAsync / WaitDone ───────────────────────────────────────────────────────

void WorkloadRunner::RunAsync() {
    async_started_.store(true);
    workers_.emplace_back([this]() {
        async_result_ = Run();
    });
}

RunResult WorkloadRunner::WaitDone() {
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    return async_result_;
}

} // namespace adoc
