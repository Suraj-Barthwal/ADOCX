#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "adocx/adoc_db.h"

using adocx::AdocConfig;
using adocx::AdocDb;
using adocx::KVStore;
using adocx::MemoryKVStore;

struct Args {
  std::size_t key_count = 10000;
  std::size_t operations = 200000;
  double read_ratio = 0.8;
  uint64_t seed = 7;
  std::string output_csv = "benchmark_results.csv";
};

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    std::string token = argv[i];
    if (token == "--keys" && i + 1 < argc) args.key_count = std::stoull(argv[++i]);
    if (token == "--ops" && i + 1 < argc) args.operations = std::stoull(argv[++i]);
    if (token == "--read-ratio" && i + 1 < argc) args.read_ratio = std::stod(argv[++i]);
    if (token == "--seed" && i + 1 < argc) args.seed = static_cast<uint64_t>(std::stoull(argv[++i]));
    if (token == "--out" && i + 1 < argc) args.output_csv = argv[++i];
  }
  return args;
}

std::size_t SkewedKey(std::mt19937_64& rng, std::size_t key_count) {
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const double u = unit(rng);
  const double skewed = std::pow(u, 4.0);
  return std::min<std::size_t>(key_count - 1, static_cast<std::size_t>(skewed * static_cast<double>(key_count)));
}

template <typename Fn>
double MeasureSeconds(Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  fn();
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(end - start).count();
}

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);

  std::vector<std::string> keys;
  keys.reserve(args.key_count);
  for (std::size_t i = 0; i < args.key_count; ++i) {
    keys.push_back("k" + std::to_string(i));
  }

  std::mt19937_64 rng(args.seed);
  std::uniform_real_distribution<double> ratio_pick(0.0, 1.0);

  auto baseline = std::make_unique<MemoryKVStore>();
  AdocConfig cfg;
  cfg.hot_set_capacity = std::max<std::size_t>(64, args.key_count / 20);
  cfg.promote_threshold = 0.6;
  cfg.demote_threshold = 0.4;
  AdocDb adoc(std::make_unique<MemoryKVStore>(), cfg);

  for (const auto& k : keys) {
    const auto v = "value-" + k;
    baseline->Put(k, v);
    adoc.Put(k, v);
  }

  const double baseline_seconds = MeasureSeconds([&] {
    for (std::size_t i = 0; i < args.operations; ++i) {
      const std::string& key = keys[SkewedKey(rng, args.key_count)];
      if (ratio_pick(rng) < args.read_ratio) {
        (void)baseline->Get(key);
      } else {
        baseline->Put(key, "value-" + key + "-u" + std::to_string(i));
      }
    }
  });

  rng.seed(args.seed);
  const double adoc_seconds = MeasureSeconds([&] {
    for (std::size_t i = 0; i < args.operations; ++i) {
      const std::string& key = keys[SkewedKey(rng, args.key_count)];
      if (ratio_pick(rng) < args.read_ratio) {
        (void)adoc.Get(key);
      } else {
        adoc.Put(key, "value-" + key + "-u" + std::to_string(i));
      }
    }
  });

  const auto stats = adoc.stats();

  std::ofstream out(args.output_csv, std::ios::trunc);
  out << "variant,seconds,ops,throughput_ops_per_sec,hot_hits,cold_hits,misses,promotions,demotions\n";
  out << "baseline," << baseline_seconds << ',' << args.operations << ','
      << (static_cast<double>(args.operations) / baseline_seconds) << ",0,0,0,0,0\n";
  out << "adoc," << adoc_seconds << ',' << args.operations << ','
      << (static_cast<double>(args.operations) / adoc_seconds) << ',' << stats.hit_hot << ','
      << stats.hit_cold << ',' << stats.misses << ',' << stats.promotions << ',' << stats.demotions << '\n';

  std::cout << "Benchmark completed. Results written to " << args.output_csv << '\n';
  return 0;
}
