#include "waxcpp/wax_store.hpp"

#include "../test_logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class FixtureMode {
  kPass,
  kOpenFail,
  kVerifyFail,
};

std::string_view ModeToString(FixtureMode mode) {
  switch (mode) {
    case FixtureMode::kPass:
      return "pass";
    case FixtureMode::kOpenFail:
      return "open_fail";
    case FixtureMode::kVerifyFail:
      return "verify_fail";
  }
  return "unknown";
}

struct FixtureExpectation {
  FixtureMode mode = FixtureMode::kPass;
  bool verify_deep = true;
  std::optional<std::uint64_t> frame_count;
  std::optional<std::uint64_t> generation;
  std::optional<std::string> error_contains;
};

std::string Trim(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool ParseBool(std::string value, const std::string& key) {
  value = ToLower(Trim(std::move(value)));
  if (value == "true" || value == "1" || value == "yes") {
    return true;
  }
  if (value == "false" || value == "0" || value == "no") {
    return false;
  }
  throw std::runtime_error("invalid boolean value for key '" + key + "'");
}

FixtureMode ParseMode(std::string value) {
  value = ToLower(Trim(std::move(value)));
  if (value == "pass") {
    return FixtureMode::kPass;
  }
  if (value == "open_fail") {
    return FixtureMode::kOpenFail;
  }
  if (value == "verify_fail") {
    return FixtureMode::kVerifyFail;
  }
  throw std::runtime_error("invalid mode value: " + value);
}

FixtureExpectation LoadExpectation(const std::filesystem::path& mv2s_path) {
  FixtureExpectation expected{};
  const auto expected_path = std::filesystem::path(mv2s_path.string() + ".expected");
  if (!std::filesystem::exists(expected_path)) {
    return expected;
  }

  std::ifstream in(expected_path);
  if (!in) {
    throw std::runtime_error("failed to open expected sidecar: " + expected_path.string());
  }

  std::string line;
  std::size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    const auto pos = trimmed.find('=');
    if (pos == std::string::npos) {
      throw std::runtime_error("invalid expected sidecar line " + std::to_string(line_no));
    }
    const auto key = Trim(trimmed.substr(0, pos));
    const auto value = Trim(trimmed.substr(pos + 1));
    if (key == "mode") {
      expected.mode = ParseMode(value);
    } else if (key == "verify_deep") {
      expected.verify_deep = ParseBool(value, key);
    } else if (key == "frame_count") {
      expected.frame_count = static_cast<std::uint64_t>(std::stoull(value));
    } else if (key == "generation") {
      expected.generation = static_cast<std::uint64_t>(std::stoull(value));
    } else if (key == "error_contains") {
      expected.error_contains = value;
    } else {
      throw std::runtime_error("unknown key in expected sidecar: " + key);
    }
  }

  if (expected.mode != FixtureMode::kPass &&
      (expected.frame_count.has_value() || expected.generation.has_value())) {
    throw std::runtime_error("stats expectations are only valid for mode=pass");
  }
  return expected;
}

bool HasMv2sExtension(const std::filesystem::path& path) {
  auto ext = path.extension().string();
  if (ext.size() != 5) {
    return false;
  }
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return ext == ".mv2s";
}

bool IsSyntheticFixture(const std::filesystem::path& path) {
  const auto normalized = path.generic_string();
  return normalized.find("/synthetic/") != std::string::npos;
}

std::vector<std::filesystem::path> DiscoverFixtures(const std::filesystem::path& fixtures_root) {
  std::vector<std::filesystem::path> fixtures;
  if (!std::filesystem::exists(fixtures_root)) {
    return fixtures;
  }

  for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures_root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (!HasMv2sExtension(entry.path())) {
      continue;
    }
    fixtures.push_back(entry.path());
  }

  std::sort(fixtures.begin(), fixtures.end());
  return fixtures;
}

void AssertExpected(const std::filesystem::path& fixture_path,
                    const waxcpp::WaxStats& stats,
                    const FixtureExpectation& expected) {
  if (expected.frame_count.has_value() && stats.frame_count != *expected.frame_count) {
    throw std::runtime_error("frame_count mismatch for " + fixture_path.string());
  }
  if (expected.generation.has_value() && stats.generation != *expected.generation) {
    throw std::runtime_error("generation mismatch for " + fixture_path.string());
  }
}

void AssertErrorMatch(const std::filesystem::path& fixture_path,
                      const std::exception& ex,
                      const FixtureExpectation& expected) {
  if (!expected.error_contains.has_value()) {
    return;
  }
  const std::string message = ex.what();
  if (message.find(*expected.error_contains) == std::string::npos) {
    throw std::runtime_error("error mismatch for " + fixture_path.string());
  }
}

}  // namespace

