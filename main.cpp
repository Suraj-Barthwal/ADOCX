#include "adoc_tuner.h"
#include "metrics_collector.h"
#include "stall_listener.h"
#include "workload.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace adoc;

static const double MBf = 1024.0 * 1024.0;
static const double GBf = 1024.0 * 1024.0 * 1024.0;

struct Profile {
    const char* name;
    double device_bw;
    double input_rate;
};

static void run_profile(const Profile& prof) {
    EngineParams ep;
    ep.device_bw = prof.device_bw;
    MetricsCollector collector(ep);

    TunerConfig tcfg;

    tcfg.rdo_bytes_threshold = 8ULL * GB;
    AdocTuner tuner(TuningKnobs{}, tcfg);

    StallListener listener({20, tcfg.rdo_bytes_threshold});

    WorkloadConfig wc;
    wc.name = "fillrandom";
    wc.input_rate = prof.input_rate;
    wc.windows = 200;
    Workload workload(wc);

    std::printf("\n=== %-9s  device=%.0f MB/s  input=%.0f MB/s  (%s) ===\n",
                prof.name, prof.device_bw / MBf, prof.input_rate / MBf, wc.name);
    std::printf("%4s | %8s %8s | %6s %4s %7s | %4s %4s | act   stall\n",
                "t", "thr(f/c)", "batch", "mem", "L0", "redun", "cpu", "dsk");
    std::printf("-----+-------------------+---------------------+-----------+-----\n");

    const double tw = tcfg.tw_seconds;
    int quiet = 0;
    for (int t = 0; t < workload.windows(); ++t) {
        TuningKnobs k = tuner.knobs();
        double in_bytes = workload.input_bytes(t, tw);

        Metrics m = collector.collect(k, in_bytes, tw);
        StallType st = listener.on_window(m.state, tw);
        tuner.tune(m.state);
        Overflow act = tuner.last_action();
        TuningKnobs nk = tuner.knobs();

        bool changed = (nk.total_threads != k.total_threads) ||
                       (nk.batch_size != k.batch_size);
        if (t == 0 || changed || act != Overflow::None) {
            std::printf("%4d | %2d(%d/%d) %10.0fMB | %5.0fMB %4d %6.1fGB | %3.0f%% %3.0f%% | %s  %s\n",
                        t, k.total_threads, k.flush_threads, k.compaction_threads(),
                        k.batch_size / MBf, m.buffered_bytes / MBf,
                        m.state.num_l0_files, m.state.redundant_data_bytes / GBf,
                        m.cpu_percent, m.disk_percent,
                        to_string(act), to_string(st));
        }

        if (act == Overflow::None && !changed) {
            if (++quiet >= 25) { std::printf("%4d | ... stabilized ...\n", t); break; }
        } else {
            quiet = 0;
        }
    }

    const StallStats& ss = listener.stats();
    TuningKnobs f = tuner.knobs();
    std::printf("final knobs: %d threads (%d flush / %d compaction), batch=%.0fMB\n",
                f.total_threads, f.flush_threads, f.compaction_threads(), f.batch_size / MBf);
    std::printf("stalls: MT=%d (%.0fs)  L0=%d (%.0fs)  PS=%d (%.0fs)  total=%d (%.0fs)\n",
                ss.mt_count, ss.mt_sec, ss.l0_count, ss.l0_sec,
                ss.ps_count, ss.ps_sec, ss.total_count(), ss.total_sec());
}

static int run_simulation() {
    std::printf("ADOC control-loop simulation\n");
    std::printf("(synthetic dataflow model -- illustrative, not a RocksDB benchmark)\n");
    Profile profiles[] = {
        { "SATA-HDD", 210.0  * MBf, 260.0  * MBf },
        { "SATA-SSD", 490.0  * MBf, 520.0  * MBf },
        { "NVMe-SSD", 2700.0 * MBf, 2900.0 * MBf },
    };
    for (const auto& p : profiles) run_profile(p);
    return 0;
}

static int g_pass = 0, g_fail = 0;
static void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", name.c_str()); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", name.c_str()); }
}

