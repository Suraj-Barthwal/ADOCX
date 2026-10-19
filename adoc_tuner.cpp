#include "adoc_tuner.h"

namespace adoc {

const char* to_string(Overflow o) {
    switch (o) {
        case Overflow::MMO: return "MMO";
        case Overflow::L0O: return "L0O";
        case Overflow::RDO: return "RDO";
        default:            return "None";
    }
}

AdocTuner::AdocTuner(TuningKnobs init, TunerConfig cfg)
    : knobs_(init), cfg_(cfg) {
    rebalance_flush_threads();
}

void AdocTuner::increase_threads() {
    knobs_.total_threads += cfg_.thread_add_step;
    if (knobs_.total_threads > cfg_.max_threads) knobs_.total_threads = cfg_.max_threads;
}
void AdocTuner::decrease_threads() {
    knobs_.total_threads /= 2;
    if (knobs_.total_threads < cfg_.min_threads) knobs_.total_threads = cfg_.min_threads;
}
void AdocTuner::increase_batch() {
    knobs_.batch_size += cfg_.batch_add_step;
    if (knobs_.batch_size > cfg_.max_batch) knobs_.batch_size = cfg_.max_batch;
}
void AdocTuner::decrease_batch() {
    knobs_.batch_size /= 2;
    if (knobs_.batch_size < cfg_.min_batch) knobs_.batch_size = cfg_.min_batch;
}

void AdocTuner::rebalance_flush_threads() {
    int flush = knobs_.total_threads / cfg_.flush_divisor;
    if (flush < 1) flush = 1;
    if (knobs_.total_threads > 1 && flush > knobs_.total_threads - 1)
        flush = knobs_.total_threads - 1;
    if (knobs_.total_threads == 1) flush = 1;
    knobs_.flush_threads = flush;
}

TuningKnobs AdocTuner::tune(const SystemState& s) {

    const bool mmo = s.active_memtable_full_before_flush;
    const bool l0o = s.num_l0_files > cfg_.l0_file_threshold;
    const bool rdo = s.redundant_data_bytes > cfg_.rdo_bytes_threshold;

    if (l0o) {
        increase_threads();
        last_action_ = Overflow::L0O;
    } else if (rdo) {
        increase_threads();
        decrease_batch();
        last_action_ = Overflow::RDO;
    } else if (mmo) {
        decrease_threads();
        increase_batch();
        last_action_ = Overflow::MMO;
    } else {
        last_action_ = Overflow::None;
    }

    rebalance_flush_threads();
    return knobs_;
}

}
