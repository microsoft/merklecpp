// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tiles_test_util.h"
#include "util.h"

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
using merkle::tiles::EntryBundleWriter;
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

static void overwrite_file(const fs::path& p, const std::vector<uint8_t>& bytes)
{
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(
    reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
}

int main()
{
  const TemporaryDirectory temporary_directory("merklecpp_tiles_entries");
  const fs::path& base = temporary_directory.path();

  try
  {
    // 1. Encode/decode round-trip with variable-length entries.
    {
      const std::vector<std::vector<uint8_t>> entries = {
        {}, {0x01}, {0x02, 0x03, 0x04}, std::vector<uint8_t>(300, 0xAB)};
      const auto enc = TileStore::encode_entries(entries);
      // 4 length prefixes (8 bytes) + 0 + 1 + 3 + 300 payload bytes.
      expect(enc.size() == 8 + 0 + 1 + 3 + 300, "encoded size");
      const auto dec = TileStore::decode_entries(enc, entries.size());
      expect(dec == entries, "encode/decode round-trip");

      auto trailing = enc;
      trailing.push_back(0xFF);
      bool trailing_threw = false;
      try
      {
        (void)TileStore::decode_entries(trailing, entries.size());
      }
      catch (const std::exception&)
      {
        trailing_threw = true;
      }
      expect(trailing_threw, "decode rejects trailing bytes");
    }

    const auto hashes = make_hashes(70000);
    const auto entry_at = [&](uint64_t i) -> std::vector<uint8_t> {
      return hashes[i]; // HashT -> vector<uint8_t>
    };

    // 2. size 256: a single full bundle, no tail.
    {
      TileStore store(base / "e256");
      EntryBundleWriter writer(store);
      const auto s = writer.write_up_to(256, entry_at);
      expect(s.full_written == 1, "256 counts");
      expect(store.has_entry_bundle(0), "256 full bundle");
      expect(!store.has_entry_bundle(1), "256 no second bundle");

      const auto b0 = store.read_entry_bundle(0);
      expect(b0.size() == 256, "256 bundle width");
      for (size_t i = 0; i < 256; i++)
      {
        expect(Hash(b0[i]) == hashes[i], "256 entry round-trip");
      }
    }

    // 3. size 70000: 273 full bundles (the 112-entry tail is not bundled),
    // plus tile linkage.
    {
      TileStore store(base / "e70k");
      EntryBundleWriter writer(store);
      const auto s = writer.write_up_to(70000, entry_at);
      expect(s.full_written == 273, "70000 counts");
      expect(store.has_entry_bundle(0), "70000 bundle 0");
      expect(store.has_entry_bundle(272), "70000 bundle 272");
      expect(!store.has_entry_bundle(273), "70000 no bundle 273");

      // Level-0 tile entries (leaf hashes) correspond to the bundle entries
      // under the identity leaf-hash used here.
      TileWriter tw(store);
      const auto leaf_at = [&](uint64_t i) -> const Hash& { return hashes[i]; };
      tw.write_up_to(70000, leaf_at);
      const auto tile0 = store.read_tile(TileRef{0, 0});
      const auto bundle0 = store.read_entry_bundle(0);
      for (size_t i = 0; i < 256; i++)
      {
        expect(Hash(bundle0[i]) == tile0[i], "bundle<->tile linkage");
      }
    }

    // 4. Incremental writes: full bundles are immutable, the tail is not
    // bundled.
    {
      TileStore store(base / "einc");
      EntryBundleWriter writer(store);
      expect(writer.write_up_to(256, entry_at).full_written == 1, "inc s1");
      const auto s2 = writer.write_up_to(256, entry_at);
      expect(s2.full_written == 0, "inc immutable rerun");
      overwrite_file(store.entries_path(0), {0x00, 0x05, 0x01});
      expect(!store.has_entry_bundle(0), "inc corrupt bundle not durable");
      EntryBundleWriter resumed(store);
      expect(
        resumed.write_up_to(256, entry_at).full_written == 1,
        "inc rewrite corrupt");
      expect(store.has_entry_bundle(0), "inc rewritten bundle durable");
      const auto s3 = writer.write_up_to(600, entry_at);
      expect(s3.full_written == 1, "inc s3 full");
      expect(store.has_entry_bundle(1), "inc bundle 1");
      expect(!store.has_entry_bundle(2), "inc no tail bundle");
    }

    // 5. Resume repairs an interior malformed bundle even when later files are
    // valid.
    {
      const fs::path dir = base / "eholes";
      {
        TileStore store(dir);
        EntryBundleWriter writer(store);
        expect(
          writer.write_up_to(2048, entry_at).full_written == 8,
          "holes initial bundles");
        overwrite_file(store.entries_path(3), {0x00, 0x05, 0x01});
        expect(!store.has_entry_bundle(3), "holes interior bundle malformed");
        expect(store.has_entry_bundle(7), "holes later bundle remains valid");
      }

      TileStore store(dir);
      EntryBundleWriter writer(store);
      expect(
        writer.write_up_to(2048, entry_at).full_written == 1,
        "holes rewrites interior bundle");
      std::vector<std::vector<uint8_t>> expected;
      expected.reserve(merkle::tiles::TILE_WIDTH);
      constexpr uint64_t hole_begin = (uint64_t)merkle::tiles::TILE_WIDTH * 3;
      constexpr uint64_t hole_end = (uint64_t)merkle::tiles::TILE_WIDTH * 4;
      for (uint64_t i = hole_begin; i < hole_end; i++)
      {
        expected.push_back(entry_at(i));
      }
      expect(
        store.read_entry_bundle(3) == expected,
        "holes repaired bundle contents");
    }

    // 6. Recovery examines only bundle indices relevant to the requested size.
    {
      const fs::path dir = base / "esparse";
      const std::vector<std::vector<uint8_t>> bundle(
        merkle::tiles::TILE_WIDTH, {0x42});
      {
        TileStore store(dir);
        store.write_entry_bundle(0, bundle);
        for (uint64_t index = 1;; index <<= 1)
        {
          store.write_entry_bundle(index, bundle);
          if (index == (uint64_t{1} << 63))
          {
            break;
          }
        }
      }

      TileStore store(dir);
      EntryBundleWriter writer(store);
      expect(
        writer.write_up_to(merkle::tiles::TILE_WIDTH, entry_at).full_written ==
          0,
        "sparse bounded recovery");
      expect(
        store.has_entry_bundle(0), "sparse requested bundle remains valid");
    }

    std::cout << "tiles_entries: OK" << '\n';
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
