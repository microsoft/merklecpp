// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <stdexcept>

#include <merklecpp.h>

#include "util.h"

constexpr size_t PRINT_HASH_SIZE = 3;

int main()
{
  auto test_start_time = std::chrono::high_resolution_clock::now();
  const double timeout = get_timeout();
  auto seed = std::time(nullptr);
  std::cout << "seed=" << seed << " timeout=" << timeout << '\n';

  std::srand((unsigned)seed);

  try
  {
#ifndef NDEBUG
    const size_t num_trees = 64;
    const size_t max_num_leaves = 8 * 1024;
#else
    const size_t num_trees = 128;
    const size_t max_num_leaves = static_cast<size_t>(32) * 1024;
#endif

    size_t total_leaves = 0;
    size_t total_roots = 0;

    for (size_t k = 0; k < num_trees && !timed_out(timeout, test_start_time);
         k++)
    {
      std::map<size_t, merkle::Hash> past_roots;
      const auto num_leaves = static_cast<size_t>(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      total_leaves += num_leaves;
      auto hashes = make_hashes(num_leaves);

      {
        // Extract some normal roots along the way
        merkle::Tree mt;
        for (size_t i = 0; i < hashes.size(); i++)
        {
          mt.insert(hashes[i]);
          if ((std::rand() / (double)RAND_MAX) > 0.95)
          {
            past_roots[i] = mt.root();
          }
        }
      }

      // Build new tree without taking roots
      merkle::Tree mt;
      for (auto& h : hashes)
      {
        mt.insert(h);
      }

      // Extract and check past roots
      for (auto& kv : past_roots)
      {
        auto pr = mt.past_root(kv.first);
        total_roots++;
        if (*pr != kv.second)
        {
          std::cout << pr->to_string(PRINT_HASH_SIZE)
                    << " != " << kv.second.to_string(PRINT_HASH_SIZE)
                    << '\n';
          throw std::runtime_error("past root hash mismatch");
        }
      }

      if ((k != 0 && k % 1000 == 999) || k == num_trees - 1)
      {
        std::cout << k + 1 << " trees, " << total_leaves << " leaves, "
                  << total_roots << " roots"
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
