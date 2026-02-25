#include "../../server/wax_rag_handler.hpp"

#include "../temp_artifacts.hpp"
#include "../test_logger.hpp"

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Poco::JSON::Object::Ptr ParseObject(const std::string& json) {
  Poco::JSON::Parser parser;
  const Poco::Dynamic::Var parsed = parser.parse(json);
  auto object = parsed.extract<Poco::JSON::Object::Ptr>();
  if (object.isNull()) {
    throw std::runtime_error("expected JSON object");
  }
  return object;
}

std::string TempName(const std::string& prefix, const std::string& suffix) {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::ostringstream out;
  out << prefix << static_cast<long long>(now) << suffix;
  return out.str();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& body) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    throw std::runtime_error("failed to create parent path for test file: " + path.string());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open test file for write: " + path.string());
  }
  out << body;
  if (!out) {
    throw std::runtime_error("failed to write test file: " + path.string());
  }
}

std::string MakeLargeCppBody(int lines) {
  std::ostringstream out;
  out << "#include <cstdint>\n\n";
  for (int i = 0; i < lines; ++i) {
    out << "int f_" << i << "() { return " << i << "; }\n";
  }
  return out.str();
}

waxcpp::RuntimeModelsConfig MakeRuntimeConfigForTests(const std::filesystem::path& runtime_root) {
  waxcpp::RuntimeModelsConfig models{};
  models.generation_model.runtime = "llama_cpp";
  models.generation_model.model_path = "test-generation.gguf";
  models.embedding_model.runtime = "disabled";
  models.embedding_model.model_path.clear();
  models.llama_cpp_root = runtime_root.string();
  models.enable_vector_search = false;
  models.require_distinct_models = true;
  return models;
}

void SetEnvVar(const char* key, const std::string& value) {
#if defined(_MSC_VER)
  if (_putenv_s(key, value.c_str()) != 0) {
    throw std::runtime_error(std::string("failed to set env var: ") + key);
  }
#else
  if (setenv(key, value.c_str(), 1) != 0) {
    throw std::runtime_error(std::string("failed to set env var: ") + key);
  }
#endif
}

struct IndexStatusView {
  std::string state{};
  std::uint64_t indexed_chunks = 0;
  std::uint64_t committed_chunks = 0;
  std::uint64_t scanned_files = 0;
};

IndexStatusView WaitForTerminalState(waxcpp::server::WaxRAGHandler& handler, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  IndexStatusView view{};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status_raw = handler.handle_index_status(Poco::JSON::Object::Ptr{});
    Require(status_raw.rfind("Error:", 0) != 0, "index.status must not fail");
    const auto status_json = ParseObject(status_raw);
    view.state = status_json->optValue<std::string>("state", "");
    view.indexed_chunks = status_json->optValue<std::uint64_t>("indexed_chunks", 0);
    view.committed_chunks = status_json->optValue<std::uint64_t>("committed_chunks", 0);
    view.scanned_files = status_json->optValue<std::uint64_t>("scanned_files", 0);
    if (view.state != "running") {
      return view;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  throw std::runtime_error("index job did not reach terminal state before timeout");
}

void ScenarioIndexStartIsAsyncAndStopWorks() {
  waxcpp::tests::Log("scenario: index.start returns promptly and stop cancels running job");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  for (int i = 0; i < 40; ++i) {
    WriteTextFile(temp_root / ("File" + std::to_string(i) + ".cpp"), MakeLargeCppBody(1200));
  }

  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
  start_params->set("repo_root", temp_root.string());
  start_params->set("resume", false);

  const auto start_ts = std::chrono::steady_clock::now();
  const auto start_raw = handler.handle_index_start(start_params);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_ts);

  Require(start_raw.rfind("Error:", 0) != 0, "index.start must not fail");
  Require(elapsed_ms.count() < 1500, "index.start must be asynchronous and return quickly");

  const auto start_json = ParseObject(start_raw);
  Require(start_json->optValue<std::string>("state", "") == "running",
          "index.start must report running state");

  const auto stop_raw = handler.handle_index_stop(Poco::JSON::Object::Ptr{});
  Require(stop_raw.rfind("Error:", 0) != 0, "index.stop must not fail when worker is running");
  const auto stop_json = ParseObject(stop_raw);
  Require(stop_json->optValue<std::string>("state", "") == "stopped", "index.stop must report stopped state");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
}

void ScenarioIndexCompleteWritesManifests() {
  waxcpp::tests::Log("scenario: finished index job persists manifests");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "A.cpp", MakeLargeCppBody(60));
  WriteTextFile(temp_root / "B.h", "struct B { int v = 7; };");

  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
  start_params->set("repo_root", temp_root.string());
  start_params->set("resume", false);
  const auto start_raw = handler.handle_index_start(start_params);
  Require(start_raw.rfind("Error:", 0) != 0, "index.start must not fail");

  const auto view = WaitForTerminalState(handler, 20000);
  Require(view.state == "stopped", "index job must eventually complete");
  Require(view.indexed_chunks > 0, "index job must ingest at least one chunk");
  Require(std::filesystem::exists(scan_manifest), "scan manifest must exist");
  Require(std::filesystem::exists(chunk_manifest), "chunk manifest must exist");
  Require(std::filesystem::exists(file_manifest), "file manifest must exist");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

void ScenarioResumeSkipsUnchangedFilesThenIndexesChangedFile() {
  waxcpp::tests::Log("scenario: resume skips unchanged files and indexes changed file");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "One.cpp", MakeLargeCppBody(120));
  WriteTextFile(temp_root / "Two.cpp", MakeLargeCppBody(100));

  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  auto start_with_resume = [&](bool resume) {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("resume", resume);
    const auto raw = handler.handle_index_start(params);
    Require(raw.rfind("Error:", 0) != 0, "index.start must not fail");
  };

  start_with_resume(false);
  const auto first = WaitForTerminalState(handler, 20000);
  Require(first.state == "stopped", "initial index run must stop");
  Require(first.indexed_chunks > 0, "initial index run must ingest chunks");

  start_with_resume(true);
  const auto second = WaitForTerminalState(handler, 20000);
  Require(second.state == "stopped", "resume run must stop");
  Require(second.indexed_chunks == 0, "resume run over unchanged files must ingest zero chunks");

  WriteTextFile(temp_root / "Two.cpp", MakeLargeCppBody(140));
  start_with_resume(true);
  const auto third = WaitForTerminalState(handler, 20000);
  Require(third.state == "stopped", "resume run with changed file must stop");
  Require(third.indexed_chunks > 0, "resume run with changed file must ingest chunks");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("wax_rag_handler_index_test: start");
    ScenarioIndexStartIsAsyncAndStopWorks();
    ScenarioIndexCompleteWritesManifests();
    ScenarioResumeSkipsUnchangedFilesThenIndexesChangedFile();
    waxcpp::tests::Log("wax_rag_handler_index_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_handler_index_repo_");
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_handler_index_store_");
    return EXIT_FAILURE;
  }
}
