# ADOC — C++ Reproduction (USENIX FAST '23)

A C++ reproduction of the control algorithm from:

> Jinghuan Yu, Sam H. Noh, Young-ri Choi, Chun Jason Xue.
> **"ADOC: Automatically Harmonizing Dataflow Between Components in
> Log-Structured Key-Value Stores for Improved Performance."**
> USENIX FAST 2023.

ADOC suppresses **write stalls** in LSM-tree key-value stores (like RocksDB) by
monitoring the engine once per second, detecting which component is overflowing,
and automatically retuning two knobs — **background thread count** and **batch
size** — using an **AIMD** control policy (the probe-up / back-off-fast idea from
TCP congestion control).

| Overflow | Trigger | ADOC action |
|----------|---------|-------------|
| **MMO** (memory)     | active MemTable fills before flush | fewer threads, bigger batch |
| **L0O** (level-0)    | `#L0 files > 20` | more threads |
| **RDO** (redundancy) | pending compaction bytes `> 64 GB` | more threads, smaller batch |

This repo has **two parts that share one verified tuner** (`adoc_tuner`):

1. **Simulation** (`adoc_cpp/`) — the tuner driving a synthetic LSM dataflow
   model. Builds anywhere with `g++`; proves the algorithm, numbers are modeled.
2. **Real integration** (`rocksdb_adoc/`) — the *same* tuner driving an **actual
   RocksDB**. Numbers are measured. Needs RocksDB + a C++17 compiler.

---

## Repository layout

```
adoc_cpp/
├── adoc_tuner.{h,cpp}        # the tuner (shared by both parts) + control types
├── metrics_collector.{h,cpp} # synthetic engine (simulation)
├── stall_listener.{h,cpp}    # stall classifier (simulation)
├── workload.{h,cpp}          # fillrandom-style write generator (simulation)
├── main.cpp                  # simulation entry point (./adoc, ./adoc --test)
├── Makefile
└── rocksdb_adoc/             # ── REAL RocksDB integration ──
    ├── rocksdb_metrics.{h,cpp}        # reads live RocksDB properties
    ├── rocksdb_stall_listener.{h,cpp} # real rocksdb::EventListener
    ├── main.cpp                       # real driver (opens a DB, runs workload)
    ├── CMakeLists.txt
    └── build_wsl.sh                   # one-shot WSL build script
```

---

## Part 1 — Simulation (runs anywhere, no RocksDB)

### Prerequisites
- A C++14 compiler (`g++` or `clang++`). On Windows, MinGW works.

### Install & build
```bash
git clone <your-repo-url>
cd <repo>/adoc_cpp
g++ -std=c++14 -O2 -Wall -Wextra \
    adoc_tuner.cpp metrics_collector.cpp stall_listener.cpp workload.cpp main.cpp \
    -o adoc
```
(With `make` installed, just run `make` instead.)

### Run
```bash
./adoc            # control-loop simulation over device profiles
./adoc --test     # 23 conformance tests (verifies the tuner vs. the paper)
```
On Windows/PowerShell the binary is `adoc.exe` — run it as `.\adoc.exe`.

Expected: `--test` prints `Passed 23, Failed 0`; the run prints a per-second
tuning trace and a final stall summary for each simulated device.

---

## Part 2 — Real RocksDB integration

The real build needs RocksDB and a C++17 compiler. **On Windows, use WSL/Ubuntu**
(old MinGW cannot build RocksDB). Linux/macOS work directly.

### Prerequisites (Ubuntu / WSL)
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake librocksdb-dev \
    libsnappy-dev liblz4-dev libzstd-dev libbz2-dev zlib1g-dev
```

### Install & build
```bash
git clone <your-repo-url>
cd <repo>/adoc_cpp/rocksdb_adoc
bash build_wsl.sh          # installs deps (if missing) + builds via CMake
```
Or build manually:
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j"$(nproc)"
```

### Run
```bash
# ADOC enabled:
./build/adoc_rocksdb /tmp/adoc_on 60 --writers 4

# baseline (fixed RocksDB defaults) for comparison:
./build/adoc_rocksdb /tmp/adoc_off 60 --writers 4 --no-adoc
```
Arguments: `./adoc_rocksdb [db_path] [seconds] [--writers N] [--no-adoc]`.

Each run prints a per-second tuning trace and a final `RESULT` block with real
throughput and real write-stall counts. Compare the `write stalls:` lines of the
two runs — ADOC should drive stall time toward zero.

### Example measured result (RocksDB 9.11, WSL, 60 s, 4 writers)
```
===== RESULT (ADOC) =====
ops=8283833  avg throughput=138.1 kOps/s
write stalls: delayed=0  stopped=0  stalled=0.00s

===== RESULT (RocksDB-default) =====
ops=7960260  avg throughput=132.7 kOps/s
write stalls: delayed=5  stopped=0  stalled=15.63s
```
ADOC eliminated the write stalls (15.63 s → 0 s). Throughput is close and noisy
on a fast disk; for a report, run each config 3× and average (as the paper does).

---

## Which part am I looking at?

- `#include <rocksdb/...>` in a file → **real** integration. No RocksDB include →
  **simulation**.
- Build links `-lrocksdb` (CMake) → real. Plain `g++` with no library → simulation.
- Program banner says `control-loop simulation (synthetic ...)` → simulation;
  `ADOC on RocksDB | db=...` → real.

---

## Notes
- This is an independent reproduction for study; not affiliated with the original
  authors.
- The simulation's device bandwidths and the demo RDO threshold are illustrative,
  not a performance benchmark. Paper-faithful numbers require the real integration
  on the paper's hardware/workload.
```
