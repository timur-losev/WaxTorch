#include "waxcpp/live_set_rewrite.hpp"

#include <algorithm>
#include <cmath>

namespace waxcpp {

bool EvaluateMaintenanceGate(const MaintenanceGateInput& input,
                             ScheduledLiveSetMaintenanceReport& report) {
  report = {};
  report.flush_count = input.flush_count;
  report.dead_payload_bytes = input.dead_payload_bytes;
  report.total_payload_bytes = input.total_payload_bytes;
  report.dead_payload_fraction = input.total_payload_bytes > 0
      ? static_cast<double>(input.dead_payload_bytes) /
            static_cast<double>(input.total_payload_bytes)
      : 0.0;

  if (input.schedule == nullptr || !input.schedule->enabled) {
    report.outcome = MaintenanceOutcome::kDisabled;
    report.notes = "live-set rewrite schedule is disabled";
    if (input.force) {
      return false;
    }
    return false;
  }

  const auto& sched = *input.schedule;

  // Cadence gate: only check every N flushes (unless forced).
  if (!input.force) {
    const auto cadence = static_cast<std::uint64_t>(std::max(1, sched.check_every_flushes));
    if (input.flush_count % cadence != 0) {
      report.outcome = MaintenanceOutcome::kCadenceSkipped;
      report.notes = "cadence gate: flush " + std::to_string(input.flush_count) +
                     "; every " + std::to_string(cadence) + " flushes";
      return false;
    }
  }

  // Cooldown gate: minimum interval between runs.
  if (!input.force && sched.min_interval_ms > 0 && input.last_completed_ms > 0) {
    const auto next_allowed_ms =
        input.last_completed_ms + static_cast<std::int64_t>(std::max(0, sched.min_interval_ms));
    if (input.now_ms < next_allowed_ms) {
      report.outcome = MaintenanceOutcome::kCooldownSkipped;
      report.notes = "cooldown gate: waiting for minimum interval";
      return false;
    }
  }

  // Idle gate: recent write activity.
  if (!input.force && sched.minimum_idle_ms > 0 && input.last_write_activity_ms > 0) {
    const auto idle_eligible_ms =
        input.last_write_activity_ms + static_cast<std::int64_t>(std::max(0, sched.minimum_idle_ms));
    if (input.now_ms < idle_eligible_ms) {
      report.outcome = MaintenanceOutcome::kIdleSkipped;
      report.notes = "idle gate: recent writes detected";
      return false;
    }
  }

  // Threshold gate: check dead payload thresholds.
  const double clamped_fraction_threshold =
      std::min(1.0, std::max(0.0, sched.min_dead_payload_fraction));
  const bool meets_bytes = input.dead_payload_bytes >= sched.min_dead_payload_bytes;
  const bool meets_fraction = report.dead_payload_fraction >= clamped_fraction_threshold;

  if (!meets_bytes && !meets_fraction) {
    report.outcome = MaintenanceOutcome::kBelowThreshold;
    report.notes = "below thresholds: bytes=" +
                   std::to_string(input.dead_payload_bytes) + "/" +
                   std::to_string(sched.min_dead_payload_bytes) +
                   " fraction=" + std::to_string(report.dead_payload_fraction) +
                   "/" + std::to_string(clamped_fraction_threshold);
    return false;
  }

  // All gates passed — eligible for rewrite.
  return true;
}

}  // namespace waxcpp
