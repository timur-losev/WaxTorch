# Context: C++ Core RAG Port (LibTorch)

**Created**: 2026-02-18
**Last Updated**: 2026-02-21
**Current Phase**: M3 complete baseline + M5/M6 deterministic CPU baselines
**Next Agent**: wax-rag-specialist

## Task Summary
Initialize a side-by-side C++20 workspace for Wax Core RAG and start M2 with real MV2S binary format handling for read/verify.

## Decisions

| # | Decision | Rationale | Reversible? |
|---|----------|-----------|-------------|
| 1 | C++ code under `cpp/` | Isolates CMake stack from Swift SPM tree | No (should stay stable) |
| 2 | Scope v1 = Core RAG only | Matches agreed rollout and reduces risk | Yes |
| 3 | All external deps via submodules | Deterministic dependency control and auditability | No (policy) |
| 4 | LibTorch delivery = practical hybrid | Enables pinned artifact manifest + SHA verification | Yes |

## Progress

- [x] Create `cpp/` skeleton with module folders (`core/text/vector/rag/orchestrator`)
- [x] Add public interface headers in `cpp/include/waxcpp`
- [x] Add implementation stubs in `cpp/src/*`
- [x] Add smoke test and CMake baseline build
- [x] Add `.gitmodules` entries for required dependencies
- [x] Add dependency lock policy (`cpp/submodules.lock`)
- [x] Add dependency verification script (`cpp/scripts/verify_submodules.py`)
- [x] Add CI workflow (`.github/workflows/cpp-ci.yml`)
- [x] Save plan document (`Tasks/cpp-port-libtorch-plan.md`)
- [x] Implement SHA-256 utility for format checksums (`cpp/src/core/sha256.*`)
- [x] Implement MV2S header/footer codec + checksum validation (`cpp/src/core/mv2s_format.*`)
- [x] Implement canonical empty TOC v1 encoder (`EncodeEmptyTocV1`)
- [x] Implement generic TOC v1 encoder (`EncodeTocV1`) for deterministic fixture and test generation
- [x] Implement TOC decoder with structural validations (`DecodeToc`)
- [x] Implement `WaxStore::Create/Open/Verify` read-path with footer fallback scan + TOC/range validation
- [x] Implement deep verify mode (`Verify(true)`) with payload checksum validation against TOC
- [x] Fix optional dereference bug in header selection path (no deref of empty header page)
- [x] Improve footer selection to prefer latest valid footer (generation/offset) between header pointer and scan result
- [x] Add replay snapshot footer candidate support in open path (header fast footer + snapshot footer + scanned footer arbitration)
- [x] Add unit coverage for read/verify, TOC corruption detection, and dual-header fallback (`cpp/tests/unit/wax_store_verify_test.cpp`)
- [x] Add dedicated MV2S format invariant tests for TOC codec (`cpp/tests/unit/mv2s_format_test.cpp`)
- [x] Align deep verify with Swift frame checksum model: verify stored payload checksum for all frames, canonical checksum for plain frames
- [x] Add segment checksum deep verification and frame/segment overlap validation in read path
- [x] Extend TOC summary model with canonical/stored frame checksum metadata and segment metadata
- [x] Add parity test harness for fixture-driven `.mv2s` cross-language validation (`cpp/tests/parity/mv2s_fixture_parity_test.cpp`)
- [x] Add strict parity fixture gate (`WAXCPP_REQUIRE_PARITY_FIXTURES`) for CI enforcement once Swift fixtures are committed
- [x] Extend parity sidecar policy with fixture modes (`pass|open_fail|verify_fail`) and optional `error_contains` matching
- [x] Add deterministic synthetic fixture generator and baseline fixture pack under `fixtures/parity/synthetic`
- [x] Add test logging helper (`WAXCPP_TEST_LOG`) with detailed key-value scenario logs and expected-exception messages in unit/parity tests
- [x] Add detailed generation logs for synthetic fixture generator (fixture name/mode/path + wal/layout offsets/sizes + mutation offsets)
- [x] Add TOC parity validation: index manifests must match segment catalog entries (`lex/vec/time`)
- [x] Add unit coverage for manifest-to-segment linkage (`missing match` fail + `matching entry` pass)
- [x] Add synthetic parity fixture for `open_fail` when lex manifest is not backed by segment catalog entry
- [x] Add strict parity gate to require non-synthetic (Swift-generated) fixtures when enabled (`WAXCPP_REQUIRE_SWIFT_FIXTURES`)
- [x] Add dedicated `fixtures/parity/swift/` location and docs for external Swift fixture ingestion
- [x] Add Swift executable generator `WaxParityFixtureGenerator` to produce repository parity fixtures under `fixtures/parity/swift/`
- [x] Add GitHub Actions macOS workflow (`Swift Parity Fixtures`) to generate Swift fixtures when local macOS is unavailable
- [x] Complete M2 parity against external Swift-generated fixtures (cross-language artifacts)
- [x] Enable strict Swift fixture gate in default C++ CI configure step
- [x] Add C++ WAL ring read primitives (`terminal marker` + `scan state`) aligned with Swift WAL header semantics
- [x] Implement `scanPendingMutationsWithState` parity behavior in C++ WAL reader (state scan continues after decode errors, pending decode stops)
- [x] Wire WAL pending scan into `WaxStore::Open` with Swift-like `lastSequence = max(committedSeq, scanLastSeq)` handling
- [x] Add pending putFrame data-range guard (`requiredEnd`) against file size
- [x] Add open-time trailing-byte repair parity: truncate to `requiredEnd` while preserving pending `putFrame` referenced payload tail
- [x] Persist internal WAL open-state (`writePos/checkpointPos/pendingBytes/lastSequence/dirty`) for upcoming replay/apply path
- [x] Add unit scenarios for WAL open behavior:
  - undecodable pending payload does not block open
  - decodable pending putFrame is counted in `pending_frames`
  - pending putFrame payload beyond file size fails open
  - replay-snapshot fallback path truncates trailing bytes to committed footer end
  - pending putFrame tail reference keeps file truncated to referenced payload end (not footer end)
