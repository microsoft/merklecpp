// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::MemoryHashSource;
using merkle::tiles::ProofEngine;
using merkle::tiles::TiledTree;

static void expect(bool cond, const std::string& what)
{
  if (!cond)
  {
    throw std::runtime_error("check failed: " + what);
  }
}

// Validates the TreeT::subtree_root accessor via memory-only proofs: they must
// match the library exactly.
static void check_memory_source(uint64_t n, const std::vector<Hash>& hashes)
{
  const std::string at = " @n=" + std::to_string(n);

  merkle::Tree tree;
  for (uint64_t i = 0; i < n; i++)
  {
    tree.insert(hashes[i]);
  }
  const Hash root = tree.root();

  MemoryHashSource source(tree);
  ProofEngine engine(source);

  expect(engine.root(n) == root, "mem root" + at);

  std::vector<uint64_t> indices;
  if (n <= 16)
  {
    for (uint64_t i = 0; i < n; i++)
    {
      indices.push_back(i);
    }
  }
  else
  {
    for (uint64_t i : {(uint64_t)0, (uint64_t)1, n / 2, n - 1})
    {
      indices.push_back(i);
    }
  }

  for (uint64_t i : indices)
  {
    const auto p = engine.inclusion_proof(i, n);
    expect(
      *p == *tree.path(i), "mem inclusion==path i=" + std::to_string(i) + at);
    expect(p->verify(root), "mem inclusion verify i=" + std::to_string(i) + at);
  }

  std::vector<std::pair<uint64_t, uint64_t>> pairs;
  if (n <= 16)
  {
    for (uint64_t m = 1; m < n; m++)
    {
      for (uint64_t k = m + 1; k <= n; k++)
      {
        pairs.emplace_back(m, k);
      }
    }
  }
  else
  {
    pairs = {{1, n}, {n / 2, n}, {n - 1, n}};
  }

  for (const auto& pr : pairs)
  {
    const uint64_t m = pr.first;
    const uint64_t k = pr.second;
    const Hash rm = engine.root(m);
    const Hash rk = engine.root(k);
    expect(rm == *tree.past_root(m - 1), "mem past_root m" + at);

    const auto pp = engine.inclusion_proof(m / 2, m);
    expect(
      *pp == *tree.past_path(m / 2, m - 1), "mem inclusion(m)==past_path" + at);

    const auto cp = engine.consistency_proof(m, k);
    expect(
      ProofEngine::verify_consistency(m, k, rm, rk, cp),
      "mem consistency" + at);
  }
}

int main()
{
  const auto seed = std::time(nullptr);
  std::srand((unsigned)seed);
  std::cout << "seed=" << seed << '\n';

  const fs::path base = fs::temp_directory_path() /
    ("merklecpp_tiles_tree_" + std::to_string((unsigned long long)seed) + "_" +
     std::to_string(std::rand()));

  try
  {
    const auto hashes = make_hashes(1500);

    // ---- Part 1: memory-source proofs (exercises TreeT::subtree_root).
    for (uint64_t n :
         {(uint64_t)1,
          (uint64_t)2,
          (uint64_t)3,
          (uint64_t)5,
          (uint64_t)8,
          (uint64_t)13,
          (uint64_t)16,
          (uint64_t)256,
          (uint64_t)257,
          (uint64_t)1000})
    {
      check_memory_source(n, hashes);
    }
    std::cout << "memory source: OK" << '\n';

    // ---- Part 2: TiledTree checkpoint + flush, proofs over tiles + memory.
    const uint64_t n1 = 1000; // first checkpoint size
    const uint64_t N = 1500; // final size

    // Reference: a plain (never flushed) tree with the same leaves.
    merkle::Tree ref;
    for (uint64_t i = 0; i < N; i++)
    {
      ref.insert(hashes[i]);
    }
    const Hash ref_root = ref.root();

    TiledTree::Config cfg;
    cfg.prefix = base / "tt";
    cfg.retention_margin = 0;
    TiledTree tt(cfg);

    for (uint64_t i = 0; i < n1; i++)
    {
      tt.append(hashes[i]);
    }
    tt.checkpoint(); // tiles_size = 1000; flush to covered = 768
    expect(tt.checkpoint_size() == n1, "checkpoint size");
    expect(tt.tree_ref().min_index() == 768, "flushed to 768");

    for (uint64_t i = n1; i < N; i++)
    {
      tt.append(hashes[i]);
    }
    expect(tt.size() == N, "size after appends");
    expect(tt.root() == ref_root, "tiled root == reference root");

    // Indices that are: flushed (tiles only), in the checkpointed-but-resident
    // overlap, and on the un-checkpointed resident frontier.
    for (uint64_t i :
         {(uint64_t)0,
          (uint64_t)767,
          (uint64_t)800,
          (uint64_t)999,
          (uint64_t)1000,
          (uint64_t)1499})
    {
      const auto p = tt.inclusion_proof(i, N);
      expect(
        *p == *ref.path(i), "combined inclusion==ref i=" + std::to_string(i));
      expect(
        p->verify(ref_root),
        "combined inclusion verify i=" + std::to_string(i));
    }

    // The resident tree alone cannot prove a flushed index.
    bool threw = false;
    try
    {
      (void)tt.tree_ref().path(0);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    expect(threw, "memory-only path throws for flushed index");

    // Consistency across the flush boundary: checkpoint size -> current size,
    // and a flushed-era size -> current size.
    {
      const auto cp = tt.consistency_proof(n1, N);
      expect(
        ProofEngine::verify_consistency(
          n1, N, *ref.past_root(n1 - 1), ref_root, cp),
        "consistency n1->N across flush");

      const uint64_t m = 500; // flushed era
      const auto cp2 = tt.consistency_proof(m, N);
      expect(
        ProofEngine::verify_consistency(
          m, N, *ref.past_root(m - 1), ref_root, cp2),
        "consistency m->N across flush");
    }

    // A second checkpoint flushes further; proofs for now-flushed indices still
    // work (this also confirms the writer never reads a flushed leaf).
    tt.checkpoint(); // tiles_size = 1500; flush to covered = 1280
    expect(tt.checkpoint_size() == N, "second checkpoint size");
    expect(tt.tree_ref().min_index() == 1280, "flushed to 1280");

    for (uint64_t i :
         {(uint64_t)0,
          (uint64_t)1000,
          (uint64_t)1279,
          (uint64_t)1280,
          (uint64_t)1499})
    {
      const auto p = tt.inclusion_proof(i, N);
      expect(
        *p == *ref.path(i), "post-2nd inclusion==ref i=" + std::to_string(i));
      expect(
        p->verify(ref_root),
        "post-2nd inclusion verify i=" + std::to_string(i));
    }

    std::cout << "tiled tree (flush + combination): OK" << '\n';

    std::cout << "tiles_tree: OK" << '\n';

    std::error_code ec;
    fs::remove_all(base, ec);
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << '\n';
    std::error_code ec;
    fs::remove_all(base, ec);
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << '\n';
    std::error_code ec;
    fs::remove_all(base, ec);
    return 1;
  }

  return 0;
}
