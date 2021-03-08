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
    const size_t num_trees = 64;
    const size_t max_num_leaves = 8 * 1024;
    const size_t max_num_subtrees = 32;
#else
    const size_t num_trees = 256;
    const size_t max_num_leaves = 128 * 1024;
    const size_t max_num_subtrees = 256;
#endif

    size_t total_leaves = 0, total_flushes = 0, total_retractions = 0,
           total_subtrees = 0;

    for (size_t k = 0; k < num_trees && !timed_out(timeout, test_start_time);
         k++)
    {
      std::map<size_t, merkle::Hash> past_roots;
      size_t num_leaves = (size_t)(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      size_t num_subtrees = (size_t)(
        1 + (std::rand() / (double)RAND_MAX) * max_num_subtrees);
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

      for (size_t l = 0;
           l < num_subtrees && !timed_out(timeout, test_start_time);
           l++)
      {
        size_t from = random_index(mt);
        size_t to = random_index(mt);
        if (from > to)
          std::swap(from, to);

        // Serialise
        std::vector<uint8_t> buffer;
        mt.serialise(from, to, buffer);

        // Deserialise
        merkle::Tree mt2(buffer);

        // Check roots, paths, and other properties
        auto past_root = *mt.past_root(to);
        auto root_ok = mt2.root() == past_root;
        auto old_path = mt.past_path(from, to);
        auto path = mt2.path(from);
        auto path_ok = path->verify(past_root);
        auto paths_equal = *old_path == *path;
        auto size_ok = mt2.serialised_size() == mt.serialised_size(from, to);

        if (
          mt2.num_leaves() != to + 1 || mt2.min_index() != from || !root_ok ||
          !path_ok || !paths_equal || !size_ok)
        {
          std::cout << "before:" << std::endl
                    << mt.to_string(PRINT_HASH_SIZE) << std::endl;
          std::cout << "after:" << std::endl
                    << mt2.to_string(PRINT_HASH_SIZE) << std::endl;
          throw std::runtime_error("tree properties mismatch");
        }

        total_subtrees++;

        if (
          (total_subtrees % 2500 == 0) ||
          (k == num_trees - 1 && l == num_subtrees - 1))
        {
          std::cout << k + 1 << " trees, " << total_leaves << " leaves, "
                    << total_flushes << " flushes, " << total_retractions
                    << " retractions, " << total_subtrees << " subtrees"
                    << ": OK." << std::endl;
        }
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
