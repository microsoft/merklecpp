// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::CombinedHashSource;
using merkle::tiles::MemoryHashSource;
using merkle::tiles::ProofEngine;
using merkle::tiles::TileHashSource;
using merkle::tiles::TileStore;
using merkle::tiles::TileWriter;

static void expect(bool cond, const std::string& what)
{
  if (!cond)
  {
    throw std::runtime_error("check failed: " + what);
  }
}

// Exercises tile-derived proofs for a tree of `n` leaves against the existing
// library (which acts as the oracle: proofs must be byte-identical). Full tiles
// serve the covered prefix and an in-memory tree serves the un-tiled frontier,
// exactly as TiledTree combines them.
static void check_size(
  const fs::path& dir, uint64_t n, const std::vector<Hash>& hashes)
{
  const std::string at = " @n=" + std::to_string(n);

  TileStore store(dir);
  TileWriter writer(store);
  const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
  writer.write_up_to(n, leaf_at);

  // Oracle: a full, never-flushed tree with the same leaves.
  merkle::Tree tree;
  for (uint64_t i = 0; i < n; i++)
  {
    tree.insert(hashes[i]);
  }
  const Hash root = tree.root();

  // Production-shaped source: full tiles serve the covered prefix, an in-memory
  // tree serves the un-tiled frontier. Drop the tiled past from the frontier
  // tree so proofs over it are genuinely served from the tiles. merklecpp keeps
  // at least one resident leaf, so never flush the whole tree.
  const uint64_t covered = (n / 256) * 256; // 256 == TILE_WIDTH
  merkle::Tree frontier;
  for (uint64_t i = 0; i < n; i++)
  {
    frontier.insert(hashes[i]);
  }
  uint64_t drop_to = covered;
  if (n > 0 && drop_to >= n)
  {
    drop_to = n - 1;
  }
  if (drop_to > 0)
  {
    frontier.flush_to((size_t)drop_to);
  }
  const MemoryHashSource mem(frontier);
  const TileHashSource tiles(store, covered);
  const CombinedHashSource source(mem, tiles);
  const ProofEngine engine(source);

  // Root recomputed from tiles equals the library root.
  expect(engine.root(n) == root, "root" + at);

  // Indices to probe: all of them for small trees, else a spread.
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
    for (const uint64_t i :
         {(uint64_t)0, (uint64_t)1, n / 3, n / 2, n - 2, n - 1})
    {
      indices.push_back(i);
    }
  }

  // Inclusion proofs are identical to TreeT::path and verify.
  for (const uint64_t i : indices)
  {
    if (i >= n)
    {
      continue;
    }
    const auto p = engine.inclusion_proof(i, n);
    expect(*p == *tree.path(i), "inclusion==path i=" + std::to_string(i) + at);
    expect(p->verify(root), "inclusion verify i=" + std::to_string(i) + at);
  }

  // Consistency pairs: exhaustive for small trees, else a fixed spread.
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
    pairs = {{1, n}, {n / 2, n}, {n - 1, n}, {1, 2}};
    // Tile-boundary crossings.
    if (n > 256)
    {
      pairs.emplace_back(256, n);
      pairs.emplace_back(257, n);
    }
  }

  for (const auto& pr : pairs)
  {
    const uint64_t m = pr.first;
    const uint64_t k = pr.second;
    if (m == 0 || m >= k || k > n)
    {
      continue;
    }

    const Hash rm = engine.root(m);
    const Hash rk = engine.root(k);
    expect(rm == *tree.past_root(m - 1), "past_root m" + at);
    expect(rk == *tree.past_root(k - 1), "past_root k" + at);

    // Past inclusion proof matches TreeT::past_path.
    const uint64_t i = m / 2;
    const auto pp = engine.inclusion_proof(i, m);
    expect(
      *pp == *tree.past_path(i, m - 1),
      "inclusion(m)==past_path i=" + std::to_string(i) + at);
    expect(pp->verify(rm), "inclusion(m) verify" + at);

    // Consistency proof reconciles the two roots.
    const auto cp = engine.consistency_proof(m, k);
    expect(
      ProofEngine::verify_consistency(m, k, rm, rk, cp),
      "consistency " + std::to_string(m) + "->" + std::to_string(k) + at);

    // The index-based variant is consistency_proof(i+1, j+1).
    expect(
      engine.consistency_proof_from_indices(m - 1, k - 1) == cp,
      "consistency index variant" + at);

    // Tampering with a proof element or a root is rejected.
    auto bad = cp;
    bad[0].bytes[0] ^= 0xFFU;
    expect(
      !ProofEngine::verify_consistency(m, k, rm, rk, bad),
      "consistency tamper rejected" + at);

    Hash wrong = rk;
    wrong.bytes[0] ^= 0xFFU;
    expect(
      !ProofEngine::verify_consistency(m, k, rm, wrong, cp),
      "consistency wrong root rejected" + at);
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

int main()
{
  const auto seed = std::time(nullptr);
  std::srand((unsigned)seed);
  std::cout << "seed=" << seed << '\n';

  const fs::path base = fs::temp_directory_path() /
    ("merklecpp_tiles_proofs_" + std::to_string((unsigned long long)seed) +
     "_" + std::to_string(std::rand()));

  try
  {
    const auto hashes = make_hashes(70000);

    for (const uint64_t n :
         {(uint64_t)1,
          (uint64_t)2,
          (uint64_t)3,
          (uint64_t)4,
          (uint64_t)5,
          (uint64_t)7,
          (uint64_t)8,
          (uint64_t)13,
          (uint64_t)16,
          (uint64_t)255,
          (uint64_t)256,
          (uint64_t)257,
          (uint64_t)1000})
    {
      check_size(base / ("n" + std::to_string(n)), n, hashes);
    }
    std::cout << "small/medium sizes: OK" << '\n';

    // Large tree: exercises full L1 tiles and the in-memory frontier.
    check_size(base / "big", 70000, hashes);
    std::cout << "size 70000: OK" << '\n';

    std::cout << "tiles_proofs: OK" << '\n';

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
