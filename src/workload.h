#pragma once
// workload.h
// ─────────────────────────────────────────────────────────────────────────────
// Deterministic workload generator.
//
// Design goals (from the FAIR COMPARISON requirement):
//
//   1. FIXED-DATA termination: both default and ADOC modes stop after exactly
//      --num-ops writes, never after a fixed wall-clock duration.
//      Duration is only a safety timeout (--max-duration).
//
//   2. SAME SEED: std::mt19937_64 seeded with a caller-supplied uint64_t.
//      Two generators with the same seed produce the EXACT same key sequence.
//      Thread seed = base_seed + thread_id to keep multi-threaded runs
//      reproducible while preserving the same key distribution.
//
//   3. Workload types: fillrandom, readwhilewriting, ycsb-a.
//
//   4. Disk-space pre-flight: before opening the DB, check that free space
//      on the target filesystem is at least 3× the ingested data + 20%
//      headroom. Abort with a clear message if not.
// ─────────────────────────────────────────────────────────────────────────────

#include <rocksdb/db.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace adoc {

// ── WorkloadConfig ─────────────────────────────────────────────────────────────
struct WorkloadConfig {
    // Termination
    uint64_t num_ops{5'000'000};            // stop after this many writes
    int      max_duration_s{3600};          // safety timeout (secondary)

    // Key/value
    uint32_t key_size{16};
    uint32_t value_size{1000};

    // Reproducibility (FAIR COMPARISON §seed)
    uint64_t seed{42};

    // Concurrency
    int num_writer_threads{1};              // 1 for cleanest determinism

    // Workload type
    enum class Type { fillrandom, readwhilewriting, ycsba };
    Type type{Type::fillrandom};

    // Read ratio for readwhilewriting / ycsb-a
    double read_ratio{0.5};   // ycsb-a: 50% read, 50% write (paper default)

    // Column family (default CF only for this implementation)
    // db path is owned by the caller
};

// ── RunResult ─────────────────────────────────────────────────────────────────
// Returned after the workload finishes.
struct RunResult {
    uint64_t ops_written{0};
    uint64_t ops_read{0};
    double   elapsed_s{0.0};    // wall-clock from first write to last ack
    double   drain_s{0.0};      // time for WaitForCompact after last write
    bool     timed_out{false};  // true if safety timeout fired before num_ops
};

// ── Key generator ─────────────────────────────────────────────────────────────
// Given a 64-bit PRNG value, fills a fixed-length key buffer with hex digits.
// Same value → same bytes, so same seed → same sequence.
std::string MakeKey(uint64_t rng_value, uint32_t key_size);

// ── Disk-space check ──────────────────────────────────────────────────────────
// Returns true if at least (required_bytes * 1.2) bytes are free on the
// filesystem containing path. Prints a human-readable error if not.
bool CheckDiskSpace(const std::string& path, uint64_t required_bytes);

// ── WorkloadRunner ────────────────────────────────────────────────────────────
class WorkloadRunner {
public:
    // on_op_done: called after each completed operation with (op_count)
    //   – used to feed MetricsCollector::RecordOp
    // on_latency: called with write latency in nanoseconds
    //   – used to feed MetricsCollector::RecordLatency
    using OpCallback  = std::function<void(uint64_t)>;
    using LatCallback = std::function<void(uint64_t)>;

    WorkloadRunner(rocksdb::DB* db,
                   const WorkloadConfig& cfg,
                   OpCallback  on_op_done = nullptr,
                   LatCallback on_latency = nullptr);

    // Blocking call – returns when num_ops reached or timeout fires.
    // Signals all writer threads then calls db->WaitForCompact.
    RunResult Run();

    // Non-blocking: start writers in background, then return.
    // Use WaitDone() to block until completion.
    void RunAsync();
    RunResult WaitDone();

    // Interrupt from another thread (e.g. Ctrl+C handler)
    void RequestStop();

private:
    void WriterThread(int thread_id);
    void ReaderThread(int thread_id);

    rocksdb::DB*   db_;
    WorkloadConfig cfg_;
    OpCallback     on_op_done_;
    LatCallback    on_latency_;

    std::atomic<uint64_t>  ops_done_{0};
    std::atomic<bool>      stop_requested_{false};
    std::chrono::steady_clock::time_point start_time_;

    std::vector<std::thread> workers_;

    // For WaitDone()
    std::atomic<bool> async_started_{false};
    RunResult         async_result_;
};

} // namespace adoc
