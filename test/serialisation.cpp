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

  try
  {
#ifndef NDEBUG
    const size_t num_trees = 32;
    const size_t max_num_leaves = 32 * 1024;
#else
    const size_t num_trees = 256;
    const size_t max_num_leaves = static_cast<size_t>(128) * 1024;
#endif

    size_t total_leaves = 0;
    size_t total_flushes = 0;
    size_t total_retractions = 0;

    for (size_t k = 0; k < num_trees && !timed_out(timeout, test_start_time);
         k++)
    {
      std::map<size_t, merkle::Hash> past_roots;
      const auto num_leaves = static_cast<size_t>(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
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
        std::cout << "before:" << '\n'
                  << mt.to_string(PRINT_HASH_SIZE) << '\n';
        std::cout << "after:" << '\n'
                  << mt2.to_string(PRINT_HASH_SIZE) << '\n';
        throw std::runtime_error("tree properties mismatch");
      }

      if ((k != 0 && k % 1000 == 999) || k == num_trees - 1)
      {
        std::cout << k + 1 << " trees, " << total_leaves << " leaves, "
                  << total_flushes << " flushes, " << total_retractions
                  << " retractions"
                  << ": OK." << '\n';
      }
    }
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << '\n';
    std::cout << "(seed=" << seed << ")" << '\n';
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << '\n';
    std::cout << "(seed=" << seed << ")" << '\n';
    return 1;
  }

  return 0;
}
