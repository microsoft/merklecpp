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

template <typename Store>
static size_t tile_file_count(const Store& store)
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

class TileWriterProbe : public TileWriter
{
public:
  using TileWriter::TileWriter;

  void mark_level_complete(uint8_t level, uint64_t full_tiles)
  {
    ensure_level(level);
    next_full[level] = full_tiles;
    cursor_inited[level] = 1;
  }
};

template <typename Writer>
class RootFifoProbe : public Writer
{
public:
  using Writer::Writer;

  [[nodiscard]] size_t fifo_size(uint8_t level) const
  {
    return this->root_fifo_size(level);
  }
};

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
        hashes.begin() + (std::ptrdiff_t)TileStore::TILE_WIDTH * 3,
        hashes.begin() + (std::ptrdiff_t)TileStore::TILE_WIDTH * 4);
      expect(
        store.read_tile(TileRef{0, 3}) == expected, "F repaired tile contents");
      expect(tile_file_count(store) == 8, "F exact tile file count");

      std::cout << "F (interior recovery): OK" << '\n';
    }

    // ---- G. Recovery is bounded by the requested tree size, so sparse files
    // at geometrically increasing indices cannot overflow its search.
    {
      const fs::path dir = base / "g";
      const auto hashes = make_hashes(TileStore::TILE_WIDTH);
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
        writer.write_up_to(TileStore::TILE_WIDTH, leaf_at).full_written ==
          0,
        "G bounded sparse recovery");
      expect(store.has_full_tile(0, 0), "G requested tile remains valid");

      std::cout << "G (bounded sparse recovery): OK" << '\n';
    }

    // ---- H. A complete level-2 tile rolls up 256 level-1 child tiles. Prime
    // the lower-level cursors to model a long-lived writer without allocating
    // all 2^24 source leaves.
    {
      const fs::path dir = base / "h";
      TileStore store(dir);
      const auto child_template = make_hashes(TileStore::TILE_WIDTH);
      std::vector<Hash> expected;
      expected.reserve(TileStore::TILE_WIDTH);

      for (uint64_t index = 0; index < TileStore::TILE_WIDTH; index++)
      {
        auto child = child_template;
        child[0].bytes[0] = static_cast<uint8_t>(index);
        store.write_tile(TileRef{1, index}, child);
        expected.push_back(rollup(child));
      }

      TileWriterProbe writer(store);
      writer.mark_level_complete(
        0,
        (uint64_t)TileStore::TILE_WIDTH * TileStore::TILE_WIDTH);
      writer.mark_level_complete(1, TileStore::TILE_WIDTH);

      bool leaf_requested = false;
      const Hash unused;
      const auto leaf_at = [&](uint64_t) -> const Hash& {
        leaf_requested = true;
        return unused;
      };
      constexpr uint64_t size = (uint64_t)TileStore::TILE_WIDTH *
        TileStore::TILE_WIDTH * TileStore::TILE_WIDTH;
      const auto stats = writer.write_up_to(size, leaf_at);

      expect(stats.full_written == 1, "H writes one L2 tile");
      expect(!leaf_requested, "H does not revisit completed lower levels");
      expect(store.has_full_tile(2, 0), "H full L2 tile");
      expect(store.read_tile(TileRef{2, 0}) == expected, "H L2 roll-up");
      expect(!store.has_full_tile(3, 0), "H no L3 tile");
      expect(tile_file_count(store) == 257, "H exact tile file count");

      std::cout << "H (full L2 tile): OK" << '\n';
    }

#ifdef HAVE_OPENSSL
    // ---- I. The templated writer uses the selected hash size and function
    // when producing higher-level tiles.
    {
      using Hash384 = merkle::Tree384::Hash;
      using TileStore384 = merkle::tiles::TileStoreT<
        Hash384::size_bytes,
        merkle::Tree384::hash_function>;
      using TileWriter384 = merkle::tiles::TileWriterT<
        Hash384::size_bytes,
        merkle::Tree384::hash_function>;

      constexpr uint64_t size =
        (uint64_t)TileStore384::TILE_WIDTH * TileStore384::TILE_WIDTH;
      const auto hashes = make_hashesT<Hash384::size_bytes>((size_t)size);
      const auto leaf_at = [&](uint64_t index) -> const Hash384& {
        return hashes[index];
      };

      TileStore384 store(base / "i");
      TileWriter384 writer(store);
      const auto stats = writer.write_up_to(size, leaf_at);

      expect(stats.full_written == 257, "I writes L0 and L1 SHA-384 tiles");
      expect(store.has_full_tile(1, 0), "I full SHA-384 L1 tile");
      const auto level1 = store.read_tile(TileRef{1, 0});
      const std::vector<Hash384> first_tile(
        hashes.begin(), hashes.begin() + TileStore384::TILE_WIDTH);
      expect(
        level1[0] ==
          merkle::tiles::perfect_root<
            Hash384::size_bytes,
            merkle::Tree384::hash_function>(first_tile),
        "I first SHA-384 roll-up");

      merkle::Tree384 tree;
      for (const auto& hash : hashes)
      {
        tree.insert(hash);
      }
      expect(
        merkle::tiles::perfect_root<
          Hash384::size_bytes,
          merkle::Tree384::hash_function>(level1) == tree.root(),
        "I SHA-384 tiled root");
      expect(tile_file_count(store) == 257, "I exact tile file count");

      std::cout << "I (SHA-384 writer): OK" << '\n';
    }
