#include "rocksdb_metrics.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>

namespace adoc {

RocksDbMetrics::RocksDbMetrics(rocksdb::DB* db, double active_full_ratio)
    : db_(db), active_full_ratio_(active_full_ratio) {}

SystemState RocksDbMetrics::collect() const {
    SystemState s;
    auto* cf = db_->DefaultColumnFamily();

    uint64_t num_l0 = 0;
    db_->GetIntProperty(cf, "rocksdb.num-files-at-level0", &num_l0);
    s.num_l0_files = static_cast<int>(num_l0);

    uint64_t pending = 0;
    db_->GetIntProperty(cf, "rocksdb.estimate-pending-compaction-bytes", &pending);
    s.redundant_data_bytes = pending;

    uint64_t num_imm = 0, active_size = 0;
    db_->GetIntProperty(cf, "rocksdb.num-immutable-mem-table", &num_imm);
    db_->GetIntProperty(cf, "rocksdb.cur-size-active-mem-table", &active_size);

    const auto opts = db_->GetOptions(cf);
    const uint64_t write_buffer = opts.write_buffer_size;
    const int      max_imm = opts.max_write_buffer_number;
    const bool active_nearly_full =
        write_buffer > 0 &&
        active_size >= static_cast<uint64_t>(active_full_ratio_ * write_buffer);

    s.active_memtable_full_before_flush =
        (num_imm >= max_imm - 1) || (num_imm >= 1 && active_nearly_full);
    return s;
}

}
