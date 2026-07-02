// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

// End-to-end coverage of the level-2 tile paths. A full level-2 tile requires
// 256^3 == 16,777,216 leaves, so this is a deliberately large test: it writes
// the tiles for that many leaves (~65k tile files) from a deterministic leaf
// source -- no in-memory tree is built -- and cross-checks the writer's
// level-by-level roll-up against TileHashSourceT::resolve (which reads the
// level-2 tile) and against the underlying leaves.

#include "util.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::TILE_WIDTH;
using merkle::tiles::TileHashSource;
using merkle::tiles::TileRef;
using merkle::tiles::TileStore;
using merkle::tiles::TileWriter;

static void expect(bool cond, const std::string& what)
{
  if (!cond)
  {
    throw std::runtime_error("check failed: " + what);
  }
}

// Roll up a perfect (power-of-two) set of hashes with the default combiner.
static Hash rollup(const std::vector<Hash>& hashes)
{
  return merkle::tiles::perfect_root<32, merkle::sha256_compress>(hashes);
}

int main()
{
  const fs::path dir = fs::temp_directory_path() /
    ("merklecpp_tiles_level2_" +
     std::to_string((unsigned long long)std::time(nullptr)));

  try
  {
    // 256^3 leaves == exactly one full level-2 tile.
    const uint64_t n =
      (uint64_t)TILE_WIDTH * (uint64_t)TILE_WIDTH * (uint64_t)TILE_WIDTH;

    TileStore store(dir);
    TileWriter writer(store);

    // Deterministic leaf hash derived from the index (low 8 bytes); avoids
    // materialising a 16.7M-entry vector.
    Hash leaf;
    const auto leaf_at = [&](uint64_t i) -> const Hash& {
      leaf.zero();
      for (int b = 0; b < 8; b++)
      {
        leaf.bytes[b] = (uint8_t)(i >> (8 * b));
      }
      return leaf;
    };

    const auto stats = writer.write_up_to(n, leaf_at);

    // 65536 full L0 tiles + 256 full L1 tiles + 1 full L2 tile.
    expect(
      stats.full_written == (uint64_t)TILE_WIDTH * TILE_WIDTH + TILE_WIDTH + 1,
      "level2: tile counts");
    expect(store.has_full_tile(2, 0), "level2: L2 tile present");
    expect(!store.has_full_tile(2, 1), "level2: no second L2 tile");
    expect(!store.has_full_tile(3, 0), "level2: no L3 tile");

    const auto l2 = store.read_tile(TileRef{2, 0});
    expect(l2.size() == TILE_WIDTH, "level2: L2 tile width");

    const TileHashSource src(store, n);

    // Each level-2 entry j is the root of level-1 tile j, which rolls up
    // level-0 tiles, which are the leaves verbatim. Cross-check the writer's
    // roll-up, resolve's level-2 read, and the leaf chain on a sample of
    // indices.
    for (const uint64_t j :
         {(uint64_t)0, (uint64_t)1, (uint64_t)200, (uint64_t)255})
    {
      const auto l1j = store.read_tile(TileRef{1, j});
      expect(l2[j] == rollup(l1j), "level2: L2[j] == rollup(L1 tile j)");

      // resolve reads the level-2 tile for the 2^16-leaf subtree at index j.
      Hash r16;
      expect(
        src.subtree_root(16, j, r16), "level2: subtree_root(16,j) resolves");
      expect(r16 == l2[j], "level2: resolve(16,j) == L2[j]");

      // Anchor to leaves: L1 tile j entry 0 == root of L0 tile (j*256), whose
      // first entry is leaf (j * 65536).
      const auto l0 = store.read_tile(TileRef{0, j * TILE_WIDTH});
      expect(l1j[0] == rollup(l0), "level2: L1[j][0] == rollup(L0 tile)");
      expect(
        l0[0] == leaf_at(j * (uint64_t)TILE_WIDTH * TILE_WIDTH),
        "level2: L0 entry == leaf");
    }

    // Intra-tile roll-up: the 2^17-leaf subtree at 0 hashes L2[0] and L2[1].
    Hash r17;
    expect(src.subtree_root(17, 0, r17), "level2: subtree_root(17,0) resolves");
    expect(
      r17 == rollup({l2[0], l2[1]}), "level2: resolve(17,0) == H(L2[0],L2[1])");

    std::cout << "tiles_level2: OK" << '\n';

    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << '\n';
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << '\n';
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 1;
  }

  return 0;
}