- [x] Add dedicated WAL ring unit test suite (`terminal marker`, `decode-stop with continued state scan`, `wrap+padding`, `ScanWalState parity`)
- [x] Add public `WaxStore::WalStats()` surface (parity-aligned WAL runtime introspection baseline)
- [x] Extend `wax_store_verify_test` with WAL state assertions (`write/checkpoint/pending/last_seq/replay_snapshot_hit_count`)
- [x] Add checkpoint-preservation regression test for pending WAL recovery path (`checkpoint` stays on committed cursor while pending exists)
- [x] Split open repair behavior from verify path: `Open` repairs trailing bytes, `Verify` remains non-mutating
- [x] Add regression test to enforce non-repair `Verify` behavior with trailing bytes
- [x] Add clean-WAL cursor normalization regression test (`scanState.lastSequence <= committed` path aligns write/checkpoint)
- [x] Add `WaxStore::Open(path, repair)` overload to mirror Swift open/repair control
- [x] Extend WAL pending mutation decode model with delete/supersede/putEmbedding payload fields
- [x] Add C++ WAL write-side primitive (`WalRingWriter`) with Swift-like append/capacity/padding-wrap/sentinel semantics
- [x] Add checkpoint primitive (`RecordCheckpoint`) and writer instrumentation counters for WAL state parity
- [x] Add dedicated writer-side WAL unit coverage (`inline sentinel`, `padding wrap`, `capacity guard`, `separate sentinel write`)
- [x] Wire basic `WaxStore` write-path to WAL writer (`Put`, `PutBatch`, `Delete`, `Supersede`)
- [x] Implement baseline `WaxStore::Commit` apply path for decoded pending WAL mutations (`putFrame/delete/supersede`)
- [x] Persist frame lifecycle fields in TOC codec (`status`, `supersedes`, `superseded_by`) for commit/reopen continuity
- [x] Add write-path integration unit coverage (`put/commit/reopen`, pending recovery commit, delete+supersede TOC persistence)
- [x] Add deterministic crash-window failpoints for C++ commit pipeline (after TOC/footer/headerA/headerB steps)
- [x] Add crash-window recovery tests (reopen outcomes for pre-footer, post-footer, and single-header-published failures)
- [x] Add supersede graph consistency guards in commit apply path (cycle detection + conflicting edge rejection)
- [x] Add negative integration coverage for supersede cycle/conflict rejection in C++ store write tests
- [x] Add `Close()` auto-commit for locally staged pending mutations while preserving recovery-only pending WAL semantics
- [x] Add frame read API surface in C++ store (`FrameMeta`, `FrameMetas`, `FrameContent`, `FrameContents`)
- [x] Add explicit regression test for `Close()` behavior on recovered pending WAL (must not silently commit)
- [x] Add mixed pending scenario coverage (recovered pending + new local mutations commit in one pass)
- [x] Replace `FTS5SearchEngine` stub with deterministic in-memory text ranking baseline (tokenization + TF-IDF scoring + frame_id tie-break)
- [x] Add dedicated `fts5_search_engine` unit test suite (ranking, tie-break, remove, batch mismatch, empty-input guards)
- [x] Add writer-lease baseline for `WaxStore::Open/Create` via lock-directory sentinel (`.writer.lock`) to block concurrent writers on same store path
- [x] Add integration coverage for writer-lease exclusion and post-close reacquire path
- [x] Replace `USearchVectorEngine` stub with deterministic in-memory cosine ranking baseline (`Add/AddBatch/Remove/Search`)
- [x] Add `usearch_vector_engine` unit suite (constructor/dimension validation, tie-break determinism, remove, top-k clamp)
- [x] Replace `MiniLMEmbedderTorch` throw stubs with deterministic CPU fallback embeddings (token hash projection + optional L2 normalization)
- [x] Add embedding unit suite (identity/dimension contract, determinism, normalization, batch parity)
- [x] Implement `MemoryOrchestrator::Remember` baseline write-path (`content -> WaxStore::Put`) and auto-create store when file is missing
- [x] Add MemoryOrchestrator unit coverage (vector policy guard + remember/flush persistence roundtrip)
- [x] Implement `MemoryOrchestrator::Recall` baseline over committed store frames (deterministic text-overlap ranking + score/frame_id ordering)
- [x] Implement `BuildFastRAGContext` baseline materialization (deterministic result ordering, preview clamp, token counting, NaN score normalization)
- [x] Add search/unit coverage for deterministic ordering, top-k clamp, preview truncation, token counting, and orchestrator recall ranking
- [x] Implement `UnifiedSearchWithCandidates` baseline with mode-aware channel routing (`text-only`, `vector-only`, `hybrid`) and deterministic RRF fusion
- [x] Wire orchestrator recall path to mode-aware unified search with text/vector candidate channel generation from store + embedder
- [x] Add embedding memoization baseline in orchestrator (`frame_id -> embedding` cache, populated on remember and reused in vector recall path)
- [x] Add orchestrator memoization test with counting embedder (repeated recalls reuse cached doc embeddings)
- [x] Add token-budget enforcement in `BuildFastRAGContext` (`snippet_max_tokens` and `max_context_tokens` with deterministic partial truncation)
- [x] Add batch embedding path in orchestrator vector recall (`BatchEmbeddingProvider::EmbedBatch` for missing document embeddings)
- [x] Add unit coverage for token-budget clamp and batch-provider vector recall behavior
- [x] Wire `FastRAGConfig.max_snippets` into orchestrator recall request and enforce snippet-count clamp through `top_k`
- [x] Implement deterministic remember-time chunking baseline (`target_tokens` + `overlap_tokens`) with per-chunk frame ingestion
- [x] Add unit coverage for chunking split behavior and chunk payload persistence order
- [x] Implement RAG item policy baseline in context assembly (`kExpanded` for primary item, `kSnippet` for subsequent items, `kSurrogate` fallback on missing preview)
- [x] Wire `expansion_max_tokens` through recall request and enforce per-kind token limits in context materialization
- [x] Add unit coverage for RAG item kind policy and surrogate fallback behavior
- [x] Add batch embedding path to remember-time ingest (`BatchEmbeddingProvider::EmbedBatch` for multi-chunk remember)
- [x] Add unit coverage for batch-provider remember behavior (single batch call, no per-chunk embed calls)
- [x] Respect `OrchestratorConfig.ingest_batch_size` in remember-time batch embedding path (split `EmbedBatch` into deterministic slices)
- [x] Add unit coverage for ingest batch-size slicing behavior (expected number of batch calls)
- [x] Add mode-aware channel gating in orchestrator recall (`text-only` skips vector embedding path, `vector-only` skips text scoring)
- [x] Add unit coverage ensuring text-only recall does not invoke embedder
- [x] Add structured-memory baseline module with deterministic in-memory CRUD/query semantics
- [x] Add unit coverage for structured-memory upsert/versioning/remove/prefix-query ordering/validation behavior
- [x] Integrate structured-memory baseline into `MemoryOrchestrator` (`RememberFact`/`RecallFactsByEntityPrefix` API surface)
- [x] Add orchestrator-level structured-memory scenario (fact upsert/versioning/prefix ordering)
- [x] Feed structured-memory entries into orchestrator recall text-channel as `SearchSource::kStructuredMemory` candidates
- [x] Add recall coverage validating structured-memory hits appear in RAG context
- [x] Persist structured-memory facts via internal orchestrator journal records stored in `WaxStore` frames
- [x] Replay structured-memory fact journal on orchestrator startup to restore facts across `close/reopen`
- [x] Exclude internal structured-memory journal frames from regular text/vector candidate scans to prevent source leakage
- [x] Extend structured-memory journal codec with remove operation and replay support
- [x] Add orchestrator `ForgetFact` API with persisted remove journal entries
- [x] Add reopen parity coverage for fact removal and recall exclusion after delete
- [x] Switch orchestrator text recall channel to `FTS5SearchEngine` baseline (document + structured-fact indexes) with deterministic source attribution
- [x] Extend `FTS5SearchEngine` with two-phase staging primitives (`StageIndex/StageIndexBatch/StageRemove/CommitStaged/RollbackStaged`)
- [x] Add text-index staging tests for visibility (pre-commit hidden), rollback, and deterministic staged apply order
- [x] Integrate staged text indexes into orchestrator lifecycle (`Remember/RememberFact/ForgetFact` stage, `Flush` commits index mutations)
- [x] Rebuild committed text indexes on orchestrator startup from store + structured-memory replay state
- [x] Add orchestrator-level flush-gating coverage for text recall visibility
- [x] Extend `USearchVectorEngine` with two-phase staging primitives (`StageAdd/StageAddBatch/StageRemove/CommitStaged/RollbackStaged`)
- [x] Add vector-index staging tests for visibility (pre-commit hidden), rollback, and deterministic staged apply order
- [x] Integrate committed vector index into orchestrator lifecycle (`Remember` stages vectors, `Flush` commits vector mutations, startup rebuild from committed store)
- [x] Switch vector recall candidate path to committed `USearchVectorEngine` search results (no per-recall document re-embedding)
- [x] Add orchestrator-level flush-gating coverage for vector recall visibility
- [x] Add orchestrator reopen coverage for vector index rebuild (committed vectors searchable after restart)
- [x] Add orchestrator close-without-flush vector coverage (store close auto-commit + vector rebuild on reopen)
- [x] Allow vector-only recall with explicit embedding and empty query string
- [x] Add flush-failure regression coverage: failed store commit must not expose staged text index mutations
- [x] Add flush-failure regression coverage for vector channel: failed store commit must not expose staged vector index mutations
- [x] Add orchestrator config policy checks for incompatible `search_mode` / enabled-channel combinations
- [x] Harden text recall channel by validating text-index hits against committed frame metadata and payload
- [ ] Implement M3+ functionality (WAL/store write/search/rag parity)