int main() {
  const std::filesystem::path fixtures_root = WAXCPP_PARITY_FIXTURES_DIR;
  constexpr bool require_fixtures = WAXCPP_REQUIRE_PARITY_FIXTURES != 0;
  constexpr bool require_swift_fixtures = WAXCPP_REQUIRE_SWIFT_FIXTURES != 0;

  try {
    waxcpp::tests::Log("mv2s_fixture_parity_test: start");
    waxcpp::tests::LogKV("fixtures_root", fixtures_root.string());
    waxcpp::tests::LogKV("require_fixtures", require_fixtures);
    waxcpp::tests::LogKV("require_swift_fixtures", require_swift_fixtures);
    const auto fixtures = DiscoverFixtures(fixtures_root);
    waxcpp::tests::LogKV("discovered_fixtures", static_cast<std::uint64_t>(fixtures.size()));
    if (fixtures.empty()) {
#if WAXCPP_REQUIRE_PARITY_FIXTURES
      std::cerr << "mv2s_fixture_parity_test failed: no .mv2s fixtures in "
                << fixtures_root.string() << "\n";
      return EXIT_FAILURE;
#else
      std::cout << "mv2s_fixture_parity_test skipped: no .mv2s fixtures in "
                << fixtures_root.string() << "\n";
      return EXIT_SUCCESS;
#endif
    }

    std::size_t non_synthetic_count = 0;
    for (const auto& fixture : fixtures) {
      if (!IsSyntheticFixture(fixture)) {
        ++non_synthetic_count;
      }
    }
    waxcpp::tests::LogKV("non_synthetic_fixtures", static_cast<std::uint64_t>(non_synthetic_count));
#if WAXCPP_REQUIRE_SWIFT_FIXTURES
    if (non_synthetic_count == 0) {
      std::cerr << "mv2s_fixture_parity_test failed: no non-synthetic fixtures in "
                << fixtures_root.string() << "\n";
      std::cerr << "expected at least one Swift-generated fixture (e.g. fixtures/parity/swift/*.mv2s)\n";
      return EXIT_FAILURE;
    }
#endif

    for (const auto& fixture : fixtures) {
      waxcpp::tests::Log("fixture: begin");
      waxcpp::tests::LogKV("fixture_path", fixture.string());
      waxcpp::tests::LogKV("fixture_source", IsSyntheticFixture(fixture) ? std::string("synthetic")
                                                                         : std::string("non_synthetic"));
      const auto expected = LoadExpectation(fixture);
      waxcpp::tests::LogKV("fixture_mode", std::string(ModeToString(expected.mode)));
      waxcpp::tests::LogKV("fixture_verify_deep", expected.verify_deep);
      if (expected.error_contains.has_value()) {
        waxcpp::tests::LogKV("fixture_error_contains", *expected.error_contains);
      }
      if (expected.mode == FixtureMode::kOpenFail) {
        try {
          auto store = waxcpp::WaxStore::Open(fixture);
          store.Close();
        } catch (const std::exception& ex) {
          waxcpp::tests::LogKV("fixture_open_error", std::string(ex.what()));
          AssertErrorMatch(fixture, ex, expected);
          std::cout << "fixture OK (open_fail): " << fixture.string() << "\n";
          waxcpp::tests::Log("fixture: open_fail passed");
          continue;
        }
        throw std::runtime_error("expected open failure for " + fixture.string());
      }

      auto store = waxcpp::WaxStore::Open(fixture);
      if (expected.mode == FixtureMode::kVerifyFail) {
        try {
          store.Verify(expected.verify_deep);
        } catch (const std::exception& ex) {
          waxcpp::tests::LogKV("fixture_verify_error", std::string(ex.what()));
          AssertErrorMatch(fixture, ex, expected);
          store.Close();
          std::cout << "fixture OK (verify_fail): " << fixture.string() << "\n";
          waxcpp::tests::Log("fixture: verify_fail passed");
          continue;
        }
        throw std::runtime_error("expected verify failure for " + fixture.string());
      }

      store.Verify(expected.verify_deep);
      const auto stats = store.Stats();
      waxcpp::tests::LogKV("fixture_stats_frame_count", stats.frame_count);
      waxcpp::tests::LogKV("fixture_stats_generation", stats.generation);
      AssertExpected(fixture, stats, expected);
      store.Close();
      std::cout << "fixture OK: " << fixture.string() << "\n";
      waxcpp::tests::Log("fixture: pass mode passed");
    }

    std::cout << "mv2s_fixture_parity_test passed (" << fixtures.size() << " fixtures)\n";
    waxcpp::tests::Log("mv2s_fixture_parity_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::cerr << "mv2s_fixture_parity_test failed: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
