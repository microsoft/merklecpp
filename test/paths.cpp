// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>

#include <merklecpp.h>

#include "util.h"

#define PRINT_HASH_SIZE 3

int main()
{
  auto test_start_time = std::chrono::high_resolution_clock::now();
  double timeout = get_timeout();
  auto seed = std::time(0);
  std::cout << "seed=" << seed << " timeout=" << timeout << std::endl;

  try
  {
#ifndef NDEBUG
    const size_t num_trees = 64;
    const size_t max_num_paths = 32;
    const size_t max_num_leaves = 64 * 1024;
#else
    const size_t num_trees = 256;
    const size_t max_num_paths = 128;
    const size_t max_num_leaves = 64 * 1024;
#endif

    size_t total_paths = 0, total_leaves = 0;

    for (size_t l = 0; l < num_trees && !timed_out(timeout, test_start_time);
         l++)
    {
      size_t num_leaves = (size_t)(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      size_t num_paths = (size_t)(1 + (std::rand() / (double)RAND_MAX) * max_num_paths);

      total_leaves += num_leaves;
      total_paths += num_paths;

      auto hashes = make_hashes(num_leaves);

      merkle::Tree mt;
      for (auto h : hashes)
        mt.insert(h);
      merkle::Tree::Hash root = mt.root();

      for (size_t p = 0; p < num_paths && !timed_out(timeout, test_start_time);
           p++)
      {
        size_t i = (size_t)((std::rand() / (double)(RAND_MAX)) * (num_leaves - 1));
        auto path = mt.path(i);
        if (!path->verify(root))
          throw std::runtime_error("path verification failed");
      }
    }

    std::cout << num_trees << " trees, " << total_leaves << " leaves, "
              << total_paths << " paths: OK." << std::endl;
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << std::endl;
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << std::endl;
    return 1;
  }

  return 0;
}
