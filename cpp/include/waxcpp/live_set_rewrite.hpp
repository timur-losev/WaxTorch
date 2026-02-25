#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace waxcpp {

/// Configuration for scheduled live-set file rewriting/compaction.
/// Controls when and how the store file is rewritten to reclaim dead space.
struct LiveSetRewriteSchedule {
  /// Whether scheduled rewriting is enabled.
  bool enabled = false;

  /// Check rewrite eligibility every N flushes.
  int check_every_flushes = 32;

  /// Minimum dead payload bytes before considering a rewrite.
  std::uint64_t min_dead_payload_bytes = 64 * 1024 * 1024;  // 64 MiB

  /// Minimum fraction of dead payload (0.0–1.0) before considering a rewrite.
  double min_dead_payload_fraction = 0.25;

  /// Minimum compaction gain bytes to justify the rewrite cost.
  std::uint64_t minimum_compaction_gain_bytes = 0;

  /// Minimum idle time (ms) since last user activity before rewriting.
  int minimum_idle_ms = 15000;

  /// Minimum interval (ms) between rewrite attempts.
  int min_interval_ms = 5 * 60000;  // 5 minutes

  /// Run deep verification on the rewritten file before swapping.
  bool verify_deep = false;

  /// Optional destination directory for the rewritten file.
  /// If empty, rewrites in-place (same directory as source).
  std::string destination_directory;

  /// Number of recent rewrite candidates to keep (for rollback safety).
  int keep_latest_candidates = 2;

  /// Pre-built disabled schedule.
  static LiveSetRewriteSchedule Disabled() { return {}; }
};

/// Options for a single live-set rewrite operation.
struct LiveSetRewriteOptions {
  /// Allow replacing an existing destination file.
  bool overwrite_destination = false;

  /// Replace payload bytes for non-live frames (deleted/superseded) with empty.
  bool drop_non_live_payloads = true;

  /// Run deep verification on the rewritten file before returning.
  bool verify_deep = false;
};

/// Report from a live-set rewrite operation.
struct LiveSetRewriteReport {
  std::string source_path;
  std::string destination_path;

  int frame_count = 0;
  int active_frame_count = 0;
  int dropped_payload_frames = 0;
  int deleted_frame_count = 0;
  int superseded_frame_count = 0;

  bool copied_lex_index = false;
  bool copied_vec_index = false;

  std::uint64_t logical_bytes_before = 0;
  std::uint64_t logical_bytes_after = 0;
  std::uint64_t allocated_bytes_before = 0;
  std::uint64_t allocated_bytes_after = 0;

  double duration_ms = 0.0;
};

/// Outcome of a scheduled live-set maintenance check.
enum class MaintenanceOutcome {
  kDisabled,                   // Schedule is disabled.
  kCadenceSkipped,             // Not yet time (cadence gate).
  kCooldownSkipped,            // Too soon since last run.
  kIdleSkipped,                // Recent write activity.
  kBelowThreshold,             // Dead payload below thresholds.
  kRewriteSucceeded,           // Rewrite completed successfully.
  kRewriteFailed,              // Rewrite attempted but failed.
  kValidationFailedRolledBack, // Rewrite succeeded but validation failed; rolled back.
};

/// Report from a scheduled live-set maintenance check.
struct ScheduledLiveSetMaintenanceReport {
  MaintenanceOutcome outcome = MaintenanceOutcome::kDisabled;
  bool triggered_by_flush = false;
  std::uint64_t flush_count = 0;
  std::uint64_t dead_payload_bytes = 0;
  std::uint64_t total_payload_bytes = 0;
  double dead_payload_fraction = 0.0;
  std::string candidate_path;
  std::optional<LiveSetRewriteReport> rewrite_report;
  bool rollback_performed = false;
  std::string notes;
};

/// Input state for the scheduled maintenance eligibility check.
struct MaintenanceGateInput {
  const LiveSetRewriteSchedule* schedule = nullptr;
  std::uint64_t flush_count = 0;
  bool force = false;
  std::int64_t now_ms = 0;                // Current wall-clock time.
  std::int64_t last_completed_ms = 0;     // Last completed maintenance time (0 = never).
  std::int64_t last_write_activity_ms = 0; // Last user write timestamp.
  std::uint64_t dead_payload_bytes = 0;
  std::uint64_t total_payload_bytes = 0;
};

/// Evaluate the maintenance gate: returns the outcome and fills the report.
/// Does NOT perform the actual rewrite — only determines eligibility.
/// Returns true if the rewrite should proceed.
bool EvaluateMaintenanceGate(const MaintenanceGateInput& input,
                             ScheduledLiveSetMaintenanceReport& report);

}  // namespace waxcpp
