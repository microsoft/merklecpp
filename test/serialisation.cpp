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
    const size_t max_num_leaves = 32 * 1024;
#else
    const size_t num_trees = 256;
    const size_t max_num_leaves = 128 * 1024;
#endif

    size_t total_leaves = 0, total_flushes = 0, total_retractions = 0;

    for (size_t k = 0; k < num_trees && !timed_out(timeout, test_start_time);
         k++)
    {
      std::map<size_t, merkle::Hash> past_roots;
      size_t num_leaves = (size_t)(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      total_leaves += num_leaves;
      auto hashes = make_hashes(num_leaves);

      // Build
      merkle::Tree mt;
      for (auto& h : hashes)
      {
        assert(mt.invariant());
        mt.insert(h);
        assert(mt.invariant());
        if ((std::rand() / (double)RAND_MAX) > 0.95)
        {
          assert(mt.invariant());
          mt.flush_to(random_index(mt));
          assert(mt.invariant());
          total_flushes++;
        }
        if ((std::rand() / (double)RAND_MAX) > 0.95)
        {
          assert(mt.invariant());
          mt.retract_to(random_index(mt));
          assert(mt.invariant());
          total_retractions++;
        }
      }

      // Serialise
      std::vector<uint8_t> buffer;
      mt.serialise(buffer);

      // Deserialise
      merkle::Tree mt2(buffer);

      // Check roots and other properties
      if (
        mt.root() != mt2.root() || mt.min_index() != mt2.min_index() ||
        mt.max_index() != mt2.max_index() ||
        mt.num_leaves() != mt2.num_leaves() ||
        mt.serialised_size() != mt2.serialised_size() ||
        mt.size() != mt2.size())
      {
        std::cout << "before:" << std::endl
                  << mt.to_string(PRINT_HASH_SIZE) << std::endl;
        std::cout << "after:" << std::endl
                  << mt2.to_string(PRINT_HASH_SIZE) << std::endl;
        throw std::runtime_error("tree properties mismatch");
      }

      if ((k && k % 1000 == 999) || k == num_trees - 1)
      {
        std::cout << k + 1 << " trees, " << total_leaves << " leaves, "
                  << total_flushes << " flushes, " << total_retractions
                  << " retractions"
                  << ": OK." << std::endl;
      }
    }
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << std::endl;
    std::cout << "(seed=" << seed << ")" << std::endl;
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << std::endl;
    std::cout << "(seed=" << seed << ")" << std::endl;
    return 1;
  }

  return 0;
}
