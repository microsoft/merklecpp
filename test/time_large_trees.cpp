// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <chrono>
#include <iostream>

#include "util.h"

#include <merklecpp.h>

int main()
{
  try
  {
#ifndef NDEBUG
    const size_t num_leaves = static_cast<size_t>(128) * 1024;
    const size_t root_interval = 128;
#else
    const size_t num_leaves = static_cast<size_t>(16) * 1024 * 1024;
    const size_t root_interval = 1024;
#endif

    auto hashes = make_hashes(num_leaves);

    merkle::Tree mt;
    size_t j = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (auto& h : hashes)
    {
      mt.insert(h);
      if ((j++ % root_interval) == 0)
      {
        mt.root();
      }
    }
    mt.root();
    auto stop = std::chrono::high_resolution_clock::now();
    const double seconds =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
        .count()) /
      1e9;
    std::cout << "SHA256: " << mt.statistics.to_string() << " in " << seconds
              << " sec" << '\n';

#ifdef HAVE_OPENSSL
    {
      auto hashes384 = make_hashesT<48>(num_leaves);

      merkle::Tree384 mt384;
      size_t j384 = 0;
      auto start384 = std::chrono::high_resolution_clock::now();
      for (auto& h : hashes384)
      {
        mt384.insert(h);
        if ((j384++ % root_interval) == 0)
        {
          mt384.root();
        }
      }
      mt384.root();
      auto stop384 = std::chrono::high_resolution_clock::now();
      const double seconds384 =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop384 - start384)
          .count()) /
        1e9;
      std::cout << "SHA384: " << mt384.statistics.to_string() << " in "
                << seconds384 << " sec" << '\n';
    }

    {
      auto hashes512 = make_hashesT<64>(num_leaves);

      merkle::Tree512 mt512;
      size_t j512 = 0;
      auto start512 = std::chrono::high_resolution_clock::now();
      for (auto& h : hashes512)
      {
        mt512.insert(h);
        if ((j512++ % root_interval) == 0)
        {
          mt512.root();
        }
      }
      mt512.root();
      auto stop512 = std::chrono::high_resolution_clock::now();
      const double seconds512 =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop512 - start512)
          .count()) /
        1e9;
      std::cout << "SHA512: " << mt512.statistics.to_string() << " in "
                << seconds512 << " sec" << '\n';
    }
#endif
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