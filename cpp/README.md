# Wax C++ Port Workspace

This directory hosts the side-by-side C++20 implementation track for Wax Core RAG.

## Scope
- Core RAG parity with Swift implementation.
- `.mv2s` format compatibility and deterministic retrieval.

## Build
```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Debug
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure
```

Parity fixtures strict mode:
```bash
cmake -S cpp -B cpp/build -DWAXCPP_REQUIRE_PARITY_FIXTURES=ON
```

Generate synthetic parity fixtures:
```bash
cmake --build cpp/build --target waxcpp_mv2s_fixture_generator
./cpp/build/Debug/waxcpp_mv2s_fixture_generator
```

Test logging:
```bash
# bash
WAXCPP_TEST_LOG=1 ctest --test-dir cpp/build --output-on-failure
# powershell
$env:WAXCPP_TEST_LOG='1'; ctest --test-dir cpp/build --output-on-failure
```

Core format unit test:
```bash
ctest --test-dir cpp/build -R waxcpp_mv2s_format_test --output-on-failure
```

MiniLM runtime manifest policy (optional):
```bash
# Optional: enable real libtorch runtime backend at build time.
# Note: when runtime is ON, you must pass either WAXCPP_LIBTORCH_ROOT or Torch_DIR.
# System auto-discovery is intentionally disabled for deterministic builds.
#
# Point CMake to an unpacked local libtorch folder (no auto-download in CMake)
# Valid layouts:
#   <root>/share/cmake/Torch/TorchConfig.cmake
#   <root>/libtorch/share/cmake/Torch/TorchConfig.cmake
cmake -S cpp -B cpp/build \
  -DWAXCPP_ENABLE_LIBTORCH_RUNTIME=ON \
  -DWAXCPP_LIBTORCH_ROOT=/abs/path/to/libtorch

# Optional: explicit Torch package config directory override
cmake -S cpp -B cpp/build \
  -DWAXCPP_ENABLE_LIBTORCH_RUNTIME=ON \
  -DTorch_DIR=/abs/path/to/libtorch/share/cmake/Torch

# Override manifest lookup path
export WAXCPP_LIBTORCH_MANIFEST=/abs/path/to/libtorch-manifest.json

# Require manifest presence during MiniLM embedder construction
export WAXCPP_REQUIRE_LIBTORCH_MANIFEST=1

# Optional: force-enable/disable real libtorch runtime path at runtime
export WAXCPP_ENABLE_REAL_TORCH_RUNTIME=1

# Optional: require real runtime (throw if libtorch runtime is unavailable or runtime init fails)
export WAXCPP_REQUIRE_REAL_TORCH_RUNTIME=1

# Optional: TorchScript module path (if set, module forward is applied over runtime embedding tensor)
export WAXCPP_TORCH_SCRIPT_MODULE=/abs/path/to/embedder.pt

# Optional: override root directory used to resolve selected artifact relative paths
export WAXCPP_LIBTORCH_DIST_ROOT=/abs/path/to/cpp/third_party/libtorch-dist

# Optional: require selected artifact file checksum verification against manifest sha256
export WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256=1
```

Note: when `WAXCPP_LIBTORCH_DIST_ROOT` is set, selected artifact resolution is strict:
- selected artifact paths (relative and absolute) are constrained to that root (no `..` escape);
- manifest-directory fallback resolution is disabled.
- checksum gate also rejects empty selected artifact files.

Libtorch manifest checksum gate test (requires initialized `cpp/third_party/libtorch-dist`):
```bash
export WAXCPP_REQUIRE_LIBTORCH_MANIFEST=1
export WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256=1
export WAXCPP_LIBTORCH_MANIFEST=/abs/path/to/cpp/manifest/libtorch-manifest.json
export WAXCPP_LIBTORCH_DIST_ROOT=/abs/path/to/cpp/third_party/libtorch-dist
ctest --test-dir cpp/build --output-on-failure -R waxcpp_libtorch_manifest_gate_test
```

Windows CUDA artifact prep (PowerShell):
```powershell
# From repo root. Downloads/copies a CUDA libtorch zip into cpp/third_party/libtorch-dist
# and updates cpp/manifest/libtorch-manifest.json with path+sha256.
.\scripts\build-libtorch-windows-cuda.ps1 `
  -Url "https://download.pytorch.org/libtorch/cu124/libtorch-win-shared-with-deps-2.5.1%2Bcu124.zip"

# Or use a local zip you downloaded manually:
.\scripts\build-libtorch-windows-cuda.ps1 `
  -ZipPath "C:\Downloads\libtorch-win-shared-with-deps-2.5.1+cu124.zip"
```