static int run_tests() {
    std::printf("ADOC conformance tests\n======================\n");

    {
        AdocTuner t(TuningKnobs{8, 2, 128 * MB});
        SystemState s; s.active_memtable_full_before_flush = true;
        auto r = t.tune(s);
        check(t.last_action() == Overflow::MMO, "MMO detected");
        check(r.total_threads == 4, "MMO halves threads (8 -> 4)");
        check(r.batch_size == 192 * MB, "MMO adds 64MB batch (128 -> 192)");
    }

    {
        AdocTuner t(TuningKnobs{4, 1, 128 * MB});
        SystemState s; s.num_l0_files = 21;
        auto r = t.tune(s);
        check(t.last_action() == Overflow::L0O, "L0O detected (>20 files)");
        check(r.total_threads == 6, "L0O adds 2 threads (4 -> 6)");
        check(r.batch_size == 128 * MB, "L0O leaves batch unchanged");
    }
    {
        AdocTuner t(TuningKnobs{});
        SystemState s; s.num_l0_files = 20;
        t.tune(s);
        check(t.last_action() == Overflow::None, "no L0O at exactly 20");
    }

    {
        AdocTuner t(TuningKnobs{4, 1, 256 * MB});
        SystemState s; s.redundant_data_bytes = 65 * GB;
        auto r = t.tune(s);
        check(t.last_action() == Overflow::RDO, "RDO detected (>64GB)");
        check(r.total_threads == 6, "RDO adds 2 threads (4 -> 6)");
        check(r.batch_size == 128 * MB, "RDO halves batch (256 -> 128)");
    }

    {
        AdocTuner t(TuningKnobs{4, 1, 128 * MB});
        SystemState s;
        s.active_memtable_full_before_flush = true;
        s.num_l0_files = 30;
        s.redundant_data_bytes = 100 * GB;
        auto r = t.tune(s);
        check(t.last_action() == Overflow::L0O, "priority: L0O over RDO & MMO");
        check(r.total_threads == 6 && r.batch_size == 128 * MB, "priority: only L0O applied");
    }
    {
        AdocTuner t(TuningKnobs{8, 2, 128 * MB});
        SystemState s;
        s.active_memtable_full_before_flush = true;
        s.redundant_data_bytes = 70 * GB;
        auto r = t.tune(s);
        check(t.last_action() == Overflow::RDO, "priority: RDO over MMO");
        check(r.total_threads == 10 && r.batch_size == 64 * MB, "priority: only RDO applied");
    }

    {
        AdocTuner t(TuningKnobs{2, 1, 64 * MB});
        SystemState s; s.num_l0_files = 25;
        auto r = t.tune(s);
        check(r.total_threads == 4 && r.flush_threads == 1, "flush=1 at 4 threads");
        s.num_l0_files = 25; r = t.tune(s);
        check(r.flush_threads == 1, "flush=1 at 6 threads");
        s.num_l0_files = 25; r = t.tune(s);
        check(r.total_threads == 8 && r.flush_threads == 2, "flush=2 at 8 threads");
    }

    {
        AdocTuner t(TuningKnobs{2, 1, 512 * MB});
        SystemState s; s.active_memtable_full_before_flush = true;
        auto r = t.tune(s);
        check(r.batch_size == 512 * MB, "batch capped at 512MB");
        check(r.total_threads == 1, "threads floored at min (2/2=1)");
    }

    {
        StallListener l;
        SystemState mt; mt.active_memtable_full_before_flush = true;
        check(l.on_window(mt, 1.0) == StallType::Memtable, "listener: MMO -> MT-stall");
        SystemState l0; l0.num_l0_files = 25;
        check(l.on_window(l0, 1.0) == StallType::Level0, "listener: L0O -> L0-stall");
        SystemState ps; ps.redundant_data_bytes = 100 * GB;
        check(l.on_window(ps, 1.0) == StallType::PendingSize, "listener: RDO -> PS-stall");
        check(l.stats().total_count() == 3, "listener: counted 3 distinct stalls");
    }

    std::printf("----------------------\nPassed %d, Failed %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--test") == 0) return run_tests();
    return run_simulation();
}
