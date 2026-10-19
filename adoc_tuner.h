#ifndef ADOC_TUNER_H
#define ADOC_TUNER_H

#include <cstdint>

namespace adoc {

constexpr uint64_t KB = 1024ULL;
constexpr uint64_t MB = 1024ULL * KB;
constexpr uint64_t GB = 1024ULL * MB;

enum class Overflow { None, MMO, L0O, RDO };
const char* to_string(Overflow o);

struct SystemState {
    bool     active_memtable_full_before_flush = false;
    int      num_l0_files = 0;
    uint64_t redundant_data_bytes = 0;
};

struct TuningKnobs {
    int      total_threads = 2;
    int      flush_threads = 1;
    uint64_t batch_size    = 64ULL * MB;

    int compaction_threads() const { return total_threads - flush_threads; }
};

struct TunerConfig {
    int      l0_file_threshold   = 20;
    uint64_t rdo_bytes_threshold = 64ULL * GB;
    int      thread_add_step = 2;
    uint64_t batch_add_step  = 64ULL * MB;
    int      min_threads = 1;
    int      max_threads = 32;
    uint64_t min_batch = 64ULL  * MB;
    uint64_t max_batch = 512ULL * MB;
    int      flush_divisor = 4;
    double   tw_seconds = 1.0;
};

class AdocTuner {
public:
    AdocTuner(TuningKnobs init, TunerConfig cfg = TunerConfig());

    TuningKnobs tune(const SystemState& s);

    const TuningKnobs& knobs() const { return knobs_; }
    Overflow last_action() const { return last_action_; }
    const TunerConfig& config() const { return cfg_; }

private:

    void increase_threads();
    void decrease_threads();
    void increase_batch();
    void decrease_batch();
    void rebalance_flush_threads();

    TuningKnobs knobs_;
    TunerConfig cfg_;
    Overflow    last_action_ = Overflow::None;
};

}

#endif
