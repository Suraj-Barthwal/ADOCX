# ADOCX

ADOCX is a C++ implementation of an **ADOC-style architecture on top of RocksDB** with a reproducible benchmark harness.

> Note: this repository contains an implementation template that maps the ADOC paper's architecture into practical RocksDB integration points (hot/cold tiering, adaptive promotion/demotion, and feedback tuning). The code is organized so the core ADOC logic can be tested independently from the storage backend.

## 1) Paper-oriented architecture mapping

The implementation follows a 3-layer architecture:

1. **Storage Layer (`KVStore`)**  
   Abstract key-value backend. Includes:
   - `MemoryKVStore` (used for deterministic tests/benchmarks)
   - `RocksDB` adapter (`NewRocksDbStore`) when RocksDB is available

2. **ADOC Control Layer (`AdocDb`)**  
   Implements the ADOC control loop:
   - access tracking via EMA score
   - promotion from cold tier to hot tier when score exceeds threshold
   - demotion when hot tier exceeds capacity and score falls below demote threshold
   - adaptive threshold tuning based on observed hot-hit rate

3. **Evaluation Layer (`benchmarks/adoc_benchmark.cpp`)**  
   Reproducible workload generator and CSV output for comparing baseline KV access against ADOC tiering.

## 2) Core algorithms implemented

- **Access scoring**: Exponential moving average per key (`score = (1-alpha)*score + alpha*signal`)
- **Promotion policy**: promote cold key when `score >= promote_threshold`
- **Demotion policy**: if hot set exceeds capacity, demote lowest-score hot key when score is below `demote_threshold`
- **Adaptive tuning**: periodically adjust promotion threshold according to hot-tier hit-rate feedback

## 3) Repository layout

- `include/adocx/*` — public interfaces and ADOC data structures
- `src/adoc_db.cpp` — ADOC implementation + in-memory + RocksDB backend adapter
- `tests/test_adoc.cpp` — focused unit tests for ADOC policies
- `benchmarks/adoc_benchmark.cpp` — reproducible benchmark executable
- `scripts/build.sh` — reproducible build script
- `scripts/run_experiments.sh` — reproducible experiment runner

## 4) Build and test

```bash
scripts/build.sh
ctest --test-dir build --output-on-failure
```

If RocksDB headers/libs are not installed, build continues with memory backend only.

To force no RocksDB lookup:

```bash
cmake -S . -B build -DADOCX_ENABLE_ROCKSDB=OFF
cmake --build build -j
```

## 5) Run reproducible benchmark

```bash
scripts/run_experiments.sh
```

Result file:

- `results/adoc_reproducible.csv`

CSV columns:

- `variant`
- `seconds`
- `ops`
- `throughput_ops_per_sec`
- `hot_hits`, `cold_hits`, `misses`, `promotions`, `demotions`

## 6) Interpreting results

- Compare `throughput_ops_per_sec` between `baseline` and `adoc`.
- Use `hot_hits/cold_hits` to validate that ADOC moves frequently used keys to hot tier.
- Use `promotions/demotions` to study policy dynamics and tune thresholds/capacity.

## 7) Extending toward a full paper reproduction

To align with a specific ADOC paper setup, tune these parameters in `AdocConfig` and benchmark arguments:

- `hot_set_capacity`
- `promote_threshold`
- `demote_threshold`
- `ema_alpha`
- workload mix (`--read-ratio`, key count, operation count, seed)

This lets you encode the exact paper settings while preserving deterministic experiment reproducibility.
