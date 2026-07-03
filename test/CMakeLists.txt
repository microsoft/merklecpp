# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(add_merklecpp_test target source)
  add_unit_test(${MERKLECPP_TEST_PREFIX}${target} ${source})
  set_tests_properties(
    ${MERKLECPP_TEST_PREFIX}${target} PROPERTIES LABELS "merklecpp"
  )
  if (DOCTEST_DIR)
    target_include_directories(
      ${MERKLECPP_TEST_PREFIX}${target} PRIVATE ${DOCTEST_DIR}
    )
  endif()
  if (WIN32)
    target_compile_definitions(${MERKLECPP_TEST_PREFIX}${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endfunction()

add_merklecpp_test(demo_tree demo_tree.cpp)
add_merklecpp_test(time_large_trees time_large_trees.cpp)
add_merklecpp_test(paths paths.cpp)
add_merklecpp_test(flush flush.cpp)
add_merklecpp_test(retract retract.cpp)
add_merklecpp_test(past_root past_root.cpp)
add_merklecpp_test(past_paths past_paths.cpp)
add_merklecpp_test(serialisation serialisation.cpp)
add_merklecpp_test(partial_serialisation partial_serialisation.cpp)
add_merklecpp_test(serialise_to_file serialise_to_file.cpp)
add_merklecpp_test(coverage coverage.cpp)
add_merklecpp_test(tiles_store tiles_store.cpp)
add_merklecpp_test(tiles_writer tiles_writer.cpp)
add_merklecpp_test(tiles_proofs tiles_proofs.cpp)
add_merklecpp_test(tiles_tree tiles_tree.cpp)
add_merklecpp_test(tiles_entries tiles_entries.cpp)

if(LONG_TESTS)
  add_merklecpp_test(tiles_level2 tiles_level2.cpp)
  add_merklecpp_test(time_tiles time_tiles.cpp)
endif()

find_path(DOCTEST_DIR doctest.h)
if(NOT DOCTEST_DIR)
  set(DOCTEST_VERSION 2.4.11)
  set(DOCTEST_DIR "${CMAKE_BINARY_DIR}/_deps/doctest-${DOCTEST_VERSION}")
  set(DOCTEST_HEADER "${DOCTEST_DIR}/doctest.h")

  message(STATUS "doctest.h not found; downloading doctest v${DOCTEST_VERSION}")
  file(MAKE_DIRECTORY "${DOCTEST_DIR}")
  file(
    DOWNLOAD
    "https://raw.githubusercontent.com/doctest/doctest/v${DOCTEST_VERSION}/doctest/doctest.h"
    "${DOCTEST_HEADER}"
    EXPECTED_HASH SHA256=44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb
    STATUS DOCTEST_DOWNLOAD_STATUS
    TLS_VERIFY ON
  )

  list(GET DOCTEST_DOWNLOAD_STATUS 0 DOCTEST_DOWNLOAD_CODE)
  list(GET DOCTEST_DOWNLOAD_STATUS 1 DOCTEST_DOWNLOAD_MESSAGE)
  if(NOT DOCTEST_DOWNLOAD_CODE EQUAL 0)
    message(FATAL_ERROR "failed to download doctest.h: ${DOCTEST_DOWNLOAD_MESSAGE}")
  endif()
endif()
add_merklecpp_test(unit_tests unit_tests.cpp)

if(TARGET evercrypt.host)
  add_merklecpp_test(compare_evercrypt compare_evercrypt.cpp)
endif()

if(OPENSSL
   OR EVERCRYPT
)
  add_merklecpp_test(compare_hash_functions compare_hash_functions.cpp)
endif()
