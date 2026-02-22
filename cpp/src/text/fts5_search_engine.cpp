#include "waxcpp/fts5_search_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef WAXCPP_HAS_SQLITE
#include "sqlite3.h"
#endif

namespace waxcpp {
namespace {

std::atomic<std::uint32_t> g_test_commit_fail_countdown{0};

std::vector<std::string> Tokenize(std::string_view text) {
  std::vector<std::string> tokens{};
  std::string current{};
  current.reserve(32);

  for (const unsigned char ch : text) {
    if (std::isalnum(ch) != 0) {
      current.push_back(static_cast<char>(std::tolower(ch)));
      continue;
    }
    if (!current.empty()) {
      tokens.push_back(std::move(current));
      current.clear();
      current.reserve(32);
    }
  }
  if (!current.empty()) {
    tokens.push_back(std::move(current));
  }
  return tokens;
}

std::unordered_map<std::string, std::uint32_t> TokenFreq(std::string_view text) {
  std::unordered_map<std::string, std::uint32_t> freq{};
  for (auto token : Tokenize(text)) {
    auto it = freq.find(token);
    if (it == freq.end()) {
      freq.emplace(std::move(token), 1U);
    } else {
      it->second += 1U;
    }
  }
  return freq;
}

std::vector<SearchResult> ScoreResults(const std::unordered_map<std::uint64_t, std::string>& docs,
                                       const std::vector<std::string>& query_tokens_raw,
                                       int top_k,
                                       const std::unordered_set<std::uint64_t>* candidate_filter) {
  if (top_k <= 0 || docs.empty() || query_tokens_raw.empty()) {
    return {};
  }

  std::unordered_set<std::string> unique_query_tokens{};
  unique_query_tokens.reserve(query_tokens_raw.size());
  for (const auto& token : query_tokens_raw) {
    unique_query_tokens.insert(token);
  }

  std::unordered_map<std::string, std::uint32_t> doc_freq{};
  doc_freq.reserve(unique_query_tokens.size());
  std::vector<std::pair<std::uint64_t, std::unordered_map<std::string, std::uint32_t>>> doc_freq_maps{};
  doc_freq_maps.reserve(docs.size());

  for (const auto& [frame_id, text] : docs) {
    if (candidate_filter != nullptr && candidate_filter->find(frame_id) == candidate_filter->end()) {
      continue;
    }

    auto freq = TokenFreq(text);
    for (const auto& token : unique_query_tokens) {
      if (freq.find(token) != freq.end()) {
        doc_freq[token] += 1U;
      }
    }
    doc_freq_maps.emplace_back(frame_id, std::move(freq));
  }

  const double doc_count = static_cast<double>(doc_freq_maps.size());
  if (doc_count <= 0.0) {
    return {};
  }

  std::vector<SearchResult> results{};
  results.reserve(doc_freq_maps.size());
  for (const auto& [frame_id, freq] : doc_freq_maps) {
    double score = 0.0;
    for (const auto& token : query_tokens_raw) {
      const auto tf_it = freq.find(token);
      if (tf_it == freq.end()) {
        continue;
      }
      const auto df_it = doc_freq.find(token);
      const double df = (df_it == doc_freq.end()) ? 0.0 : static_cast<double>(df_it->second);
      const double idf = std::log((doc_count + 1.0) / (df + 1.0)) + 1.0;
      score += static_cast<double>(tf_it->second) * idf;
    }
    if (score <= 0.0) {
      continue;
    }

    SearchResult result{};
    result.frame_id = frame_id;
    result.score = static_cast<float>(score);
    const auto doc_it = docs.find(frame_id);
    if (doc_it == docs.end()) {
      continue;
    }
    result.preview_text = doc_it->second;
    result.sources = {SearchSource::kText};
    results.push_back(std::move(result));
  }

  std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score > rhs.score;
    }
    return lhs.frame_id < rhs.frame_id;
  });
  if (results.size() > static_cast<std::size_t>(top_k)) {
    results.resize(static_cast<std::size_t>(top_k));
  }
  return results;
}

