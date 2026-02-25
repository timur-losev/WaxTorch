#include "waxcpp/live_set_rewrite.hpp"
#include "../test_logger.hpp"

#include <iostream>
#include <string>

namespace {

using namespace waxcpp;
using namespace waxcpp::tests;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* label) {
  if (condition) {
    ++g_pass;
    Log(std::string("  PASS: ") + label);
  } else {
    ++g_fail;
    LogError(std::string("  FAIL: ") + label);
  }
}

// ── Helper: create a default enabled schedule. ──────────────

LiveSetRewriteSchedule DefaultSchedule() {
  LiveSetRewriteSchedule s;
  s.enabled = true;
  s.check_every_flushes = 4;
  s.min_dead_payload_bytes = 1024;
  s.min_dead_payload_fraction = 0.20;
  s.minimum_idle_ms = 5000;
  s.min_interval_ms = 60000;
  return s;
}

MaintenanceGateInput DefaultInput(const LiveSetRewriteSchedule& sched) {
  MaintenanceGateInput in;
  in.schedule = &sched;
  in.flush_count = 4;  // passes cadence gate (4 % 4 == 0)
  in.force = false;
  in.now_ms = 200'000;
  in.last_completed_ms = 100'000;          // 100s ago > 60s cooldown
  in.last_write_activity_ms = 190'000;     // 10s ago > 5s idle
  in.dead_payload_bytes = 2048;            // > 1024 threshold
  in.total_payload_bytes = 8192;           // fraction = 0.25 >= 0.20
  return in;
}

// ============================================================
// 1. Disabled schedule
// ============================================================

void TestDisabled() {
  Log("=== TestDisabled ===");

  // nullptr schedule
  {
    MaintenanceGateInput in{};
    ScheduledLiveSetMaintenanceReport report{};
    bool ok = EvaluateMaintenanceGate(in, report);
    Check(!ok, "nullptr schedule returns false");
    Check(report.outcome == MaintenanceOutcome::kDisabled, "outcome is kDisabled (nullptr)");
  }

  // schedule.enabled == false
  {
    LiveSetRewriteSchedule sched;
    sched.enabled = false;
    MaintenanceGateInput in{};
    in.schedule = &sched;
    ScheduledLiveSetMaintenanceReport report{};
    bool ok = EvaluateMaintenanceGate(in, report);
    Check(!ok, "disabled schedule returns false");
    Check(report.outcome == MaintenanceOutcome::kDisabled, "outcome is kDisabled (disabled)");
  }

  // force + disabled still returns false
  {
    LiveSetRewriteSchedule sched;
    sched.enabled = false;
    MaintenanceGateInput in{};
    in.schedule = &sched;
    in.force = true;
    ScheduledLiveSetMaintenanceReport report{};
    bool ok = EvaluateMaintenanceGate(in, report);
    Check(!ok, "force + disabled still returns false");
  }
}

// ============================================================
// 2. All gates pass
// ============================================================

void TestAllGatesPass() {
  Log("=== TestAllGatesPass ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "all gates pass → eligible");
  Check(report.dead_payload_fraction > 0.0, "fraction computed");
  Check(report.flush_count == in.flush_count, "flush_count copied");
  Check(report.dead_payload_bytes == in.dead_payload_bytes, "dead_payload_bytes copied");
  Check(report.total_payload_bytes == in.total_payload_bytes, "total_payload_bytes copied");
}

// ============================================================
// 3. Cadence gate
// ============================================================

void TestCadenceGate() {
  Log("=== TestCadenceGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // flush_count not a multiple of cadence → skipped
  in.flush_count = 5;
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(!ok, "cadence gate rejects non-multiple");
  Check(report.outcome == MaintenanceOutcome::kCadenceSkipped, "outcome kCadenceSkipped");

  // force bypasses cadence gate
  in.force = true;
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "force bypasses cadence gate");
}

// ============================================================
// 4. Cooldown gate
// ============================================================

void TestCooldownGate() {
  Log("=== TestCooldownGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // now_ms too close to last_completed_ms
  in.last_completed_ms = 150'000;  // 50s ago, cooldown is 60s
  in.now_ms = 200'000;
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(!ok, "cooldown gate rejects too-soon");
  Check(report.outcome == MaintenanceOutcome::kCooldownSkipped, "outcome kCooldownSkipped");

  // force bypasses cooldown gate
  in.force = true;
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "force bypasses cooldown gate");
}

// ============================================================
// 5. Idle gate
// ============================================================

void TestIdleGate() {
  Log("=== TestIdleGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // write activity 2s ago < minimum_idle_ms (5s)
  in.last_write_activity_ms = 198'000;
  in.now_ms = 200'000;
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(!ok, "idle gate rejects recent writes");
  Check(report.outcome == MaintenanceOutcome::kIdleSkipped, "outcome kIdleSkipped");

  // force bypasses idle gate
  in.force = true;
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "force bypasses idle gate");
}

// ============================================================
// 6. Threshold gate (bytes)
// ============================================================

void TestThresholdBytesGate() {
  Log("=== TestThresholdBytesGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // Below both thresholds
  in.dead_payload_bytes = 100;                   // < 1024
  in.total_payload_bytes = 10'000;               // fraction = 0.01 < 0.20
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(!ok, "below both thresholds rejects");
  Check(report.outcome == MaintenanceOutcome::kBelowThreshold, "outcome kBelowThreshold");

  // Meets bytes threshold but not fraction
  in.dead_payload_bytes = 2000;                  // > 1024
  in.total_payload_bytes = 100'000;              // fraction = 0.02 < 0.20
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "meets bytes threshold alone → eligible");
}

// ============================================================
// 7. Threshold gate (fraction)
// ============================================================

void TestThresholdFractionGate() {
  Log("=== TestThresholdFractionGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // Meets fraction but not bytes
  in.dead_payload_bytes = 500;                   // < 1024
  in.total_payload_bytes = 1000;                 // fraction = 0.50 >= 0.20
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "meets fraction threshold alone → eligible");
}

// ============================================================
// 8. Edge: cadence == 1 (every flush)
// ============================================================

void TestCadenceEveryFlush() {
  Log("=== TestCadenceEveryFlush ===");
  auto sched = DefaultSchedule();
  sched.check_every_flushes = 1;
  auto in = DefaultInput(sched);
  in.flush_count = 7;  // any non-zero flush is a multiple of 1
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "cadence=1 allows every flush");
}