Server runtime config (llama.cpp + GGUF):
```bash
# Required when server uses llama_cpp runtime:
# point to your local llama.cpp binaries/runtime root directory
export WAXCPP_LLAMA_CPP_ROOT=/abs/path/to/llama-cpp

# Optional: override generation model path (default is Qwen3-Coder-Next-Q4_K_M.gguf)
export WAXCPP_GENERATION_MODEL=/abs/path/to/Qwen3-Coder-Next-Q4_K_M.gguf

# Optional: JSON file with explicit runtime model config
export WAXCPP_SERVER_CONFIG=/abs/path/to/server-runtime.json

# Optional when enable_vector_search=true:
# llama.cpp embedding endpoint (llama-server style)
export WAXCPP_LLAMA_EMBED_ENDPOINT=http://127.0.0.1:8081/embedding
# expected embedding dimension from the configured embedding model
export WAXCPP_LLAMA_EMBED_DIMS=1024
# request timeout in milliseconds
export WAXCPP_LLAMA_EMBED_TIMEOUT_MS=30000
# retry count on transient failures (additional attempts)
export WAXCPP_LLAMA_EMBED_MAX_RETRIES=2
# fixed retry backoff in milliseconds
export WAXCPP_LLAMA_EMBED_RETRY_BACKOFF_MS=100
# max parallel workers for unique texts in EmbedBatch
export WAXCPP_LLAMA_EMBED_MAX_BATCH_CONCURRENCY=4

# Optional generation endpoint tuning
export WAXCPP_LLAMA_GEN_ENDPOINT=http://127.0.0.1:8081/completion
export WAXCPP_LLAMA_GEN_TIMEOUT_MS=60000
export WAXCPP_LLAMA_GEN_MAX_RETRIES=2
export WAXCPP_LLAMA_GEN_RETRY_BACKOFF_MS=100
```

Example `server-runtime.json`:
```json
{
  "llama_cpp_root": "g:/Proj/Agents1/llama-cpp",
  "retrieval": {
    "enable_vector_search": false,
    "require_distinct_models": true
  },
  "models": {
    "generation_model": {
      "runtime": "llama_cpp",
      "model_path": "g:/Proj/Agents1/Models/Qwen/Qwen3-Coder-Next-Q4_K_M.gguf"
    },
    "embedding_model": {
      "runtime": "disabled",
      "model_path": ""
    }
  }
}
```

Indexing JSON-RPC methods (baseline skeleton):
```json
{"jsonrpc":"2.0","id":1,"method":"index.start","params":{"repo_root":"g:/Proj/UnrealEngine/Engine/Source","resume":true}}
{"jsonrpc":"2.0","id":2,"method":"index.status","params":{}}
{"jsonrpc":"2.0","id":3,"method":"index.stop","params":{}}
{"jsonrpc":"2.0","id":4,"method":"answer.generate","params":{"query":"How does FName hashing work in UE5?","max_context_items":10,"max_context_tokens":4000,"max_output_tokens":768}}
```

`index.start` optional controls:
```json
{
  "jsonrpc":"2.0",
  "id":5,
  "method":"index.start",
  "params":{
    "repo_root":"g:/Proj/UnrealEngine/Engine/Source",
    "resume":true,
    "flush_every_chunks":128,
    "max_files":0,
    "max_chunks":0
  }
}
```

- `flush_every_chunks`: commit/checkpoint cadence during ingest (`1..1000000`).
- `max_files`: deterministic cap on scanned file count (`0..1000000`, `0` means no cap).
- `max_chunks`: deterministic cap on ingested chunks per run (`0..1000000`, `0` means no cap).

Optional server log:
```bash
export WAXCPP_SERVER_LOG=1
```

Current behavior:
- `index.start` is asynchronous (starts background worker), scans/chunks source files, ingests changed chunks into Wax, and persists checkpoint metadata.
- `index.status` returns persisted state snapshot (`idle|running|stopped|failed`).
- `index.status` includes `phase` (`starting|scanning|ingesting|persisting_manifests|completed|stopped|failed`) for live job progress introspection.
- `index.status` includes runtime metrics: `elapsed_ms`, `indexed_chunks_per_sec`, `committed_chunks_per_sec`.
- `index.stop` requests cancellation and waits worker shutdown (`running -> stopped`).
- `resume=true` uses `<checkpoint>.file_manifest` to skip unchanged files.
- `answer.generate` performs Recall + citation map assembly (`relative_path`, `line_start`, `line_end`) and calls llama.cpp generation endpoint.
- `answer.generate` supports deterministic context clamp via `max_context_items` and `max_context_tokens` and returns `context_items_used/context_tokens_used`.

SQLite backend (optional, currently disabled by default in favor of WAL-focused track):
```bash
cmake -S cpp -B cpp/build -DWAXCPP_ENABLE_SQLITE_BACKEND=ON
```

## Dependency Policy
All third-party dependencies are managed via git submodules under `cpp/third_party/`.
Do not replace this with package manager auto-fetch.

See:
- `.gitmodules`
- `cpp/submodules.lock`
- `cpp/scripts/verify_submodules.py`

Policy test in CTest matrix:
```bash
ctest --test-dir cpp/build --output-on-failure -R waxcpp_verify_submodules_policy_test
```

Strict CI-style dependency checks:
```bash
# Require checksum-verified submodule checkouts for local verification
python3 cpp/scripts/verify_submodules.py --require-checksum-submodules-present

# Require all required submodule gitlinks (mode 160000) to be present in index
python3 cpp/scripts/verify_submodules.py --require-gitlinks-present

# Fail on any unresolved <PIN_REQUIRED> entry
python3 cpp/scripts/verify_submodules.py --enforce-pin-required
```
