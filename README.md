# ADOC-Rocks: Dynamic Write-Stall Mitigation in RocksDB
### Phase 2 — CLI Benchmark with Fixed-Data Fair Comparison

---

## Table of Contents
1. [What This Project Does](#what-this-project-does)
2. [Paper-Section Mapping](#paper-section-mapping)
3. [Prerequisites](#prerequisites)
4. [Install RocksDB](#install-rocksdb)
5. [Build](#build)
6. [Quick Start](#quick-start)
7. [CLI Reference](#cli-reference)
8. [Data-Size Presets](#data-size-presets)
9. [Threshold Profiles](#threshold-profiles)
10. [Fair Comparison Design](#fair-comparison-design)
11. [Provoke Stalls (Baseline Gate)](#provoke-stalls-baseline-gate)
12. [Understanding the Output](#understanding-the-output)
13. [Running the Full Experiment](#running-the-full-experiment)
14. [Unit Tests](#unit-tests)
15. [Limitations and Differences from the Paper](#limitations-and-differences-from-the-paper)

---

## What This Project Does

This project implements a CLI version of **ADOC** (Automatically Harmonizing
Dataflow Between Components) on top of the public RocksDB API.  It has two
modes:

| Mode | Description |
|---|---|
| `default` | Plain RocksDB with no dynamic tuning. |
| `adoc` | ADOC tuner runs every 1 second, detects overflows, and adjusts background threads and memtable size using AIMD logic. |

Both modes write **the same fixed number of operations** (not the same wall-clock
duration), making the comparison inherently fair.

---

## Paper-Section Mapping

| Paper Section | Implementation |
|---|---|
| §3 Monitoring (Tw = 1s) | `MetricsCollector` – samples every 1 s |
| §3.1 Overflow detection (MMO / L0O / RDO) | `AdocTuner::DetectOverflow()` |
| §4.2 Priority: L0O > RDO > MMO | `AdocTuner::DetectOverflow()` ordering |
| §4.3 AIMD logic | `AdocTuner::ApplyAction()` |
| §4.4 Dynamic Adjustment | `ApplyThreads()` / `ApplyBatchMb()` using `SetDBOptions` / `SetOptions` |
| §5 Stall detection via EventListener | `StallListener::OnStallConditionsChanged()` |

---

## Prerequisites

| Requirement | Version |
|---|---|
| OS | Ubuntu 22.04 (or WSL2 on Windows 11) |
| g++ | ≥ 10 (C++17) |
| CMake | ≥ 3.16 |
| RocksDB | 7.x or 8.x |
| zlib | any current |

> **WSL2 note:** keep `--db-path` inside the Linux filesystem (e.g.
> `/home/<user>/adocdb`), **not** `/mnt/c`.  `/mnt/c` is 5–20× slower
> and will hide stalls.

---

## Install RocksDB

### Ubuntu / WSL2 (recommended)
```bash
sudo apt update
sudo apt install -y librocksdb-dev libsnappy-dev zlib1g-dev \
                    liblz4-dev libzstd-dev cmake build-essential
```

### Build from source (if the apt version is too old)
```bash
git clone https://github.com/facebook/rocksdb.git
cd rocksdb
git checkout v8.11.4          # or the latest stable tag
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DROCKSDB_BUILD_SHARED=OFF \
      -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON \
      -DWITH_TESTS=OFF -DWITH_TOOLS=OFF ..
make -j$(nproc)
sudo make install
```

---

## Build

```bash
# From the project root
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

The binary is `build/adocrocks`.  Tests are in `build/adoc_tests`.

---

## Quick Start

```bash
# 1. Check free disk space first
df -h /tmp

# 2. Sanity check – default mode, 5 M ops, laptop profile
./build/adocrocks run --mode default --size small --db-path /tmp/adocdb_default

# 3. ADOC mode with the same seed
./build/adocrocks run --mode adoc --size small --seed 42 --db-path /tmp/adocdb_adoc

# 4. Full fair comparison (3 repetitions, same data, alternating order)
./build/adocrocks compare --size small --seed 42 --repeat 3 --db-path /tmp/adocdb

# 5. Print report from a saved CSV
./build/adocrocks report --csv results_default_s42.csv
```

---

## CLI Reference

### `run`

```
adocrocks run [options]
```

| Flag | Default | Description |
|---|---|---|
| `--mode {default\|adoc}` | `default` | Which mode to run |
| `--workload {fillrandom\|readwhilewriting\|ycsb-a}` | `fillrandom` | Workload type |
| `--profile {paper\|laptop}` | `laptop` | Threshold profile |
| `--num-ops N` | 5 000 000 | Fixed number of write ops |
| `--total-gb G` | — | Alternative: ingest G GB (converted to ops) |
| `--size {small\|medium\|large}` | — | Preset (overrides `--num-ops`) |
| `--seed N` | 42 | PRNG seed (printed in CSV header) |
| `--max-duration N` | 3600 | Safety timeout in seconds (NOT the normal stop) |
| `--threads N` | 4 | Initial `max_background_jobs` |
| `--batch-mb N` | 64 | Initial `write_buffer_size` in MB |
| `--db-path PATH` | `/tmp/adocdb` | DB directory (wiped before run) |
| `--value-size N` | 1000 | Bytes per value |
| `--key-size N` | 16 | Bytes per key |
| `--writer-threads N` | 1 | Number of writer goroutines |
| `--csv PATH` | auto | CSV output file |
| `--l0-slowdown N` | 10 | L0 slowdown threshold (laptop profile) |
| `--l0-stop N` | 20 | L0 stop threshold (laptop profile) |
| `--pending-soft-gb N` | 1 | Pending compaction soft limit GB |
| `--pending-hard-gb N` | 2 | Pending compaction hard limit GB |

### `compare`

All `run` flags plus:

| Flag | Default | Description |
|---|---|---|
| `--repeat N` | 3 | Number of default+adoc repetition pairs |

### `report`

```
adocrocks report --csv FILE
```

---

## Data-Size Presets

Each operation is `key_size + value_size = 16 + 1000 = 1016 bytes`.
The DB can occupy 2–3× the ingested data due to space amplification.

| Preset | Ingest | `--num-ops` | Free disk needed | Approx. time / run |
|---|---|---|---|---|
| `small` | ~4.7 GB | 5 000 000 | ~20 GB | 2–5 min |
| `medium` | ~18.9 GB | 20 000 000 | ~75 GB | 10–25 min |
| `large` | ~47.2 GB | 50 000 000 | ~180 GB | 30–90 min |

**Guidance:**
- Use `small` during development to confirm stalls appear.
- Use `medium` for final results.
- With ≤ 8 GB RAM, `small` is enough to saturate the page cache.
- With 16 GB+ RAM, go straight to `medium`.
- The binary checks free disk space before each run and aborts if
  `free_space < 3× ingest + 20%`.

---

## Threshold Profiles

### `--profile laptop` (default)

Scaled for a single consumer-grade machine with limited RAM and SSD:

| Knob | Value |
|---|---|
| L0 slowdown trigger | 10 files |
| L0 stop trigger | 20 files |
| Pending compaction soft limit | 1 GB |
| Pending compaction hard limit | 2 GB |

### `--profile paper`

The original paper's values (designed for Optane + NVMe servers, 64+ GB data):

| Knob | Value |
|---|---|
| L0 slowdown trigger | 20 files |
| L0 stop trigger | 36 files |
| Pending compaction soft limit | 64 GB |
| Pending compaction hard limit | 128 GB |

> The paper thresholds will **never trigger** on small data sizes.
> Use `--profile laptop` for development and final results on a standard machine.

---

## Fair Comparison Design

> **This is the most important design choice in the project.**

The ADOC paper compared both schemes over a **fixed 3600-second window**.
Because ADOC improves throughput, it ingested 47–67% more data than the
baseline in that window.  This means the paper's CPU-time and PS-stall numbers
for ADOC are inflated relative to the baseline, because ADOC simply did more
work.

This project fixes the data size instead:

| Aspect | This Project |
|---|---|
| Stop condition | After exactly `--num-ops` writes (not a fixed time) |
| Seed | Paired per repetition: rep *i* uses seed `42 + i − 1` for **both** modes |
| Key sequence | Identical: same seed → same `mt19937_64` → same bytes |
| Key/value sizes | Same |
| RocksDB settings | Same (compression, block cache, WAL, bloom filters) |
| Starting parameters | Same (initial threads, initial batch size) |
| DB directory | Freshly wiped before each run |
| Run order | Alternating: [default, adoc], [adoc, default], [default, adoc] |
| Timer | Starts at first write, stops at last write ACK; drain is separate |
| Drain time | `WaitForCompact` after last write, reported separately |

**What this means for your results:**  
If ADOC reduces the total wall-clock time or stall duration for the same data,
that is a clear improvement.  If it doesn't, the trade-off analysis
(CPU, space amplification) is still visible and scientifically honest.

---

## Provoke Stalls (Baseline Gate)

Before adding the ADOC tuner, you must confirm that **default RocksDB shows
write stalls** at your chosen data size.  If the throughput never drops and
`stall_count > 0` never appears, ADOC has nothing to fix.

### Checklist

1. **Data size larger than RAM.**  If your machine has 16 GB RAM, use
   `--size medium` (≈19 GB ingest).  The OS page cache will absorb the
   writes if the data fits in RAM.

2. **Low L0 thresholds.**  The default laptop profile sets `l0_slowdown=10`.
   If your SSD is very fast, lower it to 4–6:
   ```bash
   ./build/adocrocks run --mode default --size small \
       --l0-slowdown 4 --l0-stop 8 \
       --pending-soft-gb 256 --pending-hard-gb 512
   ```
   (pending-soft/hard in MB if you pass the value in MB; check the CLI.)

3. **Fewer background jobs.**  Start with `--threads 2`.  A high thread count
   keeps compaction ahead of writes and prevents L0 build-up.

4. **Smaller memtable.**  Try `--batch-mb 16` to make flushes more frequent,
   which stresses the L0 → L1 compaction pipeline.

5. **Increase `--num-ops`.**  More data = more compaction pressure.  If `small`
   shows no stalls, try `medium`.

---

## Understanding the Output

### Live terminal display (refreshed every second)

```
  t(s)   prog%      kOps/s  L0    pend_MB   imm   thds  batch_MB       stall  last_action
──────────────────────────────────────────────────────────────────────────────
  12.1    2.4%        41.3   3        128     0      4      64.0        none   -
  13.1    2.7%        37.1  11        512     2      4      64.0    l0_stall   L0O threads:4->6
```

| Column | Meaning |
|---|---|
| `t(s)` | Elapsed seconds since first write |
| `prog%` | ops\_done / num\_ops × 100 |
| `kOps/s` | Write throughput in the last 1-second window |
| `L0` | Number of L0 SST files |
| `pend_MB` | Estimated pending compaction bytes in MB |
| `imm` | Number of immutable memtables waiting to flush |
| `thds` | Current `max_background_jobs` |
| `batch_MB` | Current `write_buffer_size` in MB |
| `stall` | Current stall tag (none / l0\_stall / rdo\_stall / mmo\_stall / delayed / stopped) |
| `last_action` | Last ADOC tuner action |

### Final summary

After the workload ends, a summary block is printed with:
- **Primary metrics**: completion time, average throughput (σ), total stall
  seconds (% of runtime), stall counts by type, p99 / max write latency.
- **Secondary metrics**: write amplification, space amplification, CPU time,
  peak RSS, drain time.
- **ADOC-specific**: tuner action counts by overflow type.

### Compare table

The `compare` subcommand adds a side-by-side table:

```
Metric                       Default (mean±σ)       ADOC (mean±σ)         Improvement
────────────────────────────────────────────────────────────────────────────────────
completion_time(s)           142.50±3.20s           118.30±2.80s          +16.9% better
stall_total(s)               22.10±1.50s            8.40±0.90s            +62.0% better
...
```

`+N% better` means ADOC is N% better on that metric.
`−N% worse` means ADOC is worse (e.g. higher CPU is expected if ADOC does more
compaction to keep L0 clear).

---

## Running the Full Experiment

```bash
# Development (quick sanity check, ~5–15 min total)
./scripts/run_experiments.sh --size small --repeat 3

# Final results (~1–2.5 hours total on a medium-speed SSD)
./scripts/run_experiments.sh --size medium --repeat 3

# With cache flush between runs (requires sudo or root)
sudo ./scripts/run_experiments.sh --size medium --repeat 3
```

CSVs are written to `./experiment_results/`.  Each file is named
`results_<mode>_rep<N>_s<seed>.csv`.

---

## Unit Tests

```bash
cd build
ctest --output-on-failure
# or run directly:
./adoc_tests
```

Test categories:

| Test suite | What it covers |
|---|---|
| `KeyGenerator.*` | Same seed → same key sequence; different seeds diverge; key length always equals `key_size` |
| `OverflowPriority.*` | L0O > RDO > MMO; boundary conditions; MMO requires `flush_pending` |
| `AIMD.*` | Correct thread/batch deltas for each overflow type; min/max clamping |
| `StallListener.*` | State transitions; event recording; callback invocation |
| `MakeKey.*` | Consistent mapping; hex-only output; zero value |

---

## Limitations and Differences from the Paper

> **These must be stated in any academic submission using this code.**

### 1. Fixed-data instead of fixed-time comparison

The ADOC paper (USENIX FAST 2023) compared both schemes over a **fixed
3600-second window**.  Because ADOC improves throughput it ingested
**47–67% more data** than SILK-O in the same period.  As a result, the
paper's ADOC numbers for PS stalls and CPU time look worse than the
baseline — not because ADOC is worse, but because it did more work.

This project uses a **fixed data size** (`--num-ops`) for both modes.  This
makes the comparison fairer at the cost of direct comparability with the
paper's numbers.

### 2. Different hardware

The paper used **Optane Persistent Memory (PM)** as the write path and
**NVMe SSD** for storage on multi-socket servers.  Consumer SSDs and HDDs
behave very differently.  Expect smaller absolute gains.

### 3. Scaled thresholds

The paper's soft pending-compaction limit is 64 GB.  On a standard laptop
or desktop with 5–50 GB data, this threshold would never trigger.  The
`--profile laptop` settings (1 GB soft, 2 GB hard) are calibrated to
provoke stalls at small data sizes.

### 4. Public API only

The paper's implementation modifies RocksDB internals.  This project uses
only the **public API** (`SetDBOptions`, `SetOptions`, `EventListener`,
`GetProperty`, `Statistics`).  Some fine-grained controls available in the
paper's implementation are not accessible here.

### 5. RocksDB version

The paper's experiments used RocksDB **7.5.3**.  This project is compatible
with RocksDB 7.x and 8.x.  Default values and internal scheduling
heuristics differ across versions.

### 6. Smaller data size

A 5–50 GB ingest is significantly smaller than the paper's experimental
scale.  A smaller but real reduction in completion time or stall duration
is a valid result.  Little or no gain is also valid — it may indicate that
the hardware bandwidth is the bottleneck, not the write-stall policy.

### 7. Single machine, no NUMA

The paper's tuner accounts for NUMA topology.  This implementation does not.

---

## Expected Results

> Do NOT tune your code to match the paper's numbers.

| What to expect | Why |
|---|---|
| Smaller improvement than the paper | Consumer SSD, smaller data, public API only |
| ADOC CPU time ≥ default CPU time | ADOC does more compaction to keep L0 clear |
| ADOC space amplification ≥ default | More aggressive compaction |
| Stall duration reduced | Primary goal: ADOC reacts to overflows |
| Throughput smoother (lower σ) | Secondary benefit of dynamic tuning |
| No gain with tiny data | Dataset fits in page cache; no real compaction pressure |

If you observe little or no improvement, check:
1. Are stalls actually occurring in the default run?  (`stall_count > 0`)
2. Is the data size larger than your RAM?
3. Are the L0/pending thresholds low enough to trigger stalls?

---

*This README was generated as part of ADOC-Rocks Phase 2. All results must
include the disclaimer above in any submitted report.*
