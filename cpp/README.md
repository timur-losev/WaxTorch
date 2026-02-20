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

## Dependency Policy
All third-party dependencies are managed via git submodules under `cpp/third_party/`.
Do not replace this with package manager auto-fetch.

See:
- `.gitmodules`
- `cpp/submodules.lock`
- `cpp/scripts/verify_submodules.py`