## Modified Files

| File | Change Summary | Agent |
|------|---------------|-------|
| `.gitmodules` | Added required submodule declarations | Codex |
| `.gitignore` | Added C++ build artifact ignores | Codex |
| `.github/workflows/cpp-ci.yml` | Added C++ CI job with submodule sync/update/verify; strict parity config now enables `WAXCPP_REQUIRE_SWIFT_FIXTURES=ON` | Codex |
| `Tasks/cpp-port-libtorch-plan.md` | Saved implementation roadmap | Codex |
| `cpp/CMakeLists.txt` | Added C++ build/test scaffold | Codex |
| `cpp/README.md` | Added workspace purpose/build docs | Codex |
| `cpp/submodules.lock` | Added dependency lock policy skeleton | Codex |
| `cpp/scripts/verify_submodules.py` | Added policy consistency verifier | Codex |
| `cpp/src/core/sha256.hpp` | Added internal SHA-256 interface | Codex |
| `cpp/src/core/sha256.cpp` | Added SHA-256 implementation | Codex |
| `cpp/src/core/mv2s_format.hpp` | Added MV2S constants and codec interfaces | Codex |
| `cpp/src/core/mv2s_format.cpp` | Added MV2S header/footer codec + TOC encoder/decoder with structural checks | Codex |
| `cpp/src/core/wax_store.cpp` | Added create/open/verify read-path with deep verify (stored/plain checksum model), footer arbitration (header/snapshot/scan), TOC decode, frame+segment range checks | Codex |
| `cpp/include/waxcpp/wax_store.hpp` | Added internal load-state fields/helpers | Codex |
| `cpp/tests/unit/wax_store_verify_test.cpp` | Added M2 unit test for create/open/verify/header fallback | Codex |
| `cpp/tests/unit/mv2s_format_test.cpp` | Added TOC codec invariants test suite (roundtrip + checksum + version + dense IDs + optional tags) | Codex |
| `cpp/tests/parity/mv2s_fixture_parity_test.cpp` | Added fixture-driven parity test runner for `.mv2s` (Open/Verify + sidecar modes + optional error matching) | Codex |
| `cpp/tests/parity/mv2s_fixture_generator.cpp` | Added deterministic small-WAL synthetic fixture generator (`pass/open_fail/verify_fail`) | Codex |
| `cpp/tests/test_logger.hpp` | Added opt-in/Debug-default test logger (`WAXCPP_TEST_LOG`) for cleaner expected-failure diagnostics | Codex |
| `cpp/tests/unit/wax_store_verify_test.cpp` | Added scenario logs and expected-exception logging to reduce Visual Studio first-chance exception ambiguity | Codex |
| `cpp/CMakeLists.txt` | Added parity test target and strict fixture gating option (`WAXCPP_REQUIRE_PARITY_FIXTURES`) | Codex |
| `.github/workflows/cpp-ci.yml` | Enabled strict fixture requirement and fixture generation step in C++ CI | Codex |
| `cpp/tests/parity/README.md` | Added parity test and sidecar format documentation | Codex |
| `fixtures/parity/README.md` | Added fixture and sidecar conventions | Codex |
| `cpp/src/core/mv2s_format.hpp` | Extended TOC summary with index manifest metadata (`lex/vec/time`) | Codex |
| `cpp/src/core/mv2s_format.cpp` | Added decode-time validation that index manifests have matching segment catalog entries | Codex |
| `cpp/tests/unit/mv2s_format_test.cpp` | Added M2 tests for manifest/segment linkage fail/pass scenarios | Codex |
| `cpp/tests/parity/mv2s_fixture_generator.cpp` | Added synthetic fixture generation for lex-manifest/segment-catalog mismatch (`open_fail`) | Codex |
| `fixtures/parity/synthetic/*` | Regenerated synthetic fixtures; added manifest-linkage failure fixture and sidecar | Codex |
| `cpp/tests/parity/mv2s_fixture_parity_test.cpp` | Added non-synthetic fixture classification and strict Swift fixture gate enforcement | Codex |
| `cpp/CMakeLists.txt` | Added `WAXCPP_REQUIRE_SWIFT_FIXTURES` option and compile definition for parity test target | Codex |
| `cpp/tests/parity/README.md` | Documented strict Swift fixture gate and expected fixture layout | Codex |
| `fixtures/parity/swift/README.md` | Added Swift fixture directory contract and naming guidance | Codex |
| `Package.swift` | Added `WaxParityFixtureGenerator` executable target | Codex |
| `Sources/WaxParityFixtureGenerator/main.swift` | Added Swift fixture generator for pass/open_fail/verify_fail parity artifacts; switched to compact WAL (`64 KiB`) to avoid oversized fixture files | Codex |
| `.github/workflows/swift-parity-fixtures.yml` | Added manual macOS workflow to generate/upload Swift parity fixture artifacts; pinned setup step to Swift 6.2 | Codex |
| `cpp/src/core/wal_ring.hpp` | Added WAL record header model, pending mutation scan types, and reader interfaces (`IsTerminalMarker`, `ScanWalState`, `ScanPendingMutationsWithState`) | Codex |
| `cpp/src/core/wal_ring.cpp` | Added WAL state scanner, terminal marker detection, and pending mutation payload decode for `putFrame/delete/supersede/putEmbedding` with decoded payload metadata | Codex |
| `cpp/tests/unit/wal_ring_test.cpp` | Added focused WAL ring test coverage for sentinel detection, decode-stop semantics, wrap/padding handling, and scan-state consistency | Codex |
| `cpp/src/core/wal_ring.hpp` | Added `WalRingWriter` interface (append/canAppend/checkpoint + writer counters/state accessors) | Codex |
| `cpp/src/core/wal_ring.cpp` | Added `WalRingWriter` implementation with padding wrap handling, inline/separate sentinel writes, and capacity guards | Codex |
| `cpp/tests/unit/wal_ring_writer_test.cpp` | Added writer-side WAL tests for append semantics, wrap padding parity, checkpoint reset, and capacity overflow behavior | Codex |
| `cpp/CMakeLists.txt` | Added `waxcpp_wal_ring_writer_test` target to C++ test matrix | Codex |
| `cpp/src/core/mv2s_format.hpp` | Extended frame summary model with lifecycle fields (`status`, `supersedes`, `superseded_by`) | Codex |
| `cpp/src/core/mv2s_format.cpp` | Wired TOC encode/decode for frame lifecycle fields to preserve delete/supersede semantics across commit/reopen | Codex |
| `cpp/src/core/wal_ring.hpp` | Extended decoded WAL putFrame payload model with canonical/stored checksum + canonical metadata fields | Codex |
| `cpp/src/core/wal_ring.cpp` | Decodes WAL putFrame canonical/stored checksums for commit-time apply after recovery | Codex |
| `cpp/src/core/wax_store.cpp` | Implemented WAL-backed write operations (`Put/PutBatch/Delete/Supersede`), commit apply path, header/footer rewrite on commit, and WAL stats counter plumbing | Codex |
| `cpp/include/waxcpp/wax_store.hpp` | Added internal header/WAL runtime fields needed by write-path + commit sequencing | Codex |
| `cpp/tests/unit/wax_store_write_test.cpp` | Added write-path scenarios covering commit persistence and pending WAL recovery behavior | Codex |
| `cpp/CMakeLists.txt` | Added `waxcpp_wax_store_write_test` target to C++ test matrix | Codex |
| `cpp/src/core/wax_store_test_hooks.hpp` | Added test-only commit failpoint hooks for deterministic crash-window simulation | Codex |
| `cpp/src/core/wax_store.cpp` | Added step-based commit failpoint injection and wired crash-window hooks | Codex |
| `cpp/tests/unit/wax_store_write_test.cpp` | Added crash-window tests for failures after TOC, after footer, and after header A publication | Codex |
| `cpp/src/core/wax_store.cpp` | Added supersede apply validation (cycle detection and conflict checks) to prevent inconsistent frame graphs | Codex |
| `cpp/tests/unit/wax_store_write_test.cpp` | Added negative scenarios for supersede cycle and conflicting supersede edge rejection at commit | Codex |
| `cpp/src/core/wax_store.cpp` | Updated `Close()` semantics to auto-commit local pending mutations and avoid auto-committing recovery-only pending state | Codex |
| `cpp/tests/unit/wax_store_write_test.cpp` | Added `close` auto-commit parity coverage and adjusted crash/pending scenarios to model abrupt process termination explicitly | Codex |
| `cpp/include/waxcpp/wax_store.hpp` | Added read API declarations for committed frames (`FrameMeta(s)`, `FrameContent(s)`) | Codex |
| `cpp/src/core/wax_store.cpp` | Added committed frame cache and read API implementations backed by `.mv2s` payload reads | Codex |
| `cpp/tests/unit/wax_store_write_test.cpp` | Added frame read API parity scenario (meta/status/content before and after reopen) | Codex |
| `cpp/tests/unit/wax_store_write_test.cpp` | Added regression for `Close()` on recovered pending WAL to lock local-vs-recovered auto-commit semantics | Codex |
| `cpp/tests/unit/wax_store_write_test.cpp` | Added recovered-pending plus local mutation merge/commit scenario to validate mixed pending sequencing | Codex |
| `cpp/src/core/wax_store.cpp` | Replaced pending-WAL fail gate with Swift-like pending scan integration, `requiredEnd` protection, open-time trailing-byte repair, non-mutating verify path, `Open(path, repair)` control, internal WAL state capture, and `WalStats()` reporting | Codex |
| `cpp/include/waxcpp/wax_store.hpp` | Added internal WAL runtime state fields plus public `WaxWALStats`/`WalStats()` API and `Open(path, repair)` overload | Codex |
| `cpp/tests/unit/wax_store_verify_test.cpp` | Extended WAL parity scenarios with tail-repair checks, explicit WAL-state assertions, non-mutating verify regression, clean-WAL cursor normalization coverage, and `open(repair=false)` regression | Codex |
| `cpp/include/waxcpp/fts5_search_engine.hpp` | Added in-memory document store backing for text index/search baseline | Codex |
| `cpp/src/text/fts5_search_engine.cpp` | Implemented deterministic tokenized text search baseline (`Index/IndexBatch/Remove/Search`) with TF-IDF scoring and frame-id tie-break | Codex |
| `cpp/tests/unit/fts5_search_engine_test.cpp` | Added text-engine unit coverage for ranking, deterministic ties, remove behavior, batch validation, and empty-input semantics | Codex |
| `cpp/CMakeLists.txt` | Added `waxcpp_fts5_search_engine_test` target to C++ test matrix | Codex |
| `cpp/include/waxcpp/wax_store.hpp` | Marked `WaxStore` non-copyable and added writer-lease ownership state | Codex |
| `cpp/src/core/wax_store.cpp` | Added writer lease acquire/release lifecycle (`.writer.lock`) in open/close path | Codex |
| `cpp/tests/unit/wax_store_write_test.cpp` | Added writer-lease exclusion scenario (competing open fails until primary close) | Codex |
| `cpp/include/waxcpp/vector_engine.hpp` | Added in-memory vector storage backing for `USearchVectorEngine` baseline | Codex |
| `cpp/src/vector/usearch_vector_engine.cpp` | Implemented deterministic CPU vector search baseline (cosine similarity + frame-id tie-break + dimension checks) | Codex |
| `cpp/tests/unit/usearch_vector_engine_test.cpp` | Added vector-engine unit coverage for ranking, validation errors, remove, and top-k behavior | Codex |
| `cpp/CMakeLists.txt` | Added `waxcpp_usearch_vector_engine_test` target to C++ test matrix | Codex |
| `cpp/src/rag/embeddings.cpp` | Implemented deterministic CPU fallback for `MiniLMEmbedderTorch` (`Embed/EmbedBatch`) with hash projection + L2 normalization | Codex |
| `cpp/tests/unit/embeddings_test.cpp` | Added embedding baseline tests for determinism, normalization, and batch consistency | Codex |
| `cpp/CMakeLists.txt` | Added `waxcpp_embeddings_test` target to C++ test matrix | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Implemented baseline `Remember` path and constructor auto-create behavior for missing store files | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added orchestrator unit tests for vector-policy validation and persisted remember/flush flow | Codex |
| `cpp/CMakeLists.txt` | Added `waxcpp_memory_orchestrator_test` target to C++ test matrix | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added baseline store-backed recall path with deterministic text overlap ranking | Codex |
| `cpp/src/rag/search.cpp` | Implemented deterministic unified search baseline with mode-aware channel selection + hybrid RRF, and `BuildFastRAGContext` materialization | Codex |
| `cpp/include/waxcpp/search.hpp` | Added `UnifiedSearchWithCandidates` API for explicit text/vector candidate fusion | Codex |
| `cpp/tests/unit/search_test.cpp` | Added search unit suite for context materialization plus mode/hybrid RRF behavior | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Extended with recall ranking scenario, hybrid-with-embedder scenario, and per-scenario isolated fixture paths | Codex |
| `cpp/CMakeLists.txt` | Added `waxcpp_search_test` target to C++ test matrix | Codex |
| `cpp/include/waxcpp/memory_orchestrator.hpp` | Added orchestrator embedding-cache state | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added vector embedding cache population/reuse in remember/recall path | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added counting-embedder memoization scenario for repeated vector recalls | Codex |
| `cpp/include/waxcpp/types.hpp` | Extended `SearchRequest` with context/token budget fields (`max_context_tokens`, `snippet_max_tokens`) | Codex |
| `cpp/src/rag/search.cpp` | Added deterministic token-budget clamp in context assembly (snippet cap + total-context cap with partial final snippet) | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added batch-provider vector embedding path and wired FastRAG budget fields from config into recall request | Codex |
| `cpp/tests/unit/search_test.cpp` | Added context-budget clamp scenario for deterministic snippet and total token truncation | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added batch-embedder scenario to verify `EmbedBatch` usage in vector recall | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added `max_snippets` clamp wiring (`req.top_k=min(search_top_k,max_snippets)`) | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added recall scenario validating `max_snippets` clamp behavior | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added remember-time chunking (`target_tokens`/`overlap_tokens`) and chunk-level embedding cache population | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added chunking scenario validating frame count and deterministic chunk payloads | Codex |
| `cpp/include/waxcpp/types.hpp` | Extended `SearchRequest` with `expansion_max_tokens` for expanded-context policy control | Codex |
| `cpp/src/rag/search.cpp` | Added deterministic RAG item kind policy (`expanded/snippet/surrogate`) and per-kind token clamping | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Wired FastRAG `expansion_max_tokens` into recall request path | Codex |
| `cpp/tests/unit/search_test.cpp` | Added item-kind policy and surrogate fallback scenarios; updated budget expectations for expanded-item behavior | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added remember-time batch embedding path for `BatchEmbeddingProvider` on multi-chunk ingest | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added remember-time batch-embedder scenario validating batch invocation and no per-chunk fallback calls | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added `ingest_batch_size`-aware slicing for remember-time `EmbedBatch` calls | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added remember-time batch-size slicing scenario (`5 chunks`, `batch_size=2` => `3 batch calls`) | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added mode-aware channel gating to avoid vector embedding work in text-only recalls | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added text-only recall scenario validating embedder is not called | Codex |
| `cpp/include/waxcpp/structured_memory.hpp` | Added structured-memory public API (`StructuredMemoryStore`, entry model) | Codex |
| `cpp/src/text/structured_memory_store.cpp` | Implemented deterministic in-memory structured-memory CRUD and prefix-query behavior | Codex |
| `cpp/tests/unit/structured_memory_store_test.cpp` | Added structured-memory unit suite (upsert/get/version/remove/query/validation) | Codex |
| `cpp/CMakeLists.txt` | Added structured-memory sources/header and `waxcpp_structured_memory_store_test` target | Codex |
| `cpp/include/waxcpp/memory_orchestrator.hpp` | Added orchestrator-level structured-memory API (`RememberFact`, `RecallFactsByEntityPrefix`) and state | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Wired structured-memory store into orchestrator with baseline fact CRUD/query methods | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added orchestrator structured-memory facts scenario (upsert/version/order validation) | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added structured-memory recall candidate channel (`kStructuredMemory`) merged into unified text search path | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added recall scenario validating structured-memory sources are present in context results | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added internal structured-fact journal codec (encode/parse), fact persistence in `RememberFact`, startup replay from store, and internal-frame filtering from normal channels | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Extended structured-memory scenarios to validate persistence across reopen and absence of leaked text-source hits from internal fact journal frames | Codex |
| `cpp/include/waxcpp/memory_orchestrator.hpp` | Added `ForgetFact(entity, attribute)` API for structured-memory delete parity | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Extended structured fact journal format with remove opcode; replay now applies upsert/remove deterministically | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added persisted remove scenario (`remove -> flush -> reopen`) and recall exclusion checks | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Replaced local text-overlap scoring with `FTS5SearchEngine`-backed text channel construction for store docs and structured facts | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added source-attribution coverage to ensure store text recall keeps `SearchSource::kText` after FTS-backed channel switch | Codex |
| `cpp/include/waxcpp/fts5_search_engine.hpp` | Added two-phase text indexing API (`Stage*`, `CommitStaged`, `RollbackStaged`, pending mutation introspection) | Codex |
| `cpp/src/text/fts5_search_engine.cpp` | Implemented deterministic staged mutation queue for text index; legacy `Index/IndexBatch/Remove` now immediate wrappers over stage+commit | Codex |
| `cpp/tests/unit/fts5_search_engine_test.cpp` | Added staged-index unit scenarios for commit visibility, rollback behavior, and ordered mutation application | Codex |
| `cpp/include/waxcpp/memory_orchestrator.hpp` | Added persistent orchestrator-owned text indexes for store content and structured facts | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Switched recall text channel to long-lived staged indexes; constructor now rebuilds committed index snapshots and flush commits staged text mutations | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added flush-gating scenario proving staged text/fact mutations are invisible before flush and visible after commit | Codex |
| `cpp/include/waxcpp/vector_engine.hpp` | Added two-phase vector indexing API (`Stage*`, `CommitStaged`, `RollbackStaged`, pending mutation introspection) | Codex |
| `cpp/src/vector/usearch_vector_engine.cpp` | Implemented deterministic staged mutation queue for vector index; legacy `Add/AddBatch/Remove` now immediate wrappers over stage+commit | Codex |
| `cpp/tests/unit/usearch_vector_engine_test.cpp` | Added staged-vector unit scenarios for commit visibility, rollback behavior, and ordered mutation application | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Switched vector recall channel to committed vector index hits; constructor rebuilds vector index from committed store with embedder batch support | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Updated vector recall expectations for committed index path and added flush-gating scenario for vector visibility | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added vector reopen/close lifecycle scenarios validating committed index rebuild and no re-embed on explicit vector recall | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Relaxed recall preconditions so vector-only path can run with explicit embedding even when query is empty | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added regression scenario for `Recall(\"\", embedding)` in vector-only mode | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added failpoint-driven flush failure scenario ensuring staged text remains hidden until successful retry commit | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added failpoint-driven flush failure scenario ensuring staged vector results remain hidden until successful retry commit | Codex |
| `cpp/src/orchestrator/memory_orchestrator.cpp` | Added constructor policy validation for `search_mode` vs enabled channels and extra filtering of text index hits against committed store frame state | Codex |
| `cpp/tests/unit/memory_orchestrator_test.cpp` | Added policy-validation scenarios for invalid text-only/vector-only/hybrid configuration combinations | Codex |
| `cpp/CMakeLists.txt` | Added `src/core/wal_ring.cpp` to waxcpp target | Codex |
| `cpp/include/waxcpp/*.hpp` | Added public API skeletons | Codex |
| `cpp/src/**/*.cpp` | Added module stubs | Codex |
| `cpp/tests/**` | Added smoke + placeholders | Codex |
| `fixtures/parity/README.md` | Initialized parity fixture area | Codex |

