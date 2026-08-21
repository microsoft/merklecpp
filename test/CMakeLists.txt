# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG 2d0a9359a60c51affe2a9bebb1be1dca47868151 # v2.5.3
)
set(DOCTEST_NO_INSTALL ON)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
FetchContent_MakeAvailable(doctest)

function(add_merklecpp_test target source)
  add_unit_test(${MERKLECPP_TEST_PREFIX}${target} ${source})
  target_link_libraries(
    ${MERKLECPP_TEST_PREFIX}${target} PRIVATE doctest::doctest
  )
  set_tests_properties(
    ${MERKLECPP_TEST_PREFIX}${target} PROPERTIES LABELS "merklecpp"
  )
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
add_merklecpp_test(tiles_resume tiles_resume.cpp)
add_merklecpp_test(tiles_docs tiles_docs.cpp)
add_merklecpp_test(tiles_entries tiles_entries.cpp)
add_merklecpp_test(tiles_geometry tiles_geometry.cpp)

if(OPENSSL)
  add_merklecpp_test(tiles_hashes tiles_hashes.cpp)
endif()

if(LONG_TESTS)
  add_merklecpp_test(tiles_level2 tiles_level2.cpp)
  add_merklecpp_test(time_tiles time_tiles.cpp)
  set(TILES_LEVEL2_TIMEOUT 900)
  if(WIN32)
    set(TILES_LEVEL2_TIMEOUT 3600)
  endif()
  set_tests_properties(
    ${MERKLECPP_TEST_PREFIX}tiles_level2
    ${MERKLECPP_TEST_PREFIX}time_tiles
    PROPERTIES TIMEOUT ${TILES_LEVEL2_TIMEOUT}
  )
endif()

if(OPENSSL)
  add_merklecpp_test(compare_hash_functions compare_hash_functions.cpp)
endif()

add_merklecpp_test(unit_tests unit_tests.cpp)
