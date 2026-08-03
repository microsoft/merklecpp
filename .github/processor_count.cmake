include(ProcessorCount)

ProcessorCount(PARALLELISM)
if(PARALLELISM EQUAL 0)
  set(PARALLELISM 1)
endif()

message(STATUS "Using ${PARALLELISM} parallel jobs")
file(APPEND "$ENV{GITHUB_ENV}" "PARALLELISM=${PARALLELISM}\n")