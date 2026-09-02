#include "adocx/adoc_db.h"

#include <algorithm>
#include <limits>

#if defined(ADOCX_HAS_ROCKSDB)
#include <rocksdb/db.h>
#endif

namespace adocx {

namespace {

constexpr char kHotPrefix[] = "adoc/h/";
constexpr char kColdPrefix[] = "adoc/c/";

}  // namespace

AdocDb::AdocDb(std::unique_ptr<KVStore> store, AdocConfig config)
    : store_(std::move(store)), config_(config) {}

Status AdocDb::Put(const std::string& key, const std::string& value) {
  ++stats_.writes;
  ++ops_since_tune_;

  auto& meta = meta_[key];
  if (meta.tier == Tier::kHot) {
    auto st = store_->Put(HotKey(key), value);
    if (!st.ok) return st;
  } else {
    auto st = store_->Put(ColdKey(key), value);
    if (!st.ok) return st;
  }

  meta.score = (1.0 - config_.ema_alpha) * meta.score + config_.ema_alpha;
  if (meta.tier == Tier::kCold && meta.score >= config_.promote_threshold) {
    Promote(key, value);
  }

  DemoteOneIfNeeded();
  MaybeTune();
  return Status::Ok();
}

std::optional<std::string> AdocDb::Get(const std::string& key) {
  ++stats_.reads;
  ++ops_since_tune_;

  auto hot_val = store_->Get(HotKey(key));
  if (hot_val.has_value()) {
    ++stats_.hit_hot;
    Touch(key, true);
    MaybeTune();
    return hot_val;
  }

  auto cold_val = store_->Get(ColdKey(key));
  if (cold_val.has_value()) {
    ++stats_.hit_cold;
    Touch(key, true);

    auto& meta = meta_[key];
    if (meta.tier == Tier::kCold && meta.score >= config_.promote_threshold) {
      Promote(key, *cold_val);
      DemoteOneIfNeeded();
    }
    MaybeTune();
    return cold_val;
  }

  ++stats_.misses;
  Touch(key, false);
  MaybeTune();
  return std::nullopt;
}

Status AdocDb::Delete(const std::string& key) {
  ++ops_since_tune_;

  auto it = meta_.find(key);
  if (it != meta_.end() && it->second.tier == Tier::kHot && hot_entries_ > 0) {
    --hot_entries_;
  }

  auto st_hot = store_->Delete(HotKey(key));
  if (!st_hot.ok) return st_hot;
  auto st_cold = store_->Delete(ColdKey(key));
  if (!st_cold.ok) return st_cold;

  meta_.erase(key);
  MaybeTune();
  return Status::Ok();
}

Tier AdocDb::tier_of(const std::string& key) const {
  auto it = meta_.find(key);
  if (it == meta_.end()) return Tier::kCold;
  return it->second.tier;
}

std::string AdocDb::HotKey(const std::string& key) const { return std::string(kHotPrefix) + key; }

std::string AdocDb::ColdKey(const std::string& key) const { return std::string(kColdPrefix) + key; }

void AdocDb::Touch(const std::string& key, bool hit) {
  auto& meta = meta_[key];
  const double signal = hit ? 1.0 : 0.0;
  meta.score = (1.0 - config_.ema_alpha) * meta.score + config_.ema_alpha * signal;
}

void AdocDb::Promote(const std::string& key, const std::string& value) {
  auto& meta = meta_[key];
  if (meta.tier == Tier::kHot) return;

  auto st = store_->Put(HotKey(key), value);
  if (!st.ok) return;
  st = store_->Delete(ColdKey(key));
  if (!st.ok) return;

  meta.tier = Tier::kHot;
  ++hot_entries_;
  ++stats_.promotions;
}

void AdocDb::DemoteOneIfNeeded() {
  if (hot_entries_ <= config_.hot_set_capacity) {
    return;
  }

  std::string candidate;
  double min_score = std::numeric_limits<double>::max();
  for (const auto& [key, meta] : meta_) {
    if (meta.tier != Tier::kHot) continue;
    if (meta.score < min_score) {
      min_score = meta.score;
      candidate = key;
    }
  }

  if (candidate.empty()) return;
  if (min_score > config_.demote_threshold) return;

  auto hot_val = store_->Get(HotKey(candidate));
  if (!hot_val.has_value()) return;

  auto st = store_->Put(ColdKey(candidate), *hot_val);
  if (!st.ok) return;
  st = store_->Delete(HotKey(candidate));
  if (!st.ok) return;

  auto it = meta_.find(candidate);
  if (it != meta_.end()) {
    it->second.tier = Tier::kCold;
  }
  --hot_entries_;
  ++stats_.demotions;
}

void AdocDb::MaybeTune() {
  if (!config_.enable_auto_tuning || config_.tuning_interval_ops == 0) {
    return;
  }
  if (ops_since_tune_ < config_.tuning_interval_ops) {
    return;
  }

  const auto total_reads = stats_.hit_hot + stats_.hit_cold + stats_.misses;
  if (total_reads > 0) {
    const double hot_hit_rate = static_cast<double>(stats_.hit_hot) / static_cast<double>(total_reads);
    if (hot_hit_rate < 0.4) {
      config_.promote_threshold = std::max(0.5, config_.promote_threshold * 0.9);
    } else if (hot_hit_rate > 0.8) {
      config_.promote_threshold = std::min(5.0, config_.promote_threshold * 1.05);
    }
  }

  ops_since_tune_ = 0;
}

Status MemoryKVStore::Put(const std::string& key, const std::string& value) {
  data_[key] = value;
  return Status::Ok();
}

std::optional<std::string> MemoryKVStore::Get(const std::string& key) {
  auto it = data_.find(key);
  if (it == data_.end()) {
    return std::nullopt;
  }
  return it->second;
}

Status MemoryKVStore::Delete(const std::string& key) {
  data_.erase(key);
  return Status::Ok();
}

#if defined(ADOCX_HAS_ROCKSDB)

class RocksDbStore final : public KVStore {
 public:
  explicit RocksDbStore(std::unique_ptr<rocksdb::DB> db) : db_(std::move(db)) {}

  Status Put(const std::string& key, const std::string& value) override {
    auto st = db_->Put(rocksdb::WriteOptions(), key, value);
    if (!st.ok()) return Status::Error(st.ToString());
    return Status::Ok();
  }

  std::optional<std::string> Get(const std::string& key) override {
    std::string value;
    auto st = db_->Get(rocksdb::ReadOptions(), key, &value);
    if (st.IsNotFound()) return std::nullopt;
    if (!st.ok()) return std::nullopt;
    return value;
  }

  Status Delete(const std::string& key) override {
    auto st = db_->Delete(rocksdb::WriteOptions(), key);
    if (!st.ok()) return Status::Error(st.ToString());
    return Status::Ok();
  }

 private:
  std::unique_ptr<rocksdb::DB> db_;
};

std::unique_ptr<KVStore> NewRocksDbStore(const std::string& path, bool create_if_missing) {
  rocksdb::Options options;
  options.create_if_missing = create_if_missing;

  rocksdb::DB* raw_db = nullptr;
  auto st = rocksdb::DB::Open(options, path, &raw_db);
  if (!st.ok()) {
    return nullptr;
  }

  return std::make_unique<RocksDbStore>(std::unique_ptr<rocksdb::DB>(raw_db));
}

#endif

}  // namespace adocx
