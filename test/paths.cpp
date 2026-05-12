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
    const size_t num_trees = 64;
    const size_t max_num_paths = 32;
    const size_t max_num_leaves = static_cast<size_t>(64) * 1024;
#else
    const size_t num_trees = 256;
    const size_t max_num_paths = 128;
    const size_t max_num_leaves = static_cast<size_t>(64) * 1024;
#endif

    size_t total_paths = 0;
    size_t total_leaves = 0;

    for (size_t l = 0; l < num_trees && !timed_out(timeout, test_start_time);
         l++)
    {
      const auto num_leaves = static_cast<size_t>(1 + (std::rand() / (double)RAND_MAX) * max_num_leaves);
      const auto num_paths = static_cast<size_t>(1 + (std::rand() / (double)RAND_MAX) * max_num_paths);

      total_leaves += num_leaves;
      total_paths += num_paths;

      auto hashes = make_hashes(num_leaves);

      merkle::Tree mt;
      for (auto h : hashes)
      {
        mt.insert(h);
      }
      const merkle::Tree::Hash root = mt.root();

      for (size_t p = 0; p < num_paths && !timed_out(timeout, test_start_time);
           p++)
      {
        const auto i = static_cast<size_t>((std::rand() / static_cast<double>(RAND_MAX)) * static_cast<double>(num_leaves - 1));
        auto path = mt.path(i);
        if (!path->verify(root))
        {
          throw std::runtime_error("path verification failed");
        }
        std::vector<uint8_t> serialised_path;
        path->serialise(serialised_path);
        if (path->serialised_size() != serialised_path.size())
        {
          throw std::runtime_error("serialised_size() != serialised_path.size()");
        }
      }
    }

    std::cout << num_trees << " trees, " << total_leaves << " leaves, "
              << total_paths << " paths: OK." << '\n';
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
