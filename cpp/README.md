# Wax C++ RAG Server

> **Work in progress** — the project is under active development. APIs, storage format, and configuration may change.

Local RAG server for indexing and searching Unreal Engine 5 projects. Indexes both **C++ source code** and **Blueprint visual scripts**, extracts structured facts via LLM enrichment, and provides hybrid search (BM25 full-text + optional vector) through a JSON-RPC HTTP API. All models run locally via llama.cpp — no cloud API calls required.

## What it can index

| Source type | Extensions | Enrichment |
|-------------|------------|------------|
| UE5 C++ source | `.h`, `.hpp`, `.cpp`, `.inl`, `.inc` | Regex (class hierarchies, UCLASS/UPROPERTY macros) + optional LLM |
| UE5 Blueprints | `.bpl_json` (exported via `BlueprintGraphExport` commandlet) | LLM only (entity-attribute-value facts: calls, variables, events, inheritance) |

## Models

| Role | Model | Details |
|------|-------|---------|
| Fact enrichment (indexing) | **Qwen2.5-Coder-32B-Instruct** (Q4_K_M GGUF) | Runs via llama.cpp on a separate port; extracts structured facts from code/BP chunks |
| Answer generation (query-time) | **Qwen3-Coder** (Q4_K_M GGUF) | Used by `answer.generate` for RAG-augmented code answers |
| Embeddings (optional) | Any GGUF embedding model via llama.cpp | Powers the vector search channel; disabled by default |

## Architecture

```
  Claude / IDE ───> MCP bridge (Node.js, stdio)
                        │
                    JSON-RPC / HTTP :8080
                        │
                  waxcpp_rag_server
                   ┌────┴────┐
              BM25/FTS5   Structured facts
            (SQLite)    (in-memory, persisted)
                   └────┬────┘
                    .mv2s store
              (append-only binary)
```

## Build

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --config Release
ctest --test-dir cpp/build --output-on-failure -C Release
```

## Server runtime config (llama.cpp + GGUF)
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

# Optional orchestrator ingest tuning
export WAXCPP_ORCH_INGEST_CONCURRENCY=1
export WAXCPP_ORCH_INGEST_BATCH_SIZE=32
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
    "ingest_batch_size":1,
    "max_files":0,
    "max_chunks":0,
    "max_ram_mb":0
  }
}
```

- `flush_every_chunks`: commit/checkpoint cadence during ingest (`1..1000000`).
- `ingest_batch_size`: number of chunks buffered before batched `remember` apply (`1..1000000`).
- `max_files`: deterministic cap on scanned file count (`0..1000000`, `0` means no cap).
- `max_chunks`: deterministic cap on ingested chunks per run (`0..1000000`, `0` means no cap).
- `max_ram_mb`: soft process RSS cap for index run (`0..1000000`, `0` means disabled).

Optional server log:
```bash
export WAXCPP_SERVER_LOG=1
```

Current behavior:
- `index.start` is asynchronous (starts background worker), scans/chunks source files, ingests changed chunks into Wax, and persists checkpoint metadata.
- `index.status` returns persisted state snapshot (`idle|running|stopped|failed`).
- `index.status` includes `phase` (`starting|scanning|ingesting|persisting_manifests|completed|stopped|failed`) for live job progress introspection.
- `index.status` includes runtime metrics: `elapsed_ms`, `indexed_chunks_per_sec`, `committed_chunks_per_sec`, `process_rss_mb`.
- `index.stop` requests cancellation and waits worker shutdown (`running -> stopped`).
- `resume=true` uses `<checkpoint>.file_manifest` to skip unchanged files.
- If `<checkpoint>.file_manifest` is absent (interrupted run before manifest persist), `resume=true` falls back to
  checkpoint `committed_chunks` watermark and skips already committed deterministic chunk prefix.
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
