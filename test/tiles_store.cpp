// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::TileRef;

static void expect(bool cond, const std::string& what)
{
  if (!cond)
  {
    throw std::runtime_error("check failed: " + what);
  }
}

static void expect_eq(
  const std::string& got, const std::string& expected, const std::string& what)
{
  if (got != expected)
  {
    throw std::runtime_error(
      what + ": got '" + got + "', expected '" + expected + "'");
  }
}

static std::string rel(const merkle::tiles::TileStore& store, const fs::path& p)
{
  return p.lexically_relative(store.root()).generic_string();
}

static bool any_tmp_files(const fs::path& dir)
{
  if (!fs::exists(dir))
  {
    return false;
  }
  return std::any_of(
    fs::recursive_directory_iterator(dir),
    fs::recursive_directory_iterator(),
    [](const auto& entry) {
      return entry.path().filename().string().find(".tmp") != std::string::npos;
    });
}

class TileStoreProbe : public merkle::tiles::TileStore
{
public:
  using Store = merkle::tiles::TileStore;

  static std::vector<fs::path> create_and_record_directory_syncs(
    const fs::path& directory)
  {
    std::vector<fs::path> synced;
    Store::create_directories_durably(
      directory, [&](const fs::path& parent) { synced.push_back(parent); });
    return synced;
  }

  static void check_write_progress(size_t written)
  {
    Store::require_write_progress(written, "test");
  }
};

