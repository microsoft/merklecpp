// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>

#include <merklecpp.h>

#include "util.h"

#define PSZ 3

std::shared_ptr<merkle::Path> past_root_spec(
  const merkle::Tree& mt, size_t index, size_t as_of)
{
  merkle::Tree tmt = mt;
  tmt.retract_to(as_of);
  tmt.root();
#ifdef MERKLECPP_TRACE_ENABLED
  MERKLECPP_TOUT << "Retracted tree:" << std::endl;
  MERKLECPP_TOUT << tmt.to_string(PSZ) << std::endl;
#endif
  auto root = tmt.root();
  auto result = tmt.path(index);
  result->verify(root);
  return result;
}

int main(int argc, char** argv)
{
  auto test_start_time = std::chrono::high_resolution_clock::now();
  double timeout = get_timeout();
  auto seed = std::time(0);
  std::cout << "seed=" << seed << " timeout=" << timeout << std::endl;

  std::srand((unsigned)seed);

  try
  {
#ifndef NDEBUG
    const size_t num_trees = 24;
    const size_t max_num_paths = 128;
    const size_t max_num_leaves = 16 * 1024;
#else
    const size_t num_trees = 128;
    const size_t max_num_paths = 1024;
    const size_t max_num_leaves = 64 * 1024;
#endif

    size_t total_paths = 0, total_leaves = 0, total_flushed_nodes = 0;

    auto hashes = make_hashes(max_num_leaves);

    bool is_timed_out = false;
    for (size_t l = 0; l < num_trees && !is_timed_out; l++)
    {
      size_t num_leaves = (size_t)(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      size_t num_paths = (size_t)(1 + (std::rand() / (double)RAND_MAX) * max_num_paths);

      total_leaves += num_leaves;
      total_paths += num_paths;

      merkle::Tree mt;
      for (size_t i = 0; i < num_leaves; i++)
        mt.insert(hashes[i]);

#ifdef MERKLECPP_TRACE_ENABLED
      MERKLECPP_TOUT << "Tree: " << std::endl;
      MERKLECPP_TOUT << mt.to_string(PSZ) << std::endl;
#endif

      for (size_t m = 0;
           m < num_paths && mt.min_index() != mt.max_index() && !is_timed_out;
           m++)
      {
        size_t index = random_index(mt);
        size_t as_of = random_index(mt);
        if (index > as_of)
          std::swap(index, as_of);

        auto past_path_spec = past_root_spec(mt, index, as_of);
        auto past_root_spec = past_path_spec->root();

        if ((std::rand() / (double)RAND_MAX) > 0.95)
          if (index / 10 > mt.min_index())
          {
            size_t before = mt.min_index();
            mt.flush_to(index / 2);
            total_flushed_nodes += mt.min_index() - before;
          }

#ifdef MERKLECPP_TRACE_ENABLED
        MERKLECPP_TOUT << std::endl;
        MERKLECPP_TOUT << "past_root_spec: " << std::endl;
        MERKLECPP_TOUT << "Past path (spec) from " << index << " as_of "
                       << as_of << ": " << past_path_spec->to_string(PSZ)
                       << std::endl;
        MERKLECPP_TOUT << "Computed root: " << past_root_spec->to_string(PSZ)
                       << std::endl;
        MERKLECPP_TOUT << std::endl;

        MERKLECPP_TOUT << "Flushed tree: " << std::endl;
        MERKLECPP_TOUT << mt.to_string(PSZ) << std::endl;
#endif

        auto past_root = mt.past_root(as_of);
        auto past_path = mt.past_path(index, as_of);
        auto pp_root = past_path->root();

#ifdef MERKLECPP_TRACE_ENABLED
        MERKLECPP_TOUT << "Past root: " << past_root->to_string(PSZ)
                       << std::endl;
        MERKLECPP_TOUT << "Past path: " << past_path->to_string(PSZ)
                       << std::endl;
        MERKLECPP_TOUT << "Computed root: " << pp_root->to_string(PSZ)
                       << std::endl;
#endif

        if (*past_path_spec != *past_path)
          throw std::runtime_error("path mismatch");

        if (*pp_root != *past_root)
          throw std::runtime_error("path root mismatch");

        if (!past_path->verify(*past_root))
          throw std::runtime_error("path verification failed");

        if (m == num_paths - 1)
          std::cout << (l + 1) << " trees, " << total_leaves << " leaves, "
                    << total_flushed_nodes << " flushed, " << total_paths
                    << " past paths: OK." << std::endl;

        is_timed_out = timed_out(timeout, test_start_time);
      }
    }
  }
  catch (std::exception& ex)
  {
    std::cout << "Seed: " << seed << std::endl;
    std::cout << "Error: " << ex.what() << std::endl;
    return 1;
  }
  catch (...)
  {
    std::cout << "Seed: " << seed << std::endl;
    std::cout << "Error" << std::endl;
    return 1;
  }

  return 0;
}
