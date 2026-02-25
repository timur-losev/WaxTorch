# UE5 Codebase Indexing Plan (Wax + llama.cpp, no Torch)

## Summary
Target: index Unreal Engine 5 C++ source code into Wax with WAL-safe ingest and use `llama.cpp` for model runtime.

Current generation model decision:
- `g:/Proj/Agents1/Models/Qwen/Qwen3-Coder-Next-Q4_K_M.gguf` (256K context).

Current runtime source decision:
- local llama.cpp runtime root from `WAXCPP_LLAMA_CPP_ROOT` (no submodule in this iteration).

## Status Snapshot (2026-02-25)
- M0 complete.
- M1 complete.
- M2 complete (index RPC + checkpoint state machine).
- M3 complete (deterministic UE5 scanner + scan manifest serialization + tests).
- M4 in progress:
  - implemented deterministic chunk-manifest builder (line/token-aware windows, metadata, deterministic chunk ids),
  - `index.start` now writes both `.scan_manifest` and `.chunk_manifest`,
  - `index.start` now performs text ingest into Wax (`remember` + periodic `flush`) and updates job progress counters,
  - added file hash manifest (`.file_manifest`) and resume-time unchanged-file skipping,
  - `index.start` switched to background worker (non-blocking RPC), `index.stop` now cancels and joins safely.
- M5 in progress:
  - added `LlamaCppEmbeddingProvider` (HTTP llama.cpp embedding endpoint adapter),
  - server now wires provider when `enable_vector_search=true`,
  - added parser/provider unit tests with deterministic request-function stubs,
  - added retry/backoff and bounded-concurrency `EmbedBatch` execution with key-dedup.
- M7 in progress:
  - added `LlamaCppGenerationClient` (HTTP llama.cpp completion adapter),
  - added `answer.generate` RPC method:
    - performs Recall,
    - assembles citation map from frame metadata (`relative_path`, `line_start`, `line_end`, `symbol`),
    - builds generation prompt with frame tags (`[frame:<id>]`),
    - calls generation endpoint and returns answer + citation array.
- M8 in progress:
  - `index.start` now accepts operational controls: `flush_every_chunks` and `max_files`,
  - UE5 scanner supports cancel callback; `index.stop` can interrupt scan phase before full traversal,
  - added deterministic regressions for scanner-cancel path and capped-scan indexing path.

## Scope
- Build an ingest/search/generation server path for very large C++ codebases (UE5 scale).
- Keep deterministic indexing and WAL-safe commit behavior.
- Avoid Torch runtime for both generation and embeddings in this track.

## Architecture
1. Wax:
   - stores chunk payloads + metadata
   - maintains WAL/commit safety
   - serves retrieval (text/vector/hybrid)
2. llama.cpp (generation):
   - uses Qwen3-Coder-Next GGUF for answer generation.
3. llama.cpp (embeddings, later milestone):
   - separate GGUF embedding model, independent from generation model path.

## Milestones
### M0. Runtime Contract and Policy Gates
- Add strict runtime model config schema:
  - `generation_model` and `embedding_model` are separate.
  - `generation_model.runtime=llama_cpp` and `.gguf` path required.
  - `embedding_model.runtime=torch/libtorch` forbidden.
  - `WAXCPP_LLAMA_CPP_ROOT` required whenever llama runtime is used.
- Add unit tests for the contract.

### M1. Server Config Loader
- Load runtime config from:
  - defaults
  - `WAXCPP_GENERATION_MODEL`
  - `WAXCPP_LLAMA_CPP_ROOT`
  - optional `WAXCPP_SERVER_CONFIG` JSON
- Validate config at server startup and fail early with explicit errors.

### M2. Indexing Endpoint Skeleton
- Add JSON-RPC endpoints for:
  - `index.start`
  - `index.status`
  - `index.stop`
- Add persistent job checkpoint state for resume after restart.
- Status:
  - Implemented baseline endpoint wiring in server dispatch and handler.
  - Implemented persistent checkpoint-backed `IndexJobManager` state machine (`idle/running/stopped/failed`).
  - Added dedicated unit coverage for start/stop/fail/reload/resume behavior.

### M3. UE5 Filesystem Scanner
- Deterministic scanner for UE5 tree:
  - include code extensions (`.h/.hpp/.cpp/.inl/.inc`)
  - exclude generated/build folders
  - stable sort order
- Add reproducibility test for scanner manifest.
- Status: implemented.

### M4. Code Chunking + Metadata
- Token-aware chunking with overlap and symbol-aware boundaries.
- Metadata fields:
  - repo_root, relative_path, language, symbol, line_start, line_end, hash.
- Deterministic chunk id generation from `(path, symbol, line range, hash)`.
- Status: partially implemented (chunk-manifest generator + deterministic ids + metadata serialization).

### M5. Embedding Provider via llama.cpp
- Implement `LlamaCppEmbeddingProvider` (batch-capable).
- Wire to Wax vector ingest path.
- Add retries/timeouts and bounded concurrency.
- Status: mostly implemented (sync HTTP path + timeout + memoization + parser coverage + retries/backoff + bounded batch concurrency).

### M6. WAL-Safe Massive Ingest
- Batch ingest with periodic commit/checkpoint.
- Resume-from-manifest-hash logic to skip unchanged files.
- Crash-recovery regression tests for mid-index interruption.
- Status: partially implemented (periodic flush/checkpoint + unchanged file skip via file digest manifest + async index stop/resume regression coverage, including interrupted-run + handler-recreate + resume path in `wax_rag_handler_index_test`).

### M7. Query Pipeline + RAG
- Query embedding + retrieval + deterministic rerank.
- Build context budget pipeline for Qwen3-Coder-Next 256K runtime.
- Add response path with source citations (`path + lines`).
- Status: partially implemented (answer generation endpoint + citation map; budget policy tuning and deeper integration tests pending).

### M8. Performance and Operational Controls
- Add ingest throttles: max RAM, batch size, worker count.
- Add server metrics/logging for indexing phases and failures.
- Add regression tests for deterministic outputs across repeated runs.
- Status: partially implemented (`max_files` + `flush_every_chunks` controls in `index.start`, cancel-aware scan support, regression coverage for both).

## Acceptance Gates
1. Same source tree produces identical chunk manifest and stable top-k ordering.
2. Interrupted indexing resumes without WAL corruption or duplicate committed chunks.
3. Server startup fails fast on invalid runtime config (clear error messages).
4. UE5-scale indexing can run incrementally (changed files only).