#endif

    // ---- J. Newly written child roots roll up from a bounded FIFO.
    {
      using SmallStore = merkle::tiles::TileStoreT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;
      using SmallWriter = merkle::tiles::TileWriterT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;

      const auto hashes = make_hashes(16);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
      SmallStore store(base / "j");
      RootFifoProbe<SmallWriter> writer(store);
      const auto stats = writer.write_up_to(hashes.size(), leaf_at);

      expect(stats.full_written == 5, "J four L0 tiles and one L1 tile");
      expect(stats.root_fifo_hits == 4, "J parent consumes four cached roots");
      expect(stats.root_fifo_misses == 0, "J parent needs no durable reads");
      expect(writer.fifo_size(0) == 0, "J child FIFO consumed");
      expect(
        writer.fifo_size(1) <= SmallStore::TILE_WIDTH,
        "J FIFO remains bounded");

      std::cout << "J (FIFO hit): OK" << '\n';
    }

    // ---- K. A fresh writer falls back for pre-existing child tiles.
    {
      using SmallStore = merkle::tiles::TileStoreT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;
      using SmallWriter = merkle::tiles::TileWriterT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;

      const auto hashes = make_hashes(16);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
      SmallStore store(base / "k");
      {
        SmallWriter first(store);
        expect(
          first.write_up_to(8, leaf_at).full_written == 2,
          "K first writer publishes two children");
      }

      SmallWriter restarted(store);
      const auto stats = restarted.write_up_to(hashes.size(), leaf_at);
      expect(stats.full_written == 3, "K restart writes two children and parent");
      expect(stats.root_fifo_hits == 2, "K restart uses new child roots");
      expect(
        stats.root_fifo_misses == 2,
        "K restart recomputes pre-existing child roots");

      std::cout << "K (restart fallback): OK" << '\n';
    }

    // ---- L. A gap in newly written indices invalidates FIFO continuity.
    {
      using SmallStore = merkle::tiles::TileStoreT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;
      using SmallWriter = merkle::tiles::TileWriterT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;

      const auto hashes = make_hashes(16);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
      SmallStore store(base / "l");
      store.write_tile(
        TileRef{0, 1},
        std::vector<Hash>(hashes.begin() + 4, hashes.begin() + 8));

      SmallWriter writer(store);
      const auto stats = writer.write_up_to(hashes.size(), leaf_at);
      expect(stats.full_written == 4, "L writes three children and parent");
      expect(stats.root_fifo_hits == 2, "L uses post-gap child roots");
      expect(
        stats.root_fifo_misses == 2,
        "L recomputes roots across pre-existing gap");

      std::cout << "L (gap fallback): OK" << '\n';
    }

    // ---- M. Retry with a fresh writer after a parent-write failure falls back.
    {
      using SmallStore = merkle::tiles::TileStoreT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;
      using SmallWriter = merkle::tiles::TileWriterT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;

      const auto hashes = make_hashes(16);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
      SmallStore store(base / "m");
      const fs::path blocker = store.tile_path(TileRef{1, 0});
      fs::create_directories(blocker);
      {
        SmallWriter interrupted(store);
        bool threw = false;
        try
        {
          (void)interrupted.write_up_to(hashes.size(), leaf_at);
        }
        catch (const std::exception&)
        {
          threw = true;
        }
        expect(threw, "M blocked parent write throws");
        expect(store.has_full_tile(0, 3), "M child tiles remain durable");
      }

      fs::remove(blocker);
      SmallWriter retry(store);
      const auto stats = retry.write_up_to(hashes.size(), leaf_at);
      expect(stats.full_written == 1, "M retry writes only parent");
      expect(stats.root_fifo_hits == 0, "M retry has no stale FIFO state");
      expect(
        stats.root_fifo_misses == 4,
        "M retry recomputes every durable child root");

      std::cout << "M (failure fallback): OK" << '\n';
    }

    // ---- N. Moving a writer preserves cursors but drops opportunistic roots.
    {
      using SmallStore = merkle::tiles::TileStoreT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;
      using SmallWriter = merkle::tiles::TileWriterT<
        Hash::size_bytes,
        merkle::Tree::hash_function,
        2>;

      const auto hashes = make_hashes(16);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
      SmallStore store(base / "n");
      SmallWriter original(store);
      expect(
        original.write_up_to(12, leaf_at).full_written == 3,
        "N original writer publishes three children");

      SmallWriter moved(std::move(original));
      const auto stats = moved.write_up_to(hashes.size(), leaf_at);
      expect(stats.full_written == 2, "N moved writer writes child and parent");
      expect(stats.root_fifo_hits == 1, "N moved writer uses post-move root");
      expect(
        stats.root_fifo_misses == 3,
        "N moved writer recomputes pre-move child roots");

      std::cout << "N (move fallback): OK" << '\n';
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
