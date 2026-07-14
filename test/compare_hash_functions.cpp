// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <merklecpp.h>

#ifdef HAVE_OPENSSL
using OpenSSLFullTree = merkle::TreeT<32, merkle::sha256_openssl>;
#endif

template <typename T>
void bench(
  const std::vector<merkle::Hash>& hashes,
  const std::string& name,
  size_t root_interval)
{
  size_t j = 0;
  auto start = std::chrono::high_resolution_clock::now();
  T mt;
  for (const auto& h : hashes)
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
    static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()) /
    1e9;
  std::cout << std::left << std::setw(10) << name << ": "
            << mt.statistics.num_insert << " insertions, "
            << mt.statistics.num_root << " roots in " << seconds << " sec"
            << '\n';
}

template <typename T, size_t HASH_SIZE>
void benchT(
  const std::vector<merkle::HashT<HASH_SIZE>>& hashes,
  const std::string& name,
  size_t root_interval)
{
  size_t j = 0;
  auto start = std::chrono::high_resolution_clock::now();
  T mt;
  for (const auto& h : hashes)
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
    static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()) /
    1e9;
  std::cout << std::left << std::setw(10) << name << ": "
            << mt.statistics.num_insert << " insertions, "
            << mt.statistics.num_root << " roots in " << seconds << " sec"
            << '\n';
}

int main()
{
  try
  {
    // std::srand(0);
    std::srand(std::time(nullptr));

#ifndef NDEBUG
    const size_t num_leaves = static_cast<size_t>(128) * 1024;
    const size_t root_interval = 128;
#else
    const size_t num_leaves = static_cast<size_t>(16) * 1024 * 1024;
    const size_t root_interval = 1024;
#endif

    auto hashes = make_hashes(num_leaves);

    std::cout << "--- merklecpp trees with SHA256 compression function: "
              << '\n';

    bench<merkle::Tree>(hashes, "merklecpp", root_interval);

    std::cout << "--- merklecpp trees with full SHA256: " << '\n';

#ifdef HAVE_OPENSSL
    bench<OpenSSLFullTree>(hashes, "OpenSSL", root_interval);
#endif

#ifdef HAVE_OPENSSL
    {
      std::cout << "--- merklecpp trees with full SHA384: " << '\n';
      auto hashes384 = make_hashesT<48>(num_leaves);
      benchT<merkle::Tree384, 48>(hashes384, "OpenSSL", root_interval);
    }

    {
      std::cout << "--- merklecpp trees with full SHA512: " << '\n';
      auto hashes512 = make_hashesT<64>(num_leaves);
      benchT<merkle::Tree512, 64>(hashes512, "OpenSSL", root_interval);
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