# C++ Port Plan (Core RAG v1) with Side-by-Side Layout + Submodule Dependency Policy

## Summary
Port Core RAG Wax to C++20 with parity for `.mv2s`, deterministic retrieval, and two-phase indexing.
Swift remains in `Sources/` and `Tests/`.
C++ lives under `cpp/`.
All external dependencies are managed as git submodules.
LibTorch follows practical hybrid delivery via pinned submodule artifacts + checksum verification.

## Fixed Scope
- v1 includes Core RAG only.
- Photo/Video/PDF modules are out of scope for v1.

## Required Invariants (from `.claude/agents/wax-rag-specialist.md`)
1. Actor isolation equivalent in C++ runtime model.
2. Sendable boundary equivalent (thread-safe DTOs, immutable cross-thread payloads).
3. Frame hierarchy compatibility.
4. Supersede-not-delete behavior.
5. Capture-time semantics where applicable.
6. Deterministic retrieval and tie-breaks.
7. Protocol-driven providers.
8. On-device enforcement for core operations.
9. Two-phase indexing: stage then commit.

## Target Repository Layout
- `cpp/include/waxcpp/` public interfaces
- `cpp/src/core` file format/WAL/IO/codec
- `cpp/src/text` SQLite/FTS5/structured memory
- `cpp/src/vector` USearch and vec serialization
- `cpp/src/rag` unified search/FastRAG/tokenizer
- `cpp/src/orchestrator` MemoryOrchestrator parity layer
- `cpp/tests/{unit,integration,parity}`
- `cpp/third_party` submodules only
- `fixtures/parity/` shared fixtures

## Dependency Lock Policy
- `.gitmodules` is authoritative for dependency locations.
- `cpp/submodules.lock` stores expected repo path, pinned commit, and checksum policy.
- CI must run:
  - `git submodule sync --recursive`
  - `git submodule update --init --recursive`
  - `python cpp/scripts/verify_submodules.py`
- Any dependency update requires a dedicated dependency PR.

## Milestone Backlog
### M0 Contract + Invariants
- Lock parity matrix Swift API/tests -> C++ API/tests.
- Lock 9 invariants as acceptance gates.

### M1 Skeleton + Submodule Infra
- Create C++ layout and CMake root.
- Add `.gitmodules` entries for usearch/sqlite/googletest/libtorch-dist.
- Add CI submodule sync/update and verification.

### M2 Binary Codec + Read Path
- Implement LE codec.
- Implement header/footer/TOC/frame decode + checksums.
- Implement read-only open + verify.

### M3 WAL + Recovery
- Implement WAL ring append/replay/wrap/checkpoint.
- Implement replay snapshot path and fallback scan.

### M4 Store Write Path
- Implement create/open/put/putBatch/delete/supersede/commit/close.
- Implement staged mutations and commit rules.

### M5 Text Search + Structured Memory
- Implement FTS5 schema parity and indexing/search APIs.
- Implement structured memory schema CRUD/query parity.

### M6 Vector Search CPU
- Implement USearch engine parity.
- Implement MV2V vec segment compatibility.

### M7 LibTorch Embeddings
- Implement provider interfaces and MiniLM LibTorch backend.
- Implement batching, normalization, memoization parity.

### M8 UnifiedSearch + FastRAG
- Implement SearchRequest/Response/Mode parity.
- Implement deterministic RRF, context assembly, token budget clamping.

### M9 MemoryOrchestrator Parity
- Implement remember/recall/flush/close lifecycle parity.
- Implement chunking/batching/concurrency semantics.

### M10 CUDA (Optional)
- Add cuda_preferred runtime policy with deterministic fallback.

### M11 Hardening + Release
- Fuzz + fault-injection + compatibility matrix + release docs.

## Current Status
- M0-M2 complete: binary format parity (`.mv2s`) with Swift fixtures, deep verify, checksum/range validation.
- M3-M4 baseline complete: WAL ring writer/scan/recovery, crash-window failpoints/reopen behavior, store write-path lifecycle parity.
- M5-M6 baseline complete: deterministic text/vector engines with two-phase staging and committed-index rebuild flows.
- M7-M9 baseline complete: deterministic embedding provider runtime policies, unified search/FastRAG/token-budget behavior, orchestrator lifecycle/concurrency/close guards.
- M10 optional baseline complete for policy surface: `cpu_only|cuda_preferred` routing, manifest diagnostics, CI policy matrix; real CUDA/libtorch runtime backend remains future work.
- M11 hardening in progress: deterministic fuzzing/regressions for TOC/MV2V/WAL parsers and crash-recovery paths are active; remaining release tasks include dependency pin finalization and dedicated artifact mirror cutover.
