#include "metrics_collector.h"
#include <algorithm>

namespace adoc {

MetricsCollector::MetricsCollector(EngineParams p) : p_(p) {}

double MetricsCollector::flush_efficiency(uint64_t batch_size) {
    double f = (double)batch_size / (double)(64ULL * MB);
    return std::min(2.0, 0.6 + 0.4 * f);
}

Metrics MetricsCollector::collect(const TuningKnobs& k, double input_bytes, double tw) {
    const double in_rate      = (tw > 0.0) ? input_bytes / tw : 0.0;
    const double per_thread   = p_.device_bw / std::max(1, k.total_threads);
    const double flush_eff    = flush_efficiency(k.batch_size);
    const double flush_rate   = k.flush_threads * per_thread * flush_eff;
    const double l0l1_rate    = p_.device_bw * p_.l0l1_bw_fraction;
    const double deep_rate    = std::max(0, k.compaction_threads()) * per_thread
                                * p_.deep_efficiency;
    const double sst_bytes    = (double)k.batch_size;

    mem_ += (in_rate - flush_rate) * tw;
    if (mem_ < 0.0) mem_ = 0.0;
    const double mem_limit = 2.0 * (double)k.batch_size;
    const bool mmo = mem_ > mem_limit;
    if (mmo) mem_ = mem_limit;

    const double flushed = std::min(flush_rate, in_rate) * tw;
    l0_ += flushed / sst_bytes;
    l0_ -= (l0l1_rate * tw) / sst_bytes;
    if (l0_ < 0.0) l0_ = 0.0;

    redun_ += (l0l1_rate * tw) * p_.redundancy_ratio - deep_rate * tw;
    if (redun_ < 0.0) redun_ = 0.0;

    Metrics m;
    m.state.active_memtable_full_before_flush = mmo;
    m.state.num_l0_files = (int)l0_;
    m.state.redundant_data_bytes = (uint64_t)redun_;
    m.buffered_bytes = mem_;
    m.flush_rate = flush_rate;
    m.l0l1_rate  = l0l1_rate;
    m.deep_rate  = deep_rate;

    const double used_bw = std::min(p_.device_bw, flush_rate + l0l1_rate + deep_rate);
    m.disk_percent = std::min(100.0, (p_.device_bw > 0.0 ? used_bw / p_.device_bw * 100.0 : 0.0));
    m.cpu_percent  = std::min(100.0, (p_.max_threads > 0
                        ? (double)k.total_threads / p_.max_threads * 100.0 : 0.0));
    return m;
}

}