#ifdef WAXCPP_HAS_SQLITE

class Statement final {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db_));
    }
  }

  ~Statement() {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  sqlite3_stmt* get() const { return stmt_; }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

bool Exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc == SQLITE_OK) {
    return true;
  }
  std::string message = err != nullptr ? err : "sqlite exec failed";
  if (err != nullptr) {
    sqlite3_free(err);
  }
  throw std::runtime_error(message);
}

std::string BuildFtsMatchQuery(const std::vector<std::string>& query_tokens_raw) {
  std::unordered_set<std::string> seen{};
  std::vector<std::string> unique_tokens{};
  unique_tokens.reserve(query_tokens_raw.size());
  for (const auto& token : query_tokens_raw) {
    if (seen.insert(token).second) {
      unique_tokens.push_back(token);
    }
  }
  std::string query{};
  for (std::size_t i = 0; i < unique_tokens.size(); ++i) {
    if (i > 0) {
      query.append(" OR ");
    }
    query.push_back('"');
    query.append(unique_tokens[i]);
    query.push_back('"');
  }
  return query;
}

std::unordered_set<std::uint64_t> QueryCandidateFrameIds(sqlite3* db, const std::vector<std::string>& query_tokens_raw) {
  const auto fts_query = BuildFtsMatchQuery(query_tokens_raw);
  if (fts_query.empty()) {
    return {};
  }

  Statement select_stmt(db, "SELECT rowid FROM frame_docs_fts WHERE frame_docs_fts MATCH ?1;");
  if (sqlite3_bind_text(select_stmt.get(), 1, fts_query.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite bind failed: ") + sqlite3_errmsg(db));
  }

  std::unordered_set<std::uint64_t> ids{};
  while (true) {
    const int rc = sqlite3_step(select_stmt.get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      throw std::runtime_error(std::string("sqlite step failed: ") + sqlite3_errmsg(db));
    }
    ids.insert(static_cast<std::uint64_t>(sqlite3_column_int64(select_stmt.get(), 0)));
  }
  return ids;
}

void InsertDoc(sqlite3* db, sqlite3_stmt* stmt, std::uint64_t frame_id, const std::string& body) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  if (sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(frame_id)) != SQLITE_OK ||
      sqlite3_bind_text(stmt, 2, body.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite bind failed: ") + sqlite3_errmsg(db));
  }
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("sqlite insert failed: ") + sqlite3_errmsg(db));
  }
}

void DeleteDoc(sqlite3* db, sqlite3_stmt* stmt, std::uint64_t frame_id) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  if (sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(frame_id)) != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite bind failed: ") + sqlite3_errmsg(db));
  }
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("sqlite delete failed: ") + sqlite3_errmsg(db));
  }
}

void RebuildSqliteSnapshot(sqlite3* db, const std::unordered_map<std::uint64_t, std::string>& docs) {
  Exec(db, "BEGIN IMMEDIATE TRANSACTION;");
  try {
    Exec(db, "DELETE FROM frame_docs;");
    Statement insert_stmt(
        db,
        "INSERT INTO frame_docs(frame_id, body) VALUES(?1, ?2) "
        "ON CONFLICT(frame_id) DO UPDATE SET body=excluded.body;");
    for (const auto& [frame_id, text] : docs) {
      InsertDoc(db, insert_stmt.get(), frame_id, text);
    }
    Exec(db, "COMMIT;");
  } catch (...) {
    try {
      Exec(db, "ROLLBACK;");
    } catch (...) {
    }
    throw;
  }
}

#endif  // WAXCPP_HAS_SQLITE

