# Building the real ADOC-on-RocksDB integration

This directory is the **real** version: the verified ADOC tuner
(`../adoc_tuner.cpp`) driving an actual RocksDB instance. It needs RocksDB and a
C++17 compiler.

## Why not your current MinGW?

Your Windows toolchain is **MinGW GCC 6.3.0 (2016)**, which is C++14-era and
cannot build or link RocksDB. There is no quick fix for that compiler. You have
two realistic options; **WSL is by far the easiest.**

---

## Option A — WSL / Ubuntu (recommended)

You already have WSL 2 with Ubuntu installed. RocksDB is one `apt` package there.

1. Open an Ubuntu (WSL) terminal. Reach these files via the Windows drive mount:

   ```bash
   cd "/mnt/g/My Drive/ADOCX/adoc_cpp/rocksdb_adoc"
   ```

   > Tip: building on the `/mnt/g` (Google Drive) path works but is slow. For
   > speed, copy the `adoc_cpp` folder into the Linux filesystem first, e.g.
   > `cp -r "/mnt/g/My Drive/ADOCX/adoc_cpp" ~/adoc_cpp && cd ~/adoc_cpp/rocksdb_adoc`.

2. Build (installs RocksDB + tools, then compiles):

   ```bash
   bash build_wsl.sh
   ```

   Or do it by hand:

   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential cmake librocksdb-dev \
       libsnappy-dev liblz4-dev libzstd-dev libbz2-dev zlib1g-dev
   mkdir -p build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release && make -j"$(nproc)"
   ```

3. Run it:

   ```bash
   ./adoc_rocksdb /tmp/adocdb 60 --writers 4            # ADOC tuner ON
   ./adoc_rocksdb /tmp/adocdb_base 60 --writers 4 --no-adoc   # baseline (fixed defaults)
   ```

   Compare the two runs' `write stalls:` and `throughput` lines to see what the
   tuner does.

### Single g++ command (no CMake)

```bash
g++ -std=c++17 -O2 -I.. \
    main.cpp rocksdb_metrics.cpp rocksdb_stall_listener.cpp ../adoc_tuner.cpp \
    -lrocksdb -lpthread -ldl -lz -lbz2 -lsnappy -llz4 -lzstd \
    -o adoc_rocksdb
```

---

## Option B — Windows with MSVC + vcpkg

If you prefer native Windows, install Visual Studio (MSVC) and vcpkg, then:

```powershell
vcpkg install rocksdb:x64-windows
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

---

## What the program does

| Piece | File | Real RocksDB API used |
|-------|------|------------------------|
| Tuner (unchanged) | `../adoc_tuner.cpp` | — (pure algorithm) |
| Metrics collector | `rocksdb_metrics.cpp` | `GetIntProperty("rocksdb.num-files-at-level0" / "estimate-pending-compaction-bytes" / "num-immutable-mem-table" / "cur-size-active-mem-table")`, `GetOptions()` |
| Stall listener | `rocksdb_stall_listener.cpp` | `rocksdb::EventListener::OnStallConditionsChanged / OnFlushCompleted / OnCompactionCompleted` |
| Knob application | `main.cpp` | `SetOptions({{"write_buffer_size",...}})`, `SetDBOptions({{"max_background_jobs",...}})`, `Env::SetBackgroundThreads(..., HIGH/LOW)` |
| Workload | `main.cpp` | `DB::Put` (16 B key, 1000 B value, random) |

Each second the control thread reads real metrics, runs the tuner, and retunes
the live DB — this is the actual mechanism from the paper, not a simulation.

## Note on API versions

Written against the stable RocksDB 6.x / 7.x C++ API (what `librocksdb-dev`
ships on current Ubuntu). If your RocksDB is much older/newer and a symbol name
differs (e.g. the `WriteStallCondition` enumerators), the compiler error will
point right at it; the fix is a one-line rename.
