#ifndef ROCKSDB_METRICS_H
#define ROCKSDB_METRICS_H

#include "adoc_tuner.h"

namespace rocksdb { class DB; }

namespace adoc {

class RocksDbMetrics {
public:

    RocksDbMetrics(rocksdb::DB* db, double active_full_ratio = 0.9);

    SystemState collect() const;

private:
    rocksdb::DB* db_;
    double       active_full_ratio_;
};

}

#endif
