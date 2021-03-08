// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <merklecpp.h>

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
    const size_t num_trees = 32;
    const size_t max_num_leaves = 64 * 1024;
    const size_t max_flushes = 16;
#else
    const size_t num_trees = 128;
    const size_t max_num_leaves = 128 * 1024;
    const size_t max_flushes = 64;
#endif

    size_t total_leaves = 0, total_flushes = 0;

    for (size_t k = 0; k < num_trees && !timed_out(timeout, test_start_time);
         k++)
    {
      size_t num_leaves =
        (size_t)(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      total_leaves += num_leaves;

      auto hashes = make_hashes(num_leaves);

      merkle::Tree mt;
      for (size_t i = 0; i < hashes.size(); i++)
      {
        mt.insert(hashes[i]);
        if ((std::rand() / (double)RAND_MAX) > 0.95)
        {
          mt.flush_to(random_index(mt));
          total_flushes++;
        }
      }

      for (size_t i = 0; i < max_flushes; i++)
      {
        mt.flush_to(random_index(mt));
        total_flushes++;
        if (mt.min_index() == mt.max_index())
          break;
      }

      if ((k && k % 1000 == 0) || k == num_trees - 1)
      {
        std::cout << k << " trees, " << total_leaves << " leaves, "
                  << total_flushes << " flushes"
                  << ": OK." << std::endl;
      }
    }
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
