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

int main()
{
  const auto seed = std::time(nullptr);
  std::srand((unsigned)seed);
  std::cout << "seed=" << seed << '\n';

  const fs::path base = fs::temp_directory_path() /
    ("merklecpp_tiles_entries_" + std::to_string((unsigned long long)seed) +
     "_" + std::to_string(std::rand()));

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
      const auto s3 = writer.write_up_to(600, entry_at);
      expect(s3.full_written == 1, "inc s3 full");
      expect(store.has_entry_bundle(1), "inc bundle 1");
      expect(!store.has_entry_bundle(2), "inc no tail bundle");
    }

    std::cout << "tiles_entries: OK" << '\n';

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
