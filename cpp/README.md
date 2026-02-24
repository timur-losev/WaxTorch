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
# Optional: enable real libtorch runtime backend at build time
cmake -S cpp -B cpp/build -DWAXCPP_ENABLE_LIBTORCH_RUNTIME=ON

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
