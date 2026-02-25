# Context: UE5 Indexing with Wax + llama.cpp (No Torch)

**Created**: 2026-02-25  
**Current Phase**: M3 complete, M4 in progress  
**Owner**: wax-rag-specialist

## Decisions Locked
1. Generation model: `Qwen3-Coder-Next-Q4_K_M.gguf` (256K context).
2. Runtime source: local llama.cpp binaries from `WAXCPP_LLAMA_CPP_ROOT`.
3. Submodule for llama.cpp is deferred to later iterations.
4. Runtime config must separate:
   - `generation_model`
   - `embedding_model`
5. Torch/libtorch embedding runtime is forbidden for this track.

## Implemented in this session
1. Added runtime model contract in C++ core:
   - `cpp/include/waxcpp/runtime_model_config.hpp`
   - `cpp/src/rag/runtime_model_config.cpp`
2. Added server runtime config loading:
   - `cpp/server/runtime_config.hpp`
   - `cpp/server/runtime_config.cpp`
3. Wired server startup to runtime config + validation:
   - `cpp/server/main.cpp`
   - `cpp/server/wax_rag_handler.hpp`
   - `cpp/server/wax_rag_handler.cpp`
4. Added build wiring for server runtime config and runtime model test target:
   - `cpp/server/CMakeLists.txt`
   - `cpp/CMakeLists.txt`
5. Added runtime config docs for llama.cpp env/config usage:
   - `cpp/README.md`
6. Added indexing skeleton in server:
   - `cpp/server/index_job_manager.hpp`
   - `cpp/server/index_job_manager.cpp`
   - `cpp/tests/unit/index_job_manager_test.cpp`
   - RPC methods: `index.start`, `index.status`, `index.stop`
7. Added deterministic UE5 scanner integration:
   - `cpp/server/ue5_filesystem_scanner.hpp`
   - `cpp/server/ue5_filesystem_scanner.cpp`
   - `cpp/tests/unit/ue5_filesystem_scanner_test.cpp`
   - `index.start` now writes `<checkpoint>.scan_manifest`
8. Added deterministic chunk-manifest builder (M4 partial):
   - `cpp/server/ue5_chunk_manifest.hpp`
   - `cpp/server/ue5_chunk_manifest.cpp`
   - `cpp/tests/unit/ue5_chunk_manifest_test.cpp`
   - `index.start` now writes `<checkpoint>.chunk_manifest`
   - `index_job_manager.Complete(scanned,indexed,committed)` now records scanned files and indexed chunk count
9. Added baseline ingest execution in `index.start`:
   - chunk stream callback runs `MemoryOrchestrator::Remember(...)` per chunk with deterministic metadata
   - periodic WAL commits via `Flush()` every 128 chunks
   - live checkpoint updates via `IndexJobManager::UpdateProgress(...)`
   - final `committed_chunks` persisted on completion
10. Added resume-time incremental skip by file content hash:
   - `Ue5FileDigest` + deterministic `.file_manifest` serialization/parsing
   - unchanged path detection (`previous` vs `current` manifests)
   - with `resume=true`, chunks from unchanged files are skipped during ingest
   - `index.start` now persists `<checkpoint>.file_manifest` after successful pass

## Validation Rules Now Enforced
1. `generation_model.runtime` must be `llama_cpp`.
2. `generation_model.model_path` must point to `.gguf`.
3. `embedding_model.runtime` cannot be `torch/libtorch`.
4. When llama runtime is used, `llama_cpp_root` is mandatory and must be an existing directory.
5. If both model paths are set and distinct-model policy is enabled, they cannot be identical.
6. Vector-search enablement requires embedding runtime to be `llama_cpp` with `.gguf` path.

## Pending Next Steps
1. Implement `LlamaCppEmbeddingProvider` and wire vector ingest/search path.
2. Add end-to-end retrieval + answer path with citation metadata.
3. Move long-running index execution off request thread (background worker + cancellation-safe stop).
4. Add crash-window regression tests for interrupted index job across manifests/checkpoints.

## Operational Notes
1. Server now expects llama runtime root via:
   - `WAXCPP_LLAMA_CPP_ROOT`
   - or `llama_cpp_root` in `WAXCPP_SERVER_CONFIG`.
2. Default generation model path remains:
   - `g:/Proj/Agents1/Models/Qwen/Qwen3-Coder-Next-Q4_K_M.gguf`
3. Index checkpoint state persists to:
   - `<store_path>.index.checkpoint` (line-based deterministic state file).