void MaybeInjectCommitFailure() {
  auto remaining = g_test_commit_fail_countdown.load(std::memory_order_relaxed);
  while (remaining > 0) {
    if (g_test_commit_fail_countdown.compare_exchange_weak(remaining,
                                                           remaining - 1,
                                                           std::memory_order_relaxed,
                                                           std::memory_order_relaxed)) {
      throw std::runtime_error("FTS5SearchEngine::CommitStaged injected failure");
    }
  }
}

}  // namespace

struct FTS5SearchEngine::SQLiteState {
#ifdef WAXCPP_HAS_SQLITE
  sqlite3* db = nullptr;
#endif

  ~SQLiteState() {
#ifdef WAXCPP_HAS_SQLITE
    if (db != nullptr) {
      sqlite3_close(db);
      db = nullptr;
    }
#endif
  }
};

FTS5SearchEngine::FTS5SearchEngine() {
#ifdef WAXCPP_HAS_SQLITE
  auto sqlite_state = std::make_unique<SQLiteState>();
  if (sqlite3_open_v2(":memory:",
                      &sqlite_state->db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                      nullptr) != SQLITE_OK) {
    sqlite_state.reset();
  } else {
    try {
      (void)Exec(sqlite_state->db, "PRAGMA journal_mode=OFF;");
      (void)Exec(sqlite_state->db, "PRAGMA synchronous=OFF;");
      (void)Exec(sqlite_state->db, "CREATE TABLE IF NOT EXISTS frame_docs("
                                   "frame_id INTEGER PRIMARY KEY,"
                                   "body TEXT NOT NULL"
                                   ");");
      (void)Exec(sqlite_state->db,
                 "CREATE VIRTUAL TABLE IF NOT EXISTS frame_docs_fts USING fts5("
                 "body,"
                 "content='frame_docs',"
                 "content_rowid='frame_id',"
                 "tokenize='unicode61 remove_diacritics 0'"
                 ");");
      (void)Exec(sqlite_state->db,
                 "CREATE TRIGGER IF NOT EXISTS frame_docs_ai AFTER INSERT ON frame_docs BEGIN "
                 "INSERT INTO frame_docs_fts(rowid, body) VALUES(new.frame_id, new.body); "
                 "END;");
      (void)Exec(sqlite_state->db,
                 "CREATE TRIGGER IF NOT EXISTS frame_docs_ad AFTER DELETE ON frame_docs BEGIN "
                 "INSERT INTO frame_docs_fts(frame_docs_fts, rowid, body) VALUES('delete', old.frame_id, old.body); "
                 "END;");
      (void)Exec(sqlite_state->db,
                 "CREATE TRIGGER IF NOT EXISTS frame_docs_au AFTER UPDATE ON frame_docs BEGIN "
                 "INSERT INTO frame_docs_fts(frame_docs_fts, rowid, body) VALUES('delete', old.frame_id, old.body); "
                 "INSERT INTO frame_docs_fts(rowid, body) VALUES(new.frame_id, new.body); "
                 "END;");
      sqlite_ = std::move(sqlite_state);
    } catch (...) {
      sqlite_state.reset();
    }
  }
#endif
}

FTS5SearchEngine::~FTS5SearchEngine() = default;

FTS5SearchEngine::FTS5SearchEngine(FTS5SearchEngine&&) noexcept = default;

FTS5SearchEngine& FTS5SearchEngine::operator=(FTS5SearchEngine&&) noexcept = default;

void FTS5SearchEngine::StageIndex(std::uint64_t frame_id, const std::string& text) {
  pending_mutations_.push_back(PendingMutation{PendingMutationType::kIndex, frame_id, text});
}

void FTS5SearchEngine::StageIndexBatch(const std::vector<std::uint64_t>& frame_ids,
                                       const std::vector<std::string>& texts) {
  if (frame_ids.size() != texts.size()) {
    throw std::runtime_error("FTS5SearchEngine::StageIndexBatch size mismatch");
  }
  pending_mutations_.reserve(pending_mutations_.size() + frame_ids.size());
  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    StageIndex(frame_ids[i], texts[i]);
  }
}

