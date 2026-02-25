#include "ue5_filesystem_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace waxcpp::server {

namespace {

std::string ToAsciiLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

}  // namespace

Ue5FilesystemScanner::Ue5FilesystemScanner(Ue5ScannerConfig config) : config_(std::move(config)) {
  for (auto& extension : config_.include_extensions) {
    extension = ToAsciiLower(extension);
  }
  for (auto& dir_name : config_.exclude_directory_names) {
    dir_name = ToAsciiLower(dir_name);
  }
}

std::vector<Ue5ScanEntry> Ue5FilesystemScanner::Scan(
    const std::filesystem::path& repo_root,
    const CancelRequestedFn& cancel_requested) const {
  std::error_code ec;
  if (!std::filesystem::exists(repo_root, ec) || ec) {
    throw std::runtime_error("scan repo_root does not exist: " + repo_root.string());
  }
  if (!std::filesystem::is_directory(repo_root, ec) || ec) {
    throw std::runtime_error("scan repo_root must be a directory: " + repo_root.string());
  }

  std::vector<Ue5ScanEntry> entries{};
  std::filesystem::recursive_directory_iterator it(
      repo_root, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end{};
  for (; !ec && it != end; it.increment(ec)) {
    if (cancel_requested && cancel_requested()) {
      break;
    }
    const auto& path = it->path();
    const bool is_directory = it->is_directory(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (is_directory) {
      if (ShouldExcludeDirectory(path)) {
        it.disable_recursion_pending();
      }
      continue;
    }
    const bool is_file = it->is_regular_file(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (!is_file) {
      continue;
    }

    if (!ShouldIncludeExtension(path.extension().string())) {
      continue;
    }

    const auto relative = std::filesystem::relative(path, repo_root, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const auto relative_generic = relative.generic_string();
    if (relative_generic.empty() || relative_generic.starts_with("../")) {
      continue;
    }

    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
      ec.clear();
      continue;
    }

    entries.push_back(Ue5ScanEntry{
        .relative_path = relative_generic,
        .size_bytes = static_cast<std::uint64_t>(file_size),
    });
  }

  std::sort(entries.begin(), entries.end(), [](const Ue5ScanEntry& lhs, const Ue5ScanEntry& rhs) {
    const auto lhs_key = ToAsciiLower(lhs.relative_path);
    const auto rhs_key = ToAsciiLower(rhs.relative_path);
    if (lhs_key == rhs_key) {
      return lhs.relative_path < rhs.relative_path;
    }
    return lhs_key < rhs_key;
  });
  return entries;
}

std::string Ue5FilesystemScanner::SerializeManifest(const std::vector<Ue5ScanEntry>& entries) {
  std::ostringstream out;
  for (const auto& entry : entries) {
    out << entry.relative_path << "\t" << entry.size_bytes << "\n";
  }
  return out.str();
}

bool Ue5FilesystemScanner::ShouldIncludeExtension(const std::string& extension) const {
  if (extension.empty()) {
    return false;
  }
  const auto normalized = ToAsciiLower(extension);
  return std::find(config_.include_extensions.begin(), config_.include_extensions.end(), normalized) !=
         config_.include_extensions.end();
}

bool Ue5FilesystemScanner::ShouldExcludeDirectory(const std::filesystem::path& dir_path) const {
  const auto dir_name = ToAsciiLower(dir_path.filename().string());
  return std::find(config_.exclude_directory_names.begin(),
                   config_.exclude_directory_names.end(),
                   dir_name) != config_.exclude_directory_names.end();
}

}  // namespace waxcpp::server
