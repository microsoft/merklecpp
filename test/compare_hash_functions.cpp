// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <merklecpp.h>

constexpr size_t PRNTSZ = 3;

#ifdef HAVE_OPENSSL
using OpenSSLTree = merkle::TreeT<32, merkle::sha256_openssl>;
#endif

template <
  void (*HF1)(
    const merkle::HashT<32>& l,
    const merkle::HashT<32>& r,
    merkle::HashT<32>& out),
  void (*HF2)(
    const merkle::HashT<32>& l,
    const merkle::HashT<32>& r,
    merkle::HashT<32>& out)>
void compare_roots(
  merkle::TreeT<32, HF1>& mt1, merkle::TreeT<32, HF2>& mt2, const char* name)
{
  auto mt1_root = mt1.root();
  auto mt2_root = mt2.root();

  if (mt1_root != mt2_root)
  {
    std::cout << mt1.num_leaves() << ": " << mt1_root.to_string()
              << " != " << mt2_root.to_string() << '\n';
    std::cout << "mt1: " << '\n';
    std::cout << mt1.to_string(PRNTSZ) << '\n';
    std::cout << name << ": " << '\n';
    std::cout << mt2.to_string(PRNTSZ) << '\n';
    throw std::runtime_error("root hash mismatch");
  }
}

void compare_sha256_hashes()
{
#ifndef NDEBUG
  const size_t num_trees = 1024;
  const size_t root_interval = 31;
#else
  const size_t num_trees = 4096;
  const size_t root_interval = 128;
#endif

  size_t total_inserts = 0;
  size_t total_roots = 0;

  for (size_t k = 0; k < num_trees; k++)
  {
    merkle::Tree mt;

#ifdef HAVE_OPENSSL
    OpenSSLTree mto;
#endif

    // Build trees with k+1 leaves
    int j = 0;
    auto hashes = make_hashes(k + 1);

    for (const auto h : hashes)
    {
      mt.insert(h);

#ifdef HAVE_OPENSSL
      mto.insert(h);
#endif

      total_inserts++;

      if ((j++ % root_interval) == 0)
      {
#ifdef HAVE_OPENSSL
        compare_roots(mt, mto, "OpenSSL");
#endif

        total_roots++;
      }
    }

#ifdef HAVE_OPENSSL
    compare_roots(mt, mto, "OpenSSL");
#endif
  }

  std::cout << num_trees << " trees, " << total_inserts << " inserts, "
            << total_roots << " roots with SHA256: OK" << '\n';
}

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
    static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
        .count()) /
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
    static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
        .count()) /
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

    compare_sha256_hashes();

#ifndef NDEBUG
    const size_t num_leaves = static_cast<size_t>(128) * 1024;
    const size_t root_interval = 128;
#else
    const size_t num_leaves = static_cast<size_t>(16) * 1024 * 1024;
    const size_t root_interval = 1024;
#endif

    auto hashes = make_hashes(num_leaves);

    std::cout << "--- merklecpp trees with SHA256: " << '\n';

    bench<merkle::Tree>(hashes, "merklecpp", root_interval);

#ifdef HAVE_OPENSSL
    bench<OpenSSLTree>(hashes, "OpenSSL", root_interval);
#endif

#ifdef HAVE_OPENSSL
    {
      std::cout << "--- merklecpp trees with SHA384: " << '\n';
      auto hashes384 = make_hashesT<48>(num_leaves);
      benchT<merkle::Tree384, 48>(hashes384, "OpenSSL", root_interval);
    }

    {
      std::cout << "--- merklecpp trees with SHA512: " << '\n';
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