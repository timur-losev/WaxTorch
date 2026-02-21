cmake_minimum_required(VERSION 3.22)
project(waxcpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WAXCPP_BUILD_TESTS "Build waxcpp tests" ON)
option(WAXCPP_REQUIRE_PARITY_FIXTURES "Fail parity test if no .mv2s fixtures are found" OFF)
option(WAXCPP_REQUIRE_SWIFT_FIXTURES "Fail parity test if no non-synthetic fixtures are found" OFF)

set(WAXCPP_PUBLIC_HEADERS
  include/waxcpp/embeddings.hpp
  include/waxcpp/fts5_search_engine.hpp
  include/waxcpp/memory_orchestrator.hpp
  include/waxcpp/pch.hpp
  include/waxcpp/search.hpp
  include/waxcpp/types.hpp
  include/waxcpp/vector_engine.hpp
  include/waxcpp/wax_store.hpp
)

add_library(waxcpp STATIC
  src/core/mv2s_format.cpp
  src/core/sha256.cpp
  src/core/wal_ring.cpp
  src/core/wax_store.cpp
  src/text/fts5_search_engine.cpp
  src/vector/usearch_vector_engine.cpp
  src/rag/search.cpp
  src/rag/embeddings.cpp
  src/orchestrator/memory_orchestrator.cpp
)

target_sources(waxcpp PRIVATE ${WAXCPP_PUBLIC_HEADERS})
source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}/include
  PREFIX "Header Files"
  FILES ${WAXCPP_PUBLIC_HEADERS}
)

target_include_directories(waxcpp
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(waxcpp PUBLIC cxx_std_20)
target_precompile_headers(waxcpp
  PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/waxcpp/pch.hpp>"
)

if (MSVC)
  target_compile_options(waxcpp PRIVATE /W4 /permissive-)
else()
  target_compile_options(waxcpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (WAXCPP_BUILD_TESTS)
  enable_testing()
  add_executable(waxcpp_smoke_test tests/unit/smoke_test.cpp)
  target_link_libraries(waxcpp_smoke_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_smoke_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_smoke_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_smoke_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_smoke_test COMMAND waxcpp_smoke_test)

  add_executable(waxcpp_wax_store_verify_test tests/unit/wax_store_verify_test.cpp)
  target_link_libraries(waxcpp_wax_store_verify_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wax_store_verify_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wax_store_verify_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wax_store_verify_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_wax_store_verify_test COMMAND waxcpp_wax_store_verify_test)

  add_executable(waxcpp_wax_store_write_test tests/unit/wax_store_write_test.cpp)
  target_link_libraries(waxcpp_wax_store_write_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wax_store_write_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wax_store_write_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wax_store_write_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_wax_store_write_test COMMAND waxcpp_wax_store_write_test)

  add_executable(waxcpp_mv2s_format_test tests/unit/mv2s_format_test.cpp)
  target_link_libraries(waxcpp_mv2s_format_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_mv2s_format_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2s_format_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2s_format_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_mv2s_format_test COMMAND waxcpp_mv2s_format_test)

  add_executable(waxcpp_fts5_search_engine_test tests/unit/fts5_search_engine_test.cpp)
  target_link_libraries(waxcpp_fts5_search_engine_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_fts5_search_engine_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_fts5_search_engine_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_fts5_search_engine_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_fts5_search_engine_test COMMAND waxcpp_fts5_search_engine_test)

  add_executable(waxcpp_wal_ring_test tests/unit/wal_ring_test.cpp)
  target_link_libraries(waxcpp_wal_ring_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wal_ring_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wal_ring_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wal_ring_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_wal_ring_test COMMAND waxcpp_wal_ring_test)

  add_executable(waxcpp_wal_ring_writer_test tests/unit/wal_ring_writer_test.cpp)
  target_link_libraries(waxcpp_wal_ring_writer_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wal_ring_writer_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wal_ring_writer_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wal_ring_writer_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_wal_ring_writer_test COMMAND waxcpp_wal_ring_writer_test)

  add_executable(waxcpp_mv2s_parity_test tests/parity/mv2s_fixture_parity_test.cpp)
  target_link_libraries(waxcpp_mv2s_parity_test PRIVATE waxcpp)
  target_compile_definitions(waxcpp_mv2s_parity_test PRIVATE
    WAXCPP_PARITY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/parity"
    WAXCPP_REQUIRE_PARITY_FIXTURES=$<IF:$<BOOL:${WAXCPP_REQUIRE_PARITY_FIXTURES}>,1,0>
    WAXCPP_REQUIRE_SWIFT_FIXTURES=$<IF:$<BOOL:${WAXCPP_REQUIRE_SWIFT_FIXTURES}>,1,0>
  )
  if (MSVC)
    target_compile_options(waxcpp_mv2s_parity_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2s_parity_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2s_parity_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_mv2s_parity_test COMMAND waxcpp_mv2s_parity_test)

  add_executable(waxcpp_mv2s_fixture_generator tests/parity/mv2s_fixture_generator.cpp)
  target_link_libraries(waxcpp_mv2s_fixture_generator PRIVATE waxcpp)
  target_compile_definitions(waxcpp_mv2s_fixture_generator PRIVATE
    WAXCPP_PARITY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/parity/synthetic"
  )
  if (MSVC)
    target_compile_options(waxcpp_mv2s_fixture_generator PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2s_fixture_generator PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2s_fixture_generator REUSE_FROM waxcpp)
endif()