// ============================================================
// 9. Edge: zero total_payload_bytes (avoid div-by-zero)
// ============================================================

void TestZeroTotalPayload() {
  Log("=== TestZeroTotalPayload ===");
  auto sched = DefaultSchedule();
  sched.min_dead_payload_bytes = 0;     // allow zero
  sched.min_dead_payload_fraction = 0.0; // allow zero fraction
  auto in = DefaultInput(sched);
  in.dead_payload_bytes = 0;
  in.total_payload_bytes = 0;
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  // fraction is 0.0, meets_fraction is 0.0 >= 0.0 = true
  // meets_bytes is 0 >= 0 = true
  Check(ok, "zero totals with zero thresholds → eligible");
  Check(report.dead_payload_fraction == 0.0, "fraction is 0 with zero totals");
}

// ============================================================
// 10. Edge: last_completed_ms == 0 (never run before)
// ============================================================

void TestNeverRunBefore() {
  Log("=== TestNeverRunBefore ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);
  in.last_completed_ms = 0;  // never completed
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "never run before → cooldown is skipped → eligible");
}

// ============================================================
// 11. Edge: last_write_activity_ms == 0 (never written)
// ============================================================

void TestNeverWritten() {
  Log("=== TestNeverWritten ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);
  in.last_write_activity_ms = 0;  // never written
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "never written → idle gate skipped → eligible");
}

// ============================================================
// 12. Report notes contain useful info
// ============================================================

void TestReportNotes() {
  Log("=== TestReportNotes ===");

  // Cadence skip: notes should mention flush count and cadence
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);
  in.flush_count = 5;
  ScheduledLiveSetMaintenanceReport report{};
  EvaluateMaintenanceGate(in, report);
  Check(!report.notes.empty(), "cadence skip notes non-empty");
  Check(report.notes.find("cadence") != std::string::npos, "cadence note mentions cadence");

  // Below threshold: notes should mention bytes/fraction
  in.flush_count = 4;
  in.dead_payload_bytes = 100;
  in.total_payload_bytes = 100'000;
  report = {};
  EvaluateMaintenanceGate(in, report);
  Check(report.notes.find("threshold") != std::string::npos ||
        report.notes.find("bytes") != std::string::npos,
        "threshold note mentions bytes or threshold");
}

// ============================================================
// 13. Fraction threshold clamped to [0, 1]
// ============================================================

void TestFractionClamping() {
  Log("=== TestFractionClamping ===");
  auto sched = DefaultSchedule();
  sched.min_dead_payload_fraction = 2.0;  // > 1.0, should be clamped to 1.0
  auto in = DefaultInput(sched);
  in.dead_payload_bytes = 8192;
  in.total_payload_bytes = 8192;  // fraction = 1.0
  // After clamping, meets_fraction = 1.0 >= 1.0 = true
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "fraction > 1.0 clamped: 100% dead still passes");

  // Negative fraction clamped to 0.0
  sched.min_dead_payload_fraction = -1.0;
  in.dead_payload_bytes = 0;
  in.total_payload_bytes = 1000;  // fraction = 0.0
  // After clamping, meets_fraction = 0.0 >= 0.0 = true
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "negative fraction clamped to 0 → zero dead is still ok");
}

}  // namespace

int main() {
  Log("== live_set_rewrite_test ==");

  TestDisabled();
  TestAllGatesPass();
  TestCadenceGate();
  TestCooldownGate();
  TestIdleGate();
  TestThresholdBytesGate();
  TestThresholdFractionGate();
  TestCadenceEveryFlush();
  TestZeroTotalPayload();
  TestNeverRunBefore();
  TestNeverWritten();
  TestReportNotes();
  TestFractionClamping();

  std::cout << "\n[live_set_rewrite_test] " << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