## Wax Architecture Context

- **Actor boundaries crossed**: Planned parity for `MemoryOrchestrator` actor semantics via serialized C++ orchestrator runtime.
- **Frame kinds involved**: Core frame model only (no new multimodal kinds introduced).
- **Metadata keys introduced/changed**: None (reserved for future implementation milestones).
- **Index implications**: Text and vector indexes now have deterministic in-memory two-phase staging in C++; orchestrator commits them on `flush` and rebuilds committed snapshots on reopen.
- **Token budget impact**: FastRAG/token budgeting types remain scaffolded, logic pending.
- **Invariants in play**: 1, 2, 4, 6, 7, 8, 9 explicitly tracked; M2 work directly advances deterministic retrieval and two-phase safety foundations.

## Handoff Notes
M1 and M2 are complete. M3 baseline is in place: C++ parses WAL headers, detects terminal markers for replay snapshot/header cursor fast paths, scans pending mutations with Swift-compatible decode-stop semantics, validates pending putFrame payload ranges, truncates trailing bytes on open while preserving bytes referenced by pending putFrame, stores effective WAL open-state internally, supports WAL append/capacity/padding-wrap/sentinel/checkpoint behavior via `WalRingWriter`, and wires that into `WaxStore::Put/PutBatch/Delete/Supersede/Commit`. `Commit` applies decoded pending WAL mutations into TOC, writes new footer/header generations, and checkpoints WAL cursor state. Crash-window behavior is covered by deterministic failpoint tests for post-TOC/pre-footer, post-footer/pre-header, and single-header-published windows. Writer-lease exclusion is now enforced in `Open/Create` with `.writer.lock` sentinel semantics and reopen-after-close test coverage. Search stack baseline now includes deterministic mode-aware unified fusion (`UnifiedSearchWithCandidates`, text/vector channel routing, hybrid RRF), context materialization with explicit token budgeting and item-kind policy (`BuildFastRAGContext` with expanded/snippet/surrogate policy), and orchestrator `Remember/Recall` over committed store frames with deterministic chunking ingest. Text and vector recall channels now run via committed two-phase indexes (stage on ingest, commit on `flush`, rebuild on reopen). Structured-memory baseline now has orchestrator-level persistence via internal store-backed fact journal and startup replay, plus recall integration as `kStructuredMemory` candidates without internal-record leakage into text/vector channels.

## Open Questions
1. Final remote for `cpp/third_party/libtorch-dist` should be replaced with dedicated artifact mirror before release.
2. Pin commits in `cpp/submodules.lock` are placeholders and must be resolved in dependency PR.
3. Decide when to enforce failure on `<PIN_REQUIRED>` in dependency lock checks (currently warning-only policy path).
4. Implement full WAL replay apply path (materialize decoded pending mutations into store/index state and commit/checkpoint transitions).

