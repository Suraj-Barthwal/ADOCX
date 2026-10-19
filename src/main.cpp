#include "rocksdb_metrics.h"
#include "rocksdb_stall_listener.h"
#include "adoc_tuner.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/env.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static const double MBf = 1024.0 * 1024.0;
static const double GBf = 1024.0 * 1024.0 * 1024.0;

static void apply_knobs(rocksdb::DB* db, rocksdb::Env* env, const adoc::TuningKnobs& k) {

    db->SetOptions({{"write_buffer_size", std::to_string(k.batch_size)}});

    db->SetDBOptions({{"max_background_jobs", std::to_string(k.total_threads)}});

    env->SetBackgroundThreads(k.flush_threads < 1 ? 1 : k.flush_threads,
                              rocksdb::Env::HIGH);
    int comp = k.compaction_threads() < 1 ? 1 : k.compaction_threads();
    env->SetBackgroundThreads(comp, rocksdb::Env::LOW);
}

int main(int argc, char** argv) {
    std::string db_path = "/tmp/adoc_rocksdb";
    int seconds = 60;
    int writers = 2;
    bool use_adoc = true;

    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-adoc") use_adoc = false;
        else if (a == "--writers" && i + 1 < argc) writers = std::atoi(argv[++i]);
        else pos.push_back(a);
    }
    if (pos.size() > 0) db_path = pos[0];
    if (pos.size() > 1) seconds = std::atoi(pos[1].c_str());

    std::printf("ADOC on RocksDB  |  db=%s  seconds=%d  writers=%d  adoc=%s\n",
                db_path.c_str(), seconds, writers, use_adoc ? "ON" : "OFF");

    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64ULL << 20;
    options.max_write_buffer_number = 2;
    options.max_background_jobs = 2;
    options.level0_slowdown_writes_trigger = 20;
    options.level0_stop_writes_trigger = 36;

    auto listener = std::make_shared<adoc::AdocStallListener>();
    options.listeners.push_back(listener);
    rocksdb::Env* env = rocksdb::Env::Default();
    options.env = env;

    rocksdb::DB* db = nullptr;
    rocksdb::Status st = rocksdb::DB::Open(options, db_path, &db);
    if (!st.ok()) {
        std::fprintf(stderr, "DB::Open failed: %s\n", st.ToString().c_str());
        return 1;
    }

    adoc::AdocTuner tuner(adoc::TuningKnobs{});
    adoc::RocksDbMetrics metrics(db);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> ops{0};

    std::thread control;
    if (use_adoc) {
        control = std::thread([&]() {
            int t = 0;
            while (!stop.load()) {
                adoc::SystemState agg;
                for (int i = 0; i < 10 && !stop.load(); ++i) {
                    std::this_thread::sleep_for(100ms);
                    adoc::SystemState s = metrics.collect();
                    agg.active_memtable_full_before_flush |= s.active_memtable_full_before_flush;
                    if (s.num_l0_files > agg.num_l0_files) agg.num_l0_files = s.num_l0_files;
                    if (s.redundant_data_bytes > agg.redundant_data_bytes)
                        agg.redundant_data_bytes = s.redundant_data_bytes;
                }
                if (stop.load()) break;
                adoc::TuningKnobs k = tuner.tune(agg);
                apply_knobs(db, env, k);
                std::printf("[t%4d] thr=%d(f%d/c%d) batch=%4lluMB | L0=%d pend=%.1fGB | act=%s\n",
                            t++, k.total_threads, k.flush_threads, k.compaction_threads(),
                            (unsigned long long)(k.batch_size / (1ULL << 20)),
                            agg.num_l0_files, agg.redundant_data_bytes / GBf,
                            adoc::to_string(tuner.last_action()));
                std::fflush(stdout);
            }
        });
    }

    auto writer_fn = [&](int id) {
        std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)id);
        std::string value(1000, 'x');
        rocksdb::WriteOptions wo;
        char key[17];
        while (!stop.load()) {
            unsigned long long r = (unsigned long long)rng();
            std::snprintf(key, sizeof(key), "%016llx", r);
            rocksdb::Status ws = db->Put(wo, rocksdb::Slice(key, 16), value);
            if (ws.ok()) ops.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> ws;
    for (int i = 0; i < writers; ++i) ws.emplace_back(writer_fn, i);

    uint64_t last = 0;
    for (int t = 0; t < seconds; ++t) {
        std::this_thread::sleep_for(1s);
        uint64_t now = ops.load();
        std::printf("  (throughput t%4d: %.1f kOps/s)\n", t, (now - last) / 1000.0);
        std::fflush(stdout);
        last = now;
    }

    stop.store(true);
    for (auto& w : ws) w.join();
    if (control.joinable()) control.join();

    const adoc::RealStallStats& ss = listener->stats();
    uint64_t total = ops.load();
    std::printf("\n===== RESULT (%s) =====\n", use_adoc ? "ADOC" : "RocksDB-default");
    std::printf("ops=%llu  avg throughput=%.1f kOps/s\n",
                (unsigned long long)total, total / (double)seconds / 1000.0);
    std::printf("write stalls: delayed=%ld  stopped=%ld  stalled=%.2fs  (flushes=%ld compactions=%ld)\n",
                ss.delayed_count.load(), ss.stopped_count.load(),
                ss.stalled_ms.load() / 1000.0,
                ss.flushes.load(), ss.compactions.load());

    rocksdb::FlushOptions fo;
    db->Flush(fo);
    delete db;
    return 0;
}
