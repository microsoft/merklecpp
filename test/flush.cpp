// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <merklecpp.h>

constexpr size_t PRINT_HASH_SIZE = 3;

int main()
{
  auto test_start_time = std::chrono::high_resolution_clock::now();
  const double timeout = get_timeout();
  auto seed = std::time(nullptr);
  std::cout << "seed=" << seed << " timeout=" << timeout << '\n';

  try
  {
#ifndef NDEBUG
    const size_t num_trees = 32;
    const size_t max_num_leaves = 64 * 1024;
    const size_t max_flushes = 16;
#else
    const size_t num_trees = 128;
    const size_t max_num_leaves = static_cast<size_t>(128) * 1024;
    const size_t max_flushes = 64;
#endif

    size_t total_leaves = 0;
    size_t total_flushes = 0;

    for (size_t k = 0; k < num_trees && !timed_out(timeout, test_start_time);
         k++)
    {
      const auto num_leaves =
        static_cast<size_t>(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      total_leaves += num_leaves;

      auto hashes = make_hashes(num_leaves);

      merkle::Tree mt;
      for (auto& hash : hashes)
      {
        mt.insert(hash);
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
        {
          break;
        }
      }

      if ((k != 0 && k % 1000 == 0) || k == num_trees - 1)
      {
        std::cout << k << " trees, " << total_leaves << " leaves, "
                  << total_flushes << " flushes"
                  << ": OK." << '\n';
      }
    }
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << '\n';
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << '\n';
    return 1;
  }

  return 0;
}
