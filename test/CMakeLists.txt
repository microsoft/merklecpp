# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG 2d0a9359a60c51affe2a9bebb1be1dca47868151 # v2.5.3
)
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

if(TARGET evercrypt.host)
  add_merklecpp_test(compare_evercrypt compare_evercrypt.cpp)
endif()

if(OPENSSL
   OR EVERCRYPT
)
  add_merklecpp_test(compare_hash_functions compare_hash_functions.cpp)
endif()

add_merklecpp_test(tiles_store tiles_store.cpp)
add_merklecpp_test(unit_tests unit_tests.cpp)
