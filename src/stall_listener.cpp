// stall_listener.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Implements the EventListener that RocksDB calls whenever a write-stall
// condition changes (§3.1 of the ADOC paper – overflow detection via stalls).
//
// RocksDB's WriteStallCondition is one of:
//   kNormal    – no stall
//   kDelayed   – writes are being throttled (soft limit hit)
//   kStopped   – writes are fully stopped (hard limit hit)
//
// The reason for the stall lives in WriteStallInfo::condition.prev and .cur.
// We use the CF-level stall info (column_family_name, condition) to classify
// the event into our three overflow types (MMO / L0O / RDO).
// ─────────────────────────────────────────────────────────────────────────────

#include "stall_listener.h"

// WriteStallInfo and WriteStallCondition are defined in rocksdb/listener.h,
// not in a separate write_stall.h header (which does not exist in RocksDB 8+).
#include <rocksdb/listener.h>

#include <chrono>
#include <iostream>

namespace adoc {

// ── Constructor ───────────────────────────────────────────────────────────────

StallListener::StallListener(StateChangeCb cb) : cb_(std::move(cb)) {}

// ── ConditionToTag ─────────────────────────────────────────────────────────────
// Maps RocksDB's WriteStallCondition enum to a human-readable tag that also
// encodes the overflow type:
//   l0  → L0O (Level-0 file count overflow)
//   ps  → RDO (Pending-compaction-bytes / RDB overflow)
//   mt  → MMO (Memtable overflow)
// "delay" = writes throttled; "stop" = writes fully stalled.

std::string StallListener::ConditionToTag(rocksdb::WriteStallCondition c) {
    using WS = rocksdb::WriteStallCondition;
    switch (c) {
        case WS::kNormal:  return "none";
        // L0 file-count triggers
        case WS::kDelayed: return "delay";   // generic delayed (needs context)
        case WS::kStopped: return "stop";    // generic stopped (needs context)
        default:           return "unknown";
    }
}

// ── OnStallConditionsChanged ──────────────────────────────────────────────────
// Called by RocksDB on the thread that changes the stall condition.
// We must return quickly – no heavy work here.

void StallListener::OnStallConditionsChanged(
    const rocksdb::WriteStallInfo& info) {

    const auto now = std::chrono::steady_clock::now();

    // Determine the new state tag.
    // RocksDB does not expose the *reason* for the stall in the public
    // WriteStallInfo struct (it only gives the new WriteStallCondition).
    // We infer the type from the CF-level properties checked in the detector,
    // but here we record the raw condition so the detector can look at it.
    using WS = rocksdb::WriteStallCondition;

    std::string new_tag;
    switch (info.condition.cur) {
        case WS::kNormal:  new_tag = "none";    break;
        case WS::kDelayed: new_tag = "delayed"; break;
        case WS::kStopped: new_tag = "stopped"; break;
        default:           new_tag = "unknown"; break;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);

        // Close the most recent open event (if any)
        if (!events_.empty() && events_.back().active) {
            events_.back().end_time = now;
            events_.back().active   = false;
        }

        current_tag_ = new_tag;

        // Open a new event unless we just transitioned to "none"
        if (new_tag != "none") {
            StallEvent ev;
            ev.condition  = new_tag;
            ev.start_time = now;
            ev.active     = true;
            events_.push_back(ev);
        }
    }

    // Invoke the user callback (e.g. to push the new state to MetricsCollector)
    if (cb_) cb_(new_tag);
}

// ── CurrentState ─────────────────────────────────────────────────────────────

std::string StallListener::CurrentState() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_tag_;
}

// ── Events ────────────────────────────────────────────────────────────────────

std::vector<StallEvent> StallListener::Events() const {
    std::lock_guard<std::mutex> lk(mu_);
    return events_;
}

// ── GetStats ──────────────────────────────────────────────────────────────────
// Computes aggregated stall statistics from the event log.
// We use simple duration counting – the paper measures total stall seconds and
// stall count by type, which is exactly what this returns.
//
// Type classification: since WriteStallInfo does not expose the reason, we
// rely on the tuner's overflow detector to annotate events with the type via
// the SetStallState(type+tag) call on MetricsCollector.
// For statistics purposes, the events recorded here are tagged generically
// ("delayed" / "stopped"); we split counts and duration by type in the tuner
// where we have property context.
//
// For now: all delayed/stopped events contribute to a single total_stall_s.
// The tuner's own action log provides per-type breakdown.

StallStats StallListener::GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);

    StallStats ss;
    ss.current = current_tag_;

    const auto now = std::chrono::steady_clock::now();

    for (const auto& ev : events_) {
        auto end = ev.active ? now : ev.end_time;
        double dur_s = std::chrono::duration<double>(end - ev.start_time).count();

        ss.total_stall_s += dur_s;

        // Map tag prefix to type bucket
        if (ev.condition.find("delayed") != std::string::npos ||
            ev.condition.find("stop")    != std::string::npos) {
            // We can't distinguish L0/PS/MT from the generic tag alone here;
            // the ADOC tuner annotates the richer tag via MetricsCollector.
            // In the listener we count everything as "ps" (pending / stall)
            // to populate the primary stall counter. The tuner overrides this.
            ss.count_ps++;
            ss.dur_ps_s += dur_s;
        }
    }

    return ss;
}

} // namespace adoc