void FTS5SearchEngine::StageRemove(std::uint64_t frame_id) {
  pending_mutations_.push_back(PendingMutation{PendingMutationType::kRemove, frame_id, {}});
}

void FTS5SearchEngine::CommitStaged() {
  MaybeInjectCommitFailure();
  for (const auto& mutation : pending_mutations_) {
    if (mutation.type == PendingMutationType::kIndex) {
      docs_[mutation.frame_id] = mutation.text;
      continue;
    }
    docs_.erase(mutation.frame_id);
  }

#ifdef WAXCPP_HAS_SQLITE
  if (sqlite_ != nullptr && sqlite_->db != nullptr) {
    try {
      Exec(sqlite_->db, "BEGIN IMMEDIATE TRANSACTION;");
      Statement insert_stmt(
          sqlite_->db,
          "INSERT INTO frame_docs(frame_id, body) VALUES(?1, ?2) "
          "ON CONFLICT(frame_id) DO UPDATE SET body=excluded.body;");
      Statement delete_stmt(sqlite_->db, "DELETE FROM frame_docs WHERE frame_id = ?1;");

      for (const auto& mutation : pending_mutations_) {
        if (mutation.type == PendingMutationType::kIndex) {
          InsertDoc(sqlite_->db, insert_stmt.get(), mutation.frame_id, mutation.text);
          continue;
        }
        DeleteDoc(sqlite_->db, delete_stmt.get(), mutation.frame_id);
      }
      Exec(sqlite_->db, "COMMIT;");
    } catch (...) {
      try {
        Exec(sqlite_->db, "ROLLBACK;");
      } catch (...) {
      }
      try {
        RebuildSqliteSnapshot(sqlite_->db, docs_);
      } catch (...) {
        sqlite_.reset();
      }
    }
  }
#endif

  pending_mutations_.clear();
}

void FTS5SearchEngine::RollbackStaged() {
  pending_mutations_.clear();
}

std::size_t FTS5SearchEngine::PendingMutationCount() const {
  return pending_mutations_.size();
}

void FTS5SearchEngine::Index(std::uint64_t frame_id, const std::string& text) {
  StageIndex(frame_id, text);
  CommitStaged();
}

void FTS5SearchEngine::IndexBatch(const std::vector<std::uint64_t>& frame_ids,
                                  const std::vector<std::string>& texts) {
  StageIndexBatch(frame_ids, texts);
  CommitStaged();
}

void FTS5SearchEngine::Remove(std::uint64_t frame_id) {
  StageRemove(frame_id);
  CommitStaged();
}

std::vector<SearchResult> FTS5SearchEngine::Search(const std::string& query, int top_k) const {
  if (top_k <= 0 || docs_.empty()) {
    return {};
  }
  const auto query_tokens_raw = Tokenize(query);
  if (query_tokens_raw.empty()) {
    return {};
  }

#ifdef WAXCPP_HAS_SQLITE
  if (sqlite_ != nullptr && sqlite_->db != nullptr) {
    try {
      const auto candidate_ids = QueryCandidateFrameIds(sqlite_->db, query_tokens_raw);
      return ScoreResults(docs_, query_tokens_raw, top_k, &candidate_ids);
    } catch (...) {
      // Keep deterministic fallback behavior even when optional SQLite backend fails.
    }
  }
#endif

  return ScoreResults(docs_, query_tokens_raw, top_k, nullptr);
}

namespace text::testing {

void SetCommitFailCountdown(std::uint32_t countdown) {
  g_test_commit_fail_countdown.store(countdown, std::memory_order_relaxed);
}

void ClearCommitFailCountdown() {
  g_test_commit_fail_countdown.store(0, std::memory_order_relaxed);
}

}  // namespace text::testing

}  // namespace waxcpp
