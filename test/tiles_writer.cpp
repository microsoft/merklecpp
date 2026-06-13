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
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
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

static bool partial_dir_exists(const TileStore& store, uint8_t L, uint64_t idx)
{
  return fs::exists(store.tile_path(TileRef{L, idx, 1}).parent_path());
}

// Roll up a full level-0 tile and compare with a level-1 tile entry.
static Hash rollup(const std::vector<Hash>& leaves)
{
  return merkle::tiles::perfect_root<32, merkle::sha256_compress>(leaves);
}

int main()
{
  const auto seed = std::time(nullptr);
  std::srand((unsigned)seed);
  std::cout << "seed=" << seed << '\n';

  const fs::path base = fs::temp_directory_path() /
    ("merklecpp_tiles_writer_" + std::to_string((unsigned long long)seed) +
     "_" + std::to_string(std::rand()));

  try
  {
    // ---- A. size 256: one full L0 tile + one width-1 L1 partial, and the L1
    //         entry equals the real merkle::Tree root (roll-up == tree hash).
    {
      const auto hashes = make_hashes(256);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };

      TileStore store(base / "a");
      TileWriter writer(store);
      const auto s = writer.write_up_to(256, leaf_at);

      expect(s.full_written == 1, "A full_written");
      expect(s.partial_written == 1, "A partial_written");
      expect(s.partial_removed == 0, "A partial_removed");

      expect(store.has_full_tile(0, 0), "A L0 full tile");
      expect(
        fs::file_size(store.tile_path(TileRef{0, 0, 0})) ==
          256u * Hash().size(),
        "A L0 full tile size");
      expect(!partial_dir_exists(store, 0, 0), "A no L0 partial");
      expect(!store.has_full_tile(1, 0), "A no L1 full tile");
      expect(store.has_tile(TileRef{1, 0, 1}), "A L1 partial width 1");

      // Level-0 tile is the leaf hashes verbatim.
      const auto l0 = store.read_tile(TileRef{0, 0, 0});
      for (size_t i = 0; i < hashes.size(); i++)
      {
        expect(l0[i] == hashes[i], "A L0 entry == leaf");
      }

      // Level-1 entry == root of the equivalent merkle::Tree.
      merkle::Tree tree;
      for (const auto& h : hashes)
      {
        tree.insert(h);
      }
      const Hash root = tree.root();
      const auto l1 = store.read_tile(TileRef{1, 0, 1});
      expect(l1.size() == 1, "A L1 partial width");
      expect(l1[0] == root, "A L1 entry == tree root");
      expect(rollup(l0) == root, "A rollup(L0) == tree root");

      std::cout << "A (size 256): OK" << '\n';
    }

    // ---- B. size 70000: the spec's worked example, exact tile set/widths.
    {
      const auto hashes = make_hashes(70000);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };

      TileStore store(base / "b");
      TileWriter writer(store);
      const auto s = writer.write_up_to(70000, leaf_at);

      // 273 full L0 + 1 full L1 = 274 full; partials at L0, L1, L2.
      expect(s.full_written == 274, "B full_written");
      expect(s.partial_written == 3, "B partial_written");
      expect(s.partial_removed == 0, "B partial_removed");

      expect(store.has_full_tile(0, 0), "B L0 tile 0");
      expect(store.has_full_tile(0, 272), "B L0 tile 272");
      expect(!store.has_full_tile(0, 273), "B no L0 tile 273");
      expect(store.has_tile(TileRef{0, 273, 112}), "B L0 partial width 112");
      expect(
        fs::file_size(store.tile_path(TileRef{0, 273, 112})) ==
          112u * Hash().size(),
        "B L0 partial size");

      expect(store.has_full_tile(1, 0), "B L1 tile 0");
      expect(!store.has_full_tile(1, 1), "B no L1 tile 1");
      expect(store.has_tile(TileRef{1, 1, 17}), "B L1 partial width 17");

      expect(store.has_tile(TileRef{2, 0, 1}), "B L2 partial width 1");
      expect(!fs::exists(store.root() / "tile" / "3"), "B no level 3");

      // Higher-level entries are roll-ups of the complete child tiles.
      const auto l1 = store.read_tile(TileRef{1, 0, 0});
      expect(l1.size() == 256, "B L1 full width");
      expect(l1[0] == rollup(store.read_tile(TileRef{0, 0, 0})), "B L1[0]");
      expect(
        l1[255] == rollup(store.read_tile(TileRef{0, 255, 0})), "B L1[255]");

      std::cout << "B (size 70000): OK" << '\n';
    }

    // ---- C. incremental writes: immutability, idempotency, partial growth.
    {
      const auto hashes = make_hashes(1024);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };

      TileStore store(base / "c");
      TileWriter writer(store);

      const auto s1 = writer.write_up_to(256, leaf_at);
      expect(s1.full_written == 1, "C s1 full");

      // Re-running at the same size writes nothing (full tiles are immutable).
      const auto s2 = writer.write_up_to(256, leaf_at);
      expect(s2.full_written == 0, "C s2 full immutable");
      expect(s2.partial_written == 0, "C s2 partial idempotent");
      expect(s2.partial_removed == 0, "C s2 nothing removed");

      // Grow to 600: one new full L0 tile, new L0 partial, L1 partial widens.
      const auto s3 = writer.write_up_to(600, leaf_at);
      expect(s3.full_written == 1, "C s3 full");
      expect(s3.partial_written == 2, "C s3 partial");

      expect(store.has_full_tile(0, 0), "C L0 tile 0");
      expect(store.has_full_tile(0, 1), "C L0 tile 1");
      expect(store.has_tile(TileRef{0, 2, 88}), "C L0 partial width 88");
      expect(store.has_tile(TileRef{1, 0, 2}), "C L1 partial width 2");
      expect(!store.has_tile(TileRef{1, 0, 1}), "C old L1 partial removed");
      expect(!store.has_full_tile(1, 0), "C no L1 full tile");

      std::cout << "C (incremental): OK" << '\n';
    }

    // ---- D. supersession: a partial whose index becomes a full tile is
    //         removed when it is covered.
    {
      const auto hashes = make_hashes(1024);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };

      TileStore store(base / "d");
      TileWriter writer(store);

      writer.write_up_to(300, leaf_at);
      expect(store.has_full_tile(0, 0), "D L0 tile 0");
      expect(store.has_tile(TileRef{0, 1, 44}), "D L0 partial width 44");
      expect(store.has_tile(TileRef{1, 0, 1}), "D L1 partial width 1");

      const auto s = writer.write_up_to(512, leaf_at);
      expect(s.full_written == 1, "D full");
      expect(s.partial_removed == 1, "D one partial superseded");

      expect(store.has_full_tile(0, 1), "D L0 tile 1");
      expect(
        !partial_dir_exists(store, 0, 1), "D superseded L0 partial removed");
      expect(!store.has_tile(TileRef{0, 1, 44}), "D old L0 partial gone");
      expect(store.has_tile(TileRef{1, 0, 2}), "D L1 partial width 2");
      expect(!store.has_tile(TileRef{1, 0, 1}), "D old L1 partial gone");

      std::cout << "D (supersession): OK" << '\n';
    }

    std::cout << "tiles_writer: OK" << '\n';

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
