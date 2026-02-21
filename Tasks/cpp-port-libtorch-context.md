# Context: C++ Core RAG Port (LibTorch)

**Created**: 2026-02-18
**Last Updated**: 2026-02-21
**Current Phase**: M3 incremental implementation (WAL pending scan + open-state parity)
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
| `cpp/src/core/wax_store.cpp` | Replaced pending-WAL fail gate with Swift-like pending scan integration, `requiredEnd` protection, open-time trailing-byte repair, non-mutating verify path, `Open(path, repair)` control, internal WAL state capture, and `WalStats()` reporting | Codex |
| `cpp/include/waxcpp/wax_store.hpp` | Added internal WAL runtime state fields plus public `WaxWALStats`/`WalStats()` API and `Open(path, repair)` overload | Codex |
| `cpp/tests/unit/wax_store_verify_test.cpp` | Extended WAL parity scenarios with tail-repair checks, explicit WAL-state assertions, non-mutating verify regression, clean-WAL cursor normalization coverage, and `open(repair=false)` regression | Codex |
| `cpp/CMakeLists.txt` | Added `src/core/wal_ring.cpp` to waxcpp target | Codex |
| `cpp/include/waxcpp/*.hpp` | Added public API skeletons | Codex |
| `cpp/src/**/*.cpp` | Added module stubs | Codex |
| `cpp/tests/**` | Added smoke + placeholders | Codex |
| `fixtures/parity/README.md` | Initialized parity fixture area | Codex |

## Wax Architecture Context

- **Actor boundaries crossed**: Planned parity for `MemoryOrchestrator` actor semantics via serialized C++ orchestrator runtime.
- **Frame kinds involved**: Core frame model only (no new multimodal kinds introduced).
- **Metadata keys introduced/changed**: None (reserved for future implementation milestones).
- **Index implications**: Read-only format + TOC structural validation are now real; text/vector indexing still scaffolded.
- **Token budget impact**: FastRAG/token budgeting types remain scaffolded, logic pending.
- **Invariants in play**: 1, 2, 4, 6, 7, 8, 9 explicitly tracked; M2 work directly advances deterministic retrieval and two-phase safety foundations.

## Handoff Notes
M1 and M2 are complete. M3 has advanced from read-side guards to pending scan + repair parity behavior: C++ now parses WAL headers, detects terminal markers for replay snapshot/header cursor fast paths, scans pending mutations with Swift-compatible decode-stop semantics, validates pending putFrame payload ranges, truncates trailing bytes on open while preserving bytes referenced by pending putFrame, and stores effective WAL open-state internally. Full mutation apply/replay into committed state is still pending.

## Open Questions
1. Final remote for `cpp/third_party/libtorch-dist` should be replaced with dedicated artifact mirror before release.
2. Pin commits in `cpp/submodules.lock` are placeholders and must be resolved in dependency PR.
3. Decide when to enforce failure on `<PIN_REQUIRED>` in dependency lock checks (currently warning-only policy path).
4. Implement full WAL replay apply path (materialize decoded pending mutations into store/index state and commit/checkpoint transitions).

