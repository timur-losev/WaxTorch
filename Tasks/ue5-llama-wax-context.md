# Context: UE5 Indexing with Wax + llama.cpp (No Torch)

**Created**: 2026-02-25  
**Current Phase**: M0-M2 baseline bootstrap  
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

## Validation Rules Now Enforced
1. `generation_model.runtime` must be `llama_cpp`.
2. `generation_model.model_path` must point to `.gguf`.
3. `embedding_model.runtime` cannot be `torch/libtorch`.
4. When llama runtime is used, `llama_cpp_root` is mandatory and must be an existing directory.
5. If both model paths are set and distinct-model policy is enabled, they cannot be identical.
6. Vector-search enablement requires embedding runtime to be `llama_cpp` with `.gguf` path.

## Pending Next Steps
1. Implement `LlamaCppEmbeddingProvider` and runtime call path (HTTP or native binding).
2. Extend `index.start` from state-machine skeleton to real UE5 scanner + chunking + ingest.
3. Add progress accounting integration (`scanned_files/indexed_chunks/committed_chunks`) from live pipeline.
4. Add end-to-end retrieval + answer path with citation metadata.

## Operational Notes
1. Server now expects llama runtime root via:
   - `WAXCPP_LLAMA_CPP_ROOT`
   - or `llama_cpp_root` in `WAXCPP_SERVER_CONFIG`.
2. Default generation model path remains:
   - `g:/Proj/Agents1/Models/Qwen/Qwen3-Coder-Next-Q4_K_M.gguf`
3. Index checkpoint state persists to:
   - `<store_path>.index.checkpoint` (line-based deterministic state file).
