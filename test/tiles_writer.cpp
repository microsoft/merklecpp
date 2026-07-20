// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tiles_test_util.h"
#include "util.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

static size_t tile_file_count(const TileStore& store)
{
  const fs::path tiles = store.root() / "tile";
  if (!fs::exists(tiles))
  {
    return 0;
  }
  const fs::recursive_directory_iterator it(tiles);
  return (size_t)std::count_if(
    begin(it), end(it), [](const fs::directory_entry& e) {
      return e.is_regular_file();
    });
}

// Roll up a full level-0 tile and compare with a level-1 tile entry.
static Hash rollup(const std::vector<Hash>& leaves)
{
  return merkle::tiles::perfect_root<
    merkle::Tree::Hash::size_bytes,
    merkle::Tree::hash_function>(leaves);
}

static void overwrite_file(
  const fs::path& path, const std::vector<uint8_t>& bytes)
{
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(
    reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
}

int main()
{
  const TemporaryDirectory temporary_directory("merklecpp_tiles_writer");
  const fs::path& base = temporary_directory.path();

  try
  {
    // ---- A. size 256: exactly one full L0 tile and no L1 tile (the L1 root is
    //         not yet a full tile, so it stays in memory). The level-1 entry it
    //         would hold equals the real merkle::Tree root.
    {
      const auto hashes = make_hashes(256);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };

      TileStore store(base / "a");
      TileWriter writer(store);
      const auto s = writer.write_up_to(256, leaf_at);

      expect(s.full_written == 1, "A full_written");

      expect(store.has_full_tile(0, 0), "A L0 full tile");
      expect(
        fs::file_size(store.tile_path(TileRef{0, 0})) == 256U * Hash().size(),
        "A L0 full tile size");
      expect(!store.has_full_tile(1, 0), "A no L1 full tile");
      expect(tile_file_count(store) == 1, "A exact tile file count");

      // Level-0 tile is the leaf hashes verbatim.
      const auto l0 = store.read_tile(TileRef{0, 0});
      for (size_t i = 0; i < hashes.size(); i++)
      {
        expect(l0[i] == hashes[i], "A L0 entry == leaf");
      }

      // The (un-tiled) level-1 entry == root of the equivalent merkle::Tree.
      merkle::Tree tree;
      for (const auto& h : hashes)
      {
        tree.insert(h);
      }
      expect(rollup(l0) == tree.root(), "A rollup(L0) == tree root");

      std::cout << "A (size 256): OK" << '\n';
    }

    // ---- B. size 70000: full tiles only (273 full L0 + 1 full L1 = 274); the
    //         incomplete frontier at every level is left untiled.
    {
      const auto hashes = make_hashes(70000);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };

      TileStore store(base / "b");
      TileWriter writer(store);
      const auto s = writer.write_up_to(70000, leaf_at);

      expect(s.full_written == 274, "B full_written");

      expect(store.has_full_tile(0, 0), "B L0 tile 0");
      expect(store.has_full_tile(0, 272), "B L0 tile 272");
      expect(!store.has_full_tile(0, 273), "B no L0 tile 273");

      expect(store.has_full_tile(1, 0), "B L1 tile 0");
      expect(!store.has_full_tile(1, 1), "B no L1 tile 1");

      expect(!fs::exists(store.root() / "tile" / "2"), "B no level 2");
      expect(!fs::exists(store.root() / "tile" / "3"), "B no level 3");
      expect(tile_file_count(store) == 274, "B exact tile file count");

      // Higher-level entries are roll-ups of the complete child tiles.
      const auto l1 = store.read_tile(TileRef{1, 0});
      expect(l1.size() == 256, "B L1 full width");
      expect(l1[0] == rollup(store.read_tile(TileRef{0, 0})), "B L1[0]");
      expect(l1[255] == rollup(store.read_tile(TileRef{0, 255})), "B L1[255]");

      std::cout << "B (size 70000): OK" << '\n';
    }

    // ---- C. incremental writes preserve immutability and idempotency.
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

      // Grow to 600: one new full L0 tile (covering 512), nothing else.
      const auto s3 = writer.write_up_to(600, leaf_at);
      expect(s3.full_written == 1, "C s3 full");

      expect(store.has_full_tile(0, 0), "C L0 tile 0");
      expect(store.has_full_tile(0, 1), "C L0 tile 1");
      expect(!store.has_full_tile(0, 2), "C no L0 tile 2");
      expect(!store.has_full_tile(1, 0), "C no L1 full tile");
      expect(tile_file_count(store) == 2, "C exact tile file count");

      std::cout << "C (incremental): OK" << '\n';
    }

    // ---- D. crossing into a full level-1 tile: the roll-up appears as a full
    //         tile and prior full tiles are never rewritten.
    {
      const auto hashes = make_hashes(65536);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };

      TileStore store(base / "d");
      TileWriter writer(store);

      // One leaf short of a full L1 tile: 255 full L0 tiles, no L1 yet.
      const auto s1 = writer.write_up_to(65535, leaf_at);
      expect(s1.full_written == 255, "D s1 255 full L0");
      expect(!store.has_full_tile(1, 0), "D no L1 before completion");
      expect(tile_file_count(store) == 255, "D initial tile file count");

      // Completing the 256th L0 tile yields one new L0 tile and one full L1.
      const auto s2 = writer.write_up_to(65536, leaf_at);
      expect(s2.full_written == 2, "D s2 new L0 + new L1");
      expect(store.has_full_tile(0, 255), "D L0 tile 255");
      expect(store.has_full_tile(1, 0), "D L1 tile 0");
      expect(!store.has_full_tile(1, 1), "D no L1 tile 1");
      expect(!store.has_full_tile(2, 0), "D no L2 tile");
      expect(tile_file_count(store) == 257, "D final tile file count");

      const auto l1 = store.read_tile(TileRef{1, 0});
      expect(l1[0] == rollup(store.read_tile(TileRef{0, 0})), "D L1[0]");

      // Re-running writes nothing (everything already full and immutable).
      const auto s3 = writer.write_up_to(65536, leaf_at);
      expect(s3.full_written == 0, "D s3 immutable rerun");

      std::cout << "D (full L1 tile): OK" << '\n';
    }

    // ---- E. resume: a fresh writer over an existing store rebuilds its cursor
    //         from disk via full_prefix_length, never rewriting full tiles and
    //         writing only the newly-complete ones.
    {
      const auto hashes = make_hashes(70000);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };

      const fs::path dir = base / "e";
      {
        TileStore store(dir);
        TileWriter writer(store);
        expect(
          writer.write_up_to(600, leaf_at).full_written == 2,
          "E first writer 2 L0 tiles"); // 600 / 256 == 2 full L0 tiles
      }

      // A brand-new store + writer over the same directory: its cursor must
      // resume from what is already on disk.
      TileStore store(dir);
      TileWriter writer(store);
      expect(
        writer.write_up_to(600, leaf_at).full_written == 0,
        "E resume rewrites nothing");

      // Extending to 70000 writes only the missing tiles: 273 - 2 == 271 new L0
      // plus 1 new L1 (which rolls up the L0 tiles the first writer wrote).
      expect(
        writer.write_up_to(70000, leaf_at).full_written == 272,
        "E resume writes only new tiles");
      expect(store.has_full_tile(0, 0), "E L0 tile 0 still present");
      expect(store.has_full_tile(0, 272), "E L0 tile 272");
      expect(store.has_full_tile(1, 0), "E L1 tile 0");
      expect(
        store.read_tile(TileRef{1, 0})[0] ==
          rollup(store.read_tile(TileRef{0, 0})),
        "E L1[0] rolled up from resumed L0");
      expect(tile_file_count(store) == 274, "E exact tile file count");

      // A third fresh writer confirms full idempotence after a resume.
      TileStore store3(dir);
      TileWriter writer3(store3);
      expect(
        writer3.write_up_to(70000, leaf_at).full_written == 0,
        "E second resume idempotent");

      std::cout << "E (writer resume): OK" << '\n';
    }

    // ---- F. Resume discovers the first hole rather than trusting later valid
    // files as proof that the prefix is contiguous.
    {
      const auto hashes = make_hashes(2048);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
      const fs::path dir = base / "f";

      {
        TileStore store(dir);
        TileWriter writer(store);
        expect(
          writer.write_up_to(2048, leaf_at).full_written == 8,
          "F initial tiles");
        overwrite_file(store.tile_path(TileRef{0, 3}), {0});
        expect(!store.has_full_tile(0, 3), "F interior tile corrupt");
        expect(store.has_full_tile(0, 7), "F later tile remains valid");
      }

      TileStore store(dir);
      TileWriter writer(store);
      expect(
        writer.write_up_to(2048, leaf_at).full_written == 1,
        "F rewrites interior hole");
      const std::vector<Hash> expected(
        hashes.begin() + (std::ptrdiff_t)merkle::tiles::TILE_WIDTH * 3,
        hashes.begin() + (std::ptrdiff_t)merkle::tiles::TILE_WIDTH * 4);
      expect(
        store.read_tile(TileRef{0, 3}) == expected, "F repaired tile contents");
      expect(tile_file_count(store) == 8, "F exact tile file count");

      std::cout << "F (interior recovery): OK" << '\n';
    }

    // ---- G. Recovery is bounded by the requested tree size, so sparse files
    // at geometrically increasing indices cannot overflow its search.
    {
      const fs::path dir = base / "g";
      const auto hashes = make_hashes(merkle::tiles::TILE_WIDTH);
      {
        TileStore store(dir);
        store.write_tile(TileRef{0, 0}, hashes);
        for (uint64_t index = 1;; index <<= 1)
        {
          store.write_tile(TileRef{0, index}, hashes);
          if (index == (uint64_t{1} << 63))
          {
            break;
          }
        }
      }

      TileStore store(dir);
      TileWriter writer(store);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
      expect(
        writer.write_up_to(merkle::tiles::TILE_WIDTH, leaf_at).full_written ==
          0,
        "G bounded sparse recovery");
      expect(store.has_full_tile(0, 0), "G requested tile remains valid");

      std::cout << "G (bounded sparse recovery): OK" << '\n';
    }

    std::cout << "tiles_writer: OK" << '\n';
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
