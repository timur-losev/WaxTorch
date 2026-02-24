#include "waxcpp/maintenance.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace waxcpp {

namespace {

/// Metadata key constants for surrogate frames (forward-compatible with
/// Swift metadata keys even though the C++ binary format does not yet persist
/// per-frame metadata).
constexpr const char* kMetaSourceFrameId = "source_frame_id";
constexpr const char* kMetaAlgorithm = "surrogate_algo";
constexpr const char* kMetaVersion = "surrogate_version";
constexpr const char* kMetaMaxTokens = "surrogate_max_tokens";
constexpr const char* kMetaFormat = "surrogate_format";
constexpr const char* kHierarchicalFormatV1 = "hierarchical_v1";

constexpr int kBatchSize = 64;
constexpr std::uint8_t kFrameStatusActive = 0;

std::string BytesToString(const std::vector<std::byte>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const auto b : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  return text;
}

std::vector<std::byte> StringToBytes(const std::string& text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

/// Trim leading and trailing whitespace from a string_view.
std::string_view TrimWhitespace(std::string_view sv) {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\n' ||
                         sv.front() == '\r' || sv.front() == '\t')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\n' ||
                         sv.back() == '\r' || sv.back() == '\t')) {
    sv.remove_suffix(1);
  }
  return sv;
}

/// Build a set of frame IDs that are superseded by another frame.
/// These frames have existing surrogates (the superseding frame is the surrogate).
std::unordered_set<std::uint64_t> BuildSupersededSourceSet(
    const std::vector<WaxFrameMeta>& frames) {
  std::unordered_set<std::uint64_t> result;
  for (const auto& f : frames) {
    if (f.supersedes.has_value()) {
      result.insert(*f.supersedes);
    }
  }
  return result;
}

/// Encode hierarchical tiers as a simple delimited payload.
/// Format: "WAXSURR1\nFULL:<full>\nGIST:<gist>\nMICRO:<micro>"
/// This is a lightweight encoding that avoids JSON dependency.
std::string EncodeTiersPayload(const SurrogateTiers& tiers) {
  std::string payload;
  payload += "WAXSURR1\n";
  payload += "FULL:";
  payload += tiers.full;
  payload += "\nGIST:";
  payload += tiers.gist;
  payload += "\nMICRO:";
  payload += tiers.micro;
  return payload;
}

}  // namespace

MaintenanceReport OptimizeSurrogates(
    WaxStore& store,
    const ExtractiveSurrogateGenerator& generator,
    const MaintenanceOptions& options) {

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();

  const int clamped_max_tokens = std::max(0, options.surrogate_max_tokens);

  // Resolve optional deadline.
  std::optional<Clock::time_point> deadline;
  if (options.max_wall_time_ms.has_value()) {
    deadline = start + std::chrono::milliseconds(
                           std::max(0, *options.max_wall_time_ms));
  }

  // Resolve optional frame limit.
  std::optional<int> max_frames;
  if (options.max_frames.has_value()) {
    max_frames = std::max(0, *options.max_frames);
  }

  // Snapshot committed frames.
  const auto frames = store.FrameMetas();

  MaintenanceReport report;
  report.scanned_frames = static_cast<int>(frames.size());

  // Build a set of frame IDs that already have surrogates linked via
  // the supersedes chain (used for skip-existing detection).
  const auto superseded_sources = BuildSupersededSourceSet(frames);

  int pending_batch = 0;

  for (const auto& frame : frames) {
    // Check wall-time deadline.
    if (deadline.has_value() && Clock::now() >= *deadline) {
      report.did_timeout = true;
      break;
    }

    // Check frame limit (against eligible, not scanned).
    if (max_frames.has_value() && report.eligible_frames >= *max_frames) {
      break;
    }

    // Skip non-active frames.
    if (frame.status != kFrameStatusActive) continue;

    // Skip frames that have already been superseded.
    if (frame.superseded_by.has_value()) continue;

    // Skip zero-length payloads.
    if (frame.payload_length == 0) continue;

    // Read source content.
    const auto content_bytes = store.FrameContent(frame.id);
    const auto content_str = BytesToString(content_bytes);
    const auto trimmed = TrimWhitespace(content_str);
    if (trimmed.empty()) continue;

    // Skip content that looks like it is already a surrogate (starts with our
    // magic header).
    if (trimmed.size() >= 8 && trimmed.substr(0, 8) == "WAXSURR1") continue;

    report.eligible_frames += 1;

    // Check if this source already has a surrogate (via supersedes chain).
    const bool has_existing = superseded_sources.count(frame.id) > 0;
    if (has_existing && !options.overwrite_existing) {
      report.skipped_up_to_date += 1;
      continue;
    }

    // Generate surrogate.
    std::string surrogate_payload;
    bool is_hierarchical = false;

    if (options.enable_hierarchical) {
      const auto tiers =
          generator.GenerateTiers(trimmed, options.tier_config);
      if (tiers.full.empty()) continue;
      surrogate_payload = EncodeTiersPayload(tiers);
      is_hierarchical = true;
    } else {
      surrogate_payload = generator.Generate(trimmed, clamped_max_tokens);
      // Trim the result.
      const auto sv = TrimWhitespace(surrogate_payload);
      surrogate_payload = std::string(sv);
      if (surrogate_payload.empty()) continue;
    }

    // Build metadata (forward-compatible even if not persisted yet).
    Metadata meta;
    meta[kMetaSourceFrameId] = std::to_string(frame.id);
    meta[kMetaAlgorithm] = ExtractiveSurrogateGenerator::kAlgorithmID;
    meta[kMetaVersion] = "1";
    meta[kMetaMaxTokens] = std::to_string(clamped_max_tokens);
    if (is_hierarchical) {
      meta[kMetaFormat] = kHierarchicalFormatV1;
    }

    // Store surrogate frame.
    const auto surrogate_id = store.Put(StringToBytes(surrogate_payload), meta);
    report.generated_surrogates += 1;

    // If overwriting existing, supersede the old surrogate frame.
    // The source frame itself stays active; only old surrogates get superseded.
    if (has_existing) {
      // Find the existing surrogate frame that supersedes this source.
      for (const auto& candidate : frames) {
        if (candidate.supersedes.has_value() &&
            *candidate.supersedes == frame.id &&
            candidate.status == kFrameStatusActive &&
            !candidate.superseded_by.has_value()) {
          store.Supersede(candidate.id, surrogate_id);
          report.superseded_surrogates += 1;
          break;
        }
      }
    }

    ++pending_batch;
    if (pending_batch >= kBatchSize) {
      store.Commit();
      pending_batch = 0;
    }
  }

  // Final commit for any remaining uncommitted surrogates.
  if (pending_batch > 0) {
    store.Commit();
  }

  return report;
}

}  // namespace waxcpp
