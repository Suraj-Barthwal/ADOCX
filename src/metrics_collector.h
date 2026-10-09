#pragma once
// metrics_collector.h
// ─────────────────────────────────────────────────────────────────────────────
// Samples RocksDB internal state every Tw=1 second (§3 of the ADOC paper).
// The paper calls this the "Monitoring" step that feeds the Overflow Detector.
//
// We use:
//   - db->GetProperty()  for per-CF counters (L0 files, immutable MTs, etc.)
//   - db->GetAggregatedIntProperty() for DB-wide values
//   - RocksDB Statistics tickers for cumulative byte counters
//   - getrusage() for process CPU / peak RSS
// ─────────────────────────────────────────────────────────────────────────────

#include <rocksdb/db.h>
#include <rocksdb/statistics.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace adoc {

// ── Snapshot of one sampling window ──────────────────────────────────────────
struct MetricSnapshot {
    // Wall-clock time since run start (seconds)
    double elapsed_s{0.0};

    // Progress
    uint64_t ops_done{0};
    uint64_t num_ops{0};       // total target ops (for % display)
    double   progress_pct{0.0};

    // Throughput
    double   throughput_kops{0.0};   // ops/s in the last window (÷1000)

    // RocksDB internals
    uint64_t l0_files{0};
    uint64_t pending_compaction_mb{0};
    uint64_t immutable_memtables{0};
    bool     flush_pending{false};

    // Dynamic parameters (as currently set)
    int      cur_threads{0};
    double   cur_batch_mb{0.0};

    // Stall state (string tag from EventListener or "none")
    std::string stall_state{"none"};

    // Last ADOC action description
    std::string last_action{"-"};

    // Write latency histogram bucket counts [<1ms, 1-5ms, 5-10ms, 10-50ms,
    //                                         50-100ms, 100-500ms, >500ms]
    std::array<uint64_t, 7> lat_buckets{};

    // Cumulative bytes for write-amplification tracking
    uint64_t flush_bytes{0};
    uint64_t compact_bytes{0};
    uint64_t user_bytes_ingested{0};  // = ops_done * (key_sz + value_sz)

    // CPU time (user + sys) and peak RSS in bytes
    double   cpu_user_s{0.0};
    double   cpu_sys_s{0.0};
    uint64_t peak_rss_kb{0};
};

// ── Options passed in at construction ─────────────────────────────────────────
struct CollectorConfig {
    uint64_t    num_ops{5'000'000};
    uint32_t    key_size{16};
    uint32_t    value_size{1000};
    int         sample_interval_ms{1000};  // Tw = 1 second (paper default)
    std::string csv_path;                  // empty → no file output
};

// ── Main collector class ───────────────────────────────────────────────────────
class MetricsCollector {
public:
    explicit MetricsCollector(rocksdb::DB* db,
                              std::shared_ptr<rocksdb::Statistics> stats,
                              const CollectorConfig& cfg);
    ~MetricsCollector();

    // Start/stop the background sampling thread
    void Start();
    void Stop();

    // Called by the workload to count completed ops
    void RecordOp(uint64_t count = 1);

    // Called by the ADOC tuner to record an action in the next snapshot
    void SetLastAction(const std::string& action);

    // Called by the stall listener to push the current stall tag
    void SetStallState(const std::string& state);

    // Called by the workload to record a write latency sample (nanoseconds)
    void RecordLatency(uint64_t ns);

    // Called by the tuner to reflect current parameter values
    void SetCurrentParams(int threads, double batch_mb);

    // Returns a copy of the most-recently collected snapshot
    MetricSnapshot Latest() const;

    // Full history (for final summary)
    std::vector<MetricSnapshot> History() const;

    // Drain (final) snapshot call – can be called from the main thread after
    // the workload is done and the sampling thread is stopped.
    MetricSnapshot CollectOnce();

private:
    void SamplingLoop();
    MetricSnapshot DoCollect(double elapsed_s, double throughput_kops);
    void WriteCSVHeader();
    void WriteCSVRow(const MetricSnapshot& s);

    rocksdb::DB*                          db_;
    std::shared_ptr<rocksdb::Statistics>  stats_;
    CollectorConfig                       cfg_;

    std::atomic<uint64_t>  ops_done_{0};
    std::atomic<uint64_t>  prev_ops_{0};   // for per-window delta
    std::string            stall_state_{"none"};
    std::string            last_action_{"-"};
    int                    cur_threads_{0};
    double                 cur_batch_mb_{0.0};
    mutable std::mutex     state_mu_;

    // Latency histogram – simple fixed buckets (nanoseconds thresholds)
    // Buckets: [0,1ms) [1,5ms) [5,10ms) [10,50ms) [50,100ms) [100,500ms) [500ms+]
    static constexpr std::array<uint64_t,6> LAT_EDGES{
        1'000'000ULL, 5'000'000ULL, 10'000'000ULL,
        50'000'000ULL, 100'000'000ULL, 500'000'000ULL
    };
    mutable std::mutex                  lat_mu_;
    std::array<uint64_t, 7>             lat_counts_{};

    std::chrono::steady_clock::time_point start_time_;
    std::thread            thread_;
    std::atomic<bool>      running_{false};
    std::vector<MetricSnapshot> history_;
    mutable std::mutex          history_mu_;
    MetricSnapshot              latest_;
    mutable std::mutex          latest_mu_;

    std::ofstream csv_file_;
};

} // namespace adoc
