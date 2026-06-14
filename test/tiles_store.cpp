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

    // 2. Resource path layout.
    expect_eq(
      rel(store, store.tile_path(TileRef{0, 0, 0})),
      "tile/0/000",
      "tile_path L0 N0 full");
    expect_eq(
      rel(store, store.tile_path(TileRef{0, 273, 112})),
      "tile/0/273.p/112",
      "tile_path L0 N273 partial");
    expect_eq(
      rel(store, store.tile_path(TileRef{1, 1234067, 0})),
      "tile/1/x001/x234/067",
      "tile_path L1 big index");
    expect_eq(
      rel(store, store.entries_path(5)), "tile/entries/005", "entries full");
    expect_eq(
      rel(store, store.entries_path(5, 3)),
      "tile/entries/005.p/3",
      "entries partial");

    const size_t hsz = Hash().size();

    // 3a. Full tile byte round-trip.
    const auto full = make_hashes(merkle::tiles::TILE_WIDTH);
    const TileRef full_ref{0, 0, 0};
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

    // 3b. Partial tile byte round-trip.
    const std::vector<Hash> partial(full.begin(), full.begin() + 3);
    const TileRef partial_ref{0, 1, 3};
    store.write_tile(partial_ref, partial);
    expect(
      fs::file_size(store.tile_path(partial_ref)) == (uintmax_t)3 * hsz,
      "partial tile file size");
    const auto partial_rt = store.read_tile(partial_ref);
    expect(partial_rt.size() == 3, "partial tile width round-trip");
    for (size_t i = 0; i < partial.size(); i++)
    {
      expect(partial_rt[i] == partial[i], "partial tile hash round-trip");
    }

    // 3c. Width mismatches are rejected.
    bool threw = false;
    try
    {
      store.write_tile(TileRef{0, 2, 0}, partial); // 3 hashes for a full tile
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    expect(threw, "width mismatch rejected");

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
