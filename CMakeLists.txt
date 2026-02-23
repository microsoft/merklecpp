# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

project(merklecpp LANGUAGES CXX C ASM)

cmake_minimum_required(VERSION 3.11)
set(CMAKE_CXX_STANDARD 17)

set(MERKLECPP_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(PROFILE "enable profiling" OFF)
option(TESTS "enable testing" OFF)
option(EVERCRYPT "enable comparison with EverCrypt Merkle trees" OFF)
option(OPENSSL "enable OpenSSL" OFF)
option(TRACE "enable debug traces" OFF)
option(CLANG_TIDY "enable clang-tidy checks during build" OFF)

if(CLANG_TIDY)
  find_program(CLANG_TIDY_PROGRAM clang-tidy)
  if(CLANG_TIDY_PROGRAM)
    set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_PROGRAM})
    set(CMAKE_C_CLANG_TIDY ${CLANG_TIDY_PROGRAM})
  else()
    message(FATAL_ERROR "clang-tidy not found")
  endif()
endif()

add_library(merklecpp INTERFACE)
target_include_directories(merklecpp INTERFACE .)

if(TRACE)
  target_compile_definitions(merklecpp INTERFACE MERKLECPP_TRACE_ENABLED)
endif()

install(TARGETS merklecpp)

set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".so")

if(EVERCRYPT)
  if(NOT EVERCRYPT_DIR)
    message(FATAL_ERROR "EverCrypt not found, add -DEVERCRYPT_DIR=...")
  endif()

  file(GLOB EVERCRYPT_SRC "${EVERCRYPT_DIR}/*.c"
       "${EVERCRYPT_DIR}/*-x86_64-linux.S"
  )
  add_library(evercrypt STATIC ${EVERCRYPT_SRC})
  target_include_directories(
    evercrypt PUBLIC ${EVERCRYPT_DIR} ${EVERCRYPT_DIR}/kremlin
                     ${EVERCRYPT_DIR}/kremlin/kremlib
  )
  set_target_properties(evercrypt PROPERTIES LINKER_LANGUAGE C)

  target_compile_definitions(merklecpp INTERFACE HAVE_EVERCRYPT)
  target_link_libraries(merklecpp INTERFACE evercrypt)
endif()

if(OPENSSL)
  find_package(OpenSSL)
  target_compile_definitions(merklecpp INTERFACE HAVE_OPENSSL)
  target_link_libraries(merklecpp INTERFACE crypto)
endif()

if(TESTS)
  enable_testing()

  function(add_unit_test NAME SRC)
    add_executable(${NAME} ${SRC})
    target_link_libraries(${NAME} PRIVATE $<BUILD_INTERFACE:merklecpp>)

    if(PROFILE)
      target_compile_options(${NAME} PRIVATE -g -pg)
      target_link_options(${NAME} PRIVATE -g -pg)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      target_compile_options(
        ${NAME} PRIVATE -fsanitize=undefined,address -fno-omit-frame-pointer
      )
      target_link_options(${NAME} PRIVATE -fsanitize=undefined,address)
    endif()

    add_test(${NAME} ${NAME})
  endfunction()

  add_subdirectory(test)
endif()
