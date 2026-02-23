// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>

#include <merklecpp.h>

#include "util.h"

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
    const size_t num_trees = 128;
    const size_t max_num_leaves = 64 * 1024;
    const size_t max_retractions = 16;
#else
    const size_t num_trees = 256;
    const size_t max_num_leaves = static_cast<size_t>(256) * 1024;
    const size_t max_retractions = 64;
#endif

    size_t total_leaves = 0;
    size_t total_retractions = 0;

    for (size_t k = 0; k < num_trees && !timed_out(timeout, test_start_time);
         k++)
    {
      const auto num_leaves = static_cast<size_t>(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      total_leaves += num_leaves;

      auto hashes = make_hashes(num_leaves);

      merkle::Tree mt;
      for (size_t i = 0; i < hashes.size(); i++)
      {
        mt.insert(hashes[i]);
        if (i > 0 && std::rand() / (double)RAND_MAX > 0.5)
        {
          mt.retract_to(mt.max_index() - 1);
          total_retractions++;
          mt.insert(hashes[i]);
        }

        if ((std::rand() / (double)RAND_MAX) > 0.95)
        {
          mt.retract_to(random_index(mt));
          total_retractions++;
        }
      }

      for (size_t i = 0; i < max_retractions; i++)
      {
        mt.retract_to(random_index(mt));
        total_retractions++;
        if (mt.min_index() == mt.max_index())
        {
          break;
        }
      }

      if ((k != 0 && k % 1000 == 0) || k == num_trees - 1)
      {
        std::cout << k << " trees, " << total_leaves << " leaves, "
                  << total_retractions << " retractions"
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