// Overwrites a file with exactly the given bytes (to simulate corruption).
static void overwrite_file(const fs::path& p, const std::vector<uint8_t>& bytes)
{
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(
    reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
}

int main()
{
  const auto seed = std::time(nullptr);
  std::srand((unsigned)seed);
  std::cout << "seed=" << seed << '\n';

  const fs::path dir = fs::temp_directory_path() /
    ("merklecpp_tiles_" + std::to_string((unsigned long long)seed) + "_" +
     std::to_string(std::rand()));

  try
  {
    // 1. Tile index encoding vectors (tlog-tiles examples).
    expect_eq(merkle::tiles::encode_tile_index(0), "000", "encode 0");
    expect_eq(merkle::tiles::encode_tile_index(5), "005", "encode 5");
    expect_eq(merkle::tiles::encode_tile_index(255), "255", "encode 255");
    expect_eq(merkle::tiles::encode_tile_index(999), "999", "encode 999");
    expect_eq(
      merkle::tiles::encode_tile_index(1000), "x001/000", "encode 1000");
    expect_eq(
      merkle::tiles::encode_tile_index(1234067),
      "x001/x234/067",
      "encode 1234067");

    merkle::tiles::TileStore store(dir);

    // 2. Resource path layout (full tiles and bundles only).
    expect_eq(
      rel(store, store.tile_path(TileRef{0, 0})),
      "tile/0/000",
      "tile_path L0 N0");
    expect_eq(
      rel(store, store.tile_path(TileRef{1, 1234067})),
      "tile/1/x001/x234/067",
      "tile_path L1 big index");
    expect_eq(
      rel(store, store.entries_path(5)), "tile/entries/005", "entries full");

    // 2b. Every newly-created directory link is followed by a sync of its
    // parent, from the first missing ancestor through the final directory.
    {
      const fs::path nested = dir / "durable" / "tile" / "1";
      const auto synced =
        TileStoreProbe::create_and_record_directory_syncs(nested);
      const std::vector<fs::path> expected = {
        dir.parent_path(), dir, dir / "durable", dir / "durable" / "tile"};
      expect(synced == expected, "new directory parents synced in order");
      expect(fs::is_directory(nested), "nested directories created");
      expect(
        TileStoreProbe::create_and_record_directory_syncs(nested).empty(),
        "existing directories need no creation sync");
    }

    // 2c. A successful write must make progress. This shared guard prevents
    // the Windows WriteFile loop from spinning if it reports zero bytes.
    {
      bool zero_threw = false;
      try
      {
        TileStoreProbe::check_write_progress(0);
      }
      catch (const std::exception&)
      {
        zero_threw = true;
      }
      expect(zero_threw, "zero-byte write rejected");
      TileStoreProbe::check_write_progress(1);
    }

    const size_t hsz = Hash().size();

    // 3a. Full tile byte round-trip.
    const auto full = make_hashes(merkle::tiles::TILE_WIDTH);
    const TileRef full_ref{0, 0};
    store.write_tile(full_ref, full);
    expect(store.has_full_tile(0, 0), "has_full_tile after write");
    expect(!store.has_full_tile(0, 5), "missing full tile");
    expect(
      fs::file_size(store.tile_path(full_ref)) ==
        (uintmax_t)merkle::tiles::TILE_WIDTH * hsz,
      "full tile file size");
    const auto full_rt = store.read_tile(full_ref);
    expect(full_rt.size() == full.size(), "full tile width round-trip");
    for (size_t i = 0; i < full.size(); i++)
    {
      expect(full_rt[i] == full[i], "full tile hash round-trip");
    }

    // 3b. Wrong-width writes are rejected (only 256-wide tiles are valid).
    const std::vector<Hash> three(full.begin(), full.begin() + 3);
    bool threw = false;
    try
    {
      store.write_tile(TileRef{0, 2}, three); // 3 hashes for a full tile
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    expect(threw, "width mismatch rejected");

    // 3c. Corrupt / truncated files are rejected on read (integrity check), so
    // a torn write can never be served as a valid tile or bundle.
    {
      const auto expect_throws =
        [](const std::function<void()>& fn, const std::string& what) {
          bool t = false;
          try
          {
            fn();
          }
          catch (const std::exception&)
          {
            t = true;
          }
          expect(t, what);
        };

      // Truncated tile: fewer bytes than a full tile.
      overwrite_file(store.tile_path(full_ref), std::vector<uint8_t>(hsz, 0));
      expect(!store.has_full_tile(0, 0), "truncated tile is not durable");
      expect_throws(
        [&] { (void)store.read_tile(full_ref); }, "truncated tile rejected");
      store.write_tile(full_ref, full);
      expect(store.has_full_tile(0, 0), "truncated tile rewritten");
      expect(store.read_tile(full_ref) == full, "rewritten tile round-trip");

      // Oversized tile: more bytes than a full tile.
      overwrite_file(
        store.tile_path(full_ref),
        std::vector<uint8_t>((merkle::tiles::TILE_WIDTH + 1) * hsz, 0));
      expect(!store.has_full_tile(0, 0), "oversized tile is not durable");
      expect_throws(
        [&] { (void)store.read_tile(full_ref); }, "oversized tile rejected");

      // A valid full entry bundle whose file is then cut short.
      std::vector<std::vector<uint8_t>> entries(merkle::tiles::TILE_WIDTH);
      for (size_t i = 0; i < entries.size(); i++)
      {
        entries[i] = {(uint8_t)i, 0x7F};
      }
      store.write_entry_bundle(0, entries);
      expect(store.has_entry_bundle(0), "bundle durable before truncation");
      expect(
        store.read_entry_bundle(0).size() == merkle::tiles::TILE_WIDTH,
        "bundle valid before truncation");
      // Claims a 5-byte entry but supplies only one trailing byte.
      overwrite_file(store.entries_path(0), {0x00, 0x05, 0x01});
      expect(!store.has_entry_bundle(0), "truncated bundle is not durable");
      expect_throws(
        [&] { (void)store.read_entry_bundle(0); },
        "truncated entry bundle rejected");

      // A syntactically complete bundle with trailing bytes is not durable.
      auto encoded = merkle::tiles::TileStore::encode_entries(entries);
      encoded.push_back(0xFF);
      overwrite_file(store.entries_path(0), encoded);
      expect(!store.has_entry_bundle(0), "trailing bundle is not durable");
      expect_throws(
        [&] { (void)store.read_entry_bundle(0); },
        "trailing entry bundle rejected");

      // decode_entries directly: a length prefix that overruns the buffer.
      expect_throws(
        [] {
          (void)merkle::tiles::TileStore::decode_entries({0xFF, 0xFF, 0x00}, 1);
        },
        "decode_entries oversized length rejected");
      expect_throws(
        [] {
          (void)merkle::tiles::TileStore::decode_entries({0x00, 0x00, 0xFF}, 1);
        },
        "decode_entries trailing bytes rejected");
    }

// Windows file replacement semantics can reject racing same-path replaces even
// when the final tile remains valid, so keep this stress check POSIX-only.
#ifndef _WIN32
    // 3d. Concurrent same-tile writes use unique temp files and leave no
    // temporary files behind after success.
    {
      const TileRef concurrent_ref{0, 42};
      std::atomic<bool> ok{true};
      std::vector<std::thread> threads;
      for (size_t i = 0; i < 8; i++)
      {
        threads.emplace_back([&] {
          try
          {
            store.write_tile(concurrent_ref, full);
          }
          catch (...)
          {
            ok = false;
          }
        });
      }
      for (auto& thread : threads)
      {
        thread.join();
      }
      expect(ok, "concurrent writes succeeded");
      expect(store.read_tile(concurrent_ref) == full, "concurrent tile valid");
      expect(!any_tmp_files(dir), "no temp files left after writes");
    }
#endif

    std::cout << "tiles_store: OK" << '\n';

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
