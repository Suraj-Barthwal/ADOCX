#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "adocx/adoc_db.h"

using adocx::AdocConfig;
using adocx::AdocDb;
using adocx::MemoryKVStore;
using adocx::Tier;

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

void TestPromotesHotKeys() {
  AdocConfig cfg;
  cfg.promote_threshold = 0.2;
  cfg.hot_set_capacity = 10;
  cfg.enable_auto_tuning = false;

  AdocDb db(std::make_unique<MemoryKVStore>(), cfg);
  auto st = db.Put("k1", "v1");
  Expect(st.ok, "put should succeed");

  auto v1 = db.Get("k1");
  Expect(v1.has_value() && *v1 == "v1", "value should be readable");
  Expect(db.tier_of("k1") == Tier::kHot, "key should be promoted to hot tier");
}

void TestDemotesWhenCapacityExceeded() {
  AdocConfig cfg;
  cfg.promote_threshold = 0.2;
  cfg.demote_threshold = 0.5;
  cfg.hot_set_capacity = 1;
  cfg.enable_auto_tuning = false;

  AdocDb db(std::make_unique<MemoryKVStore>(), cfg);
  Expect(db.Put("a", "1").ok, "put a");
  Expect(db.Get("a").has_value(), "get a");

  Expect(db.Put("b", "2").ok, "put b");
  Expect(db.Get("b").has_value(), "get b");

  const bool a_hot = db.tier_of("a") == Tier::kHot;
  const bool b_hot = db.tier_of("b") == Tier::kHot;
  Expect(a_hot != b_hot, "exactly one key should remain hot when capacity is 1");
}

void TestDeleteRemovesAllTiers() {
  AdocConfig cfg;
  cfg.promote_threshold = 0.2;
  cfg.enable_auto_tuning = false;

  AdocDb db(std::make_unique<MemoryKVStore>(), cfg);
  Expect(db.Put("x", "1").ok, "put x");
  Expect(db.Get("x").has_value(), "get x before delete");
  Expect(db.Delete("x").ok, "delete x");
  Expect(!db.Get("x").has_value(), "x should not exist after delete");
}

}  // namespace

int main() {
  TestPromotesHotKeys();
  TestDemotesWhenCapacityExceeded();
  TestDeleteRemovesAllTiers();
  std::cout << "All ADOC tests passed\n";
  return 0;
}
