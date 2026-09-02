#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "adocx/adoc_config.h"
#include "adocx/kv_store.h"

namespace adocx {

enum class Tier { kCold, kHot };

struct AdocStats {
  std::size_t reads = 0;
  std::size_t writes = 0;
  std::size_t hit_hot = 0;
  std::size_t hit_cold = 0;
  std::size_t misses = 0;
  std::size_t promotions = 0;
  std::size_t demotions = 0;
};

class AdocDb {
 public:
  explicit AdocDb(std::unique_ptr<KVStore> store, AdocConfig config = {});

  Status Put(const std::string& key, const std::string& value);
  std::optional<std::string> Get(const std::string& key);
  Status Delete(const std::string& key);

  AdocStats stats() const { return stats_; }
  Tier tier_of(const std::string& key) const;

 private:
  struct Meta {
    Tier tier = Tier::kCold;
    double score = 0.0;
  };

  std::string HotKey(const std::string& key) const;
  std::string ColdKey(const std::string& key) const;
  void Touch(const std::string& key, bool hit);
  void Promote(const std::string& key, const std::string& value);
  void DemoteOneIfNeeded();
  void MaybeTune();

  std::unique_ptr<KVStore> store_;
  AdocConfig config_;
  AdocStats stats_;
  std::unordered_map<std::string, Meta> meta_;
  std::size_t hot_entries_ = 0;
  std::size_t ops_since_tune_ = 0;
};

class MemoryKVStore final : public KVStore {
 public:
  Status Put(const std::string& key, const std::string& value) override;
  std::optional<std::string> Get(const std::string& key) override;
  Status Delete(const std::string& key) override;

 private:
  std::unordered_map<std::string, std::string> data_;
};

#if defined(ADOCX_HAS_ROCKSDB)
std::unique_ptr<KVStore> NewRocksDbStore(const std::string& path, bool create_if_missing = true);
#endif

}  // namespace adocx
