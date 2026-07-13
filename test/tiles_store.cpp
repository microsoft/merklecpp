// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
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

static void expect_throws(
  const std::function<void()>& fn, const std::string& what)
{
  bool threw = false;
  try
  {
    fn();
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  expect(threw, what);
}

static void custom_hash(const Hash& lhs, const Hash& rhs, Hash& out)
{
  (void)rhs;
  out = lhs;
}

template <typename Store>
static std::string rel(const Store& store, const fs::path& p)
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

  static void check_write_progress(size_t written)
  {
    Store::require_write_progress(written, "test");
  }

  static uint64_t current_process_id()
  {
    return Store::process_id();
  }
};

struct SyncFault
{
  fs::path fail_path;
  size_t failures_remaining = 0;
  size_t matching_calls_before_failure = 0;
  std::vector<fs::path> calls;

  void sync(const fs::path& path)
  {
    const auto normalised = path.lexically_normal();
    calls.push_back(normalised);
    if (failures_remaining > 0 && normalised == fail_path.lexically_normal())
    {
      if (matching_calls_before_failure > 0)
      {
        matching_calls_before_failure--;
        return;
      }
      failures_remaining--;
      throw std::runtime_error("injected directory sync failure");
    }
  }

  [[nodiscard]] size_t call_count(const fs::path& path) const
  {
    const auto normalised = path.lexically_normal();
    return (size_t)std::count(calls.begin(), calls.end(), normalised);
  }
};

class FaultInjectingTileStore : public merkle::tiles::TileStore
{
public:
  using Store = merkle::tiles::TileStore;

  FaultInjectingTileStore(
    fs::path prefix, const std::shared_ptr<SyncFault>& fault) :
    Store(
      std::move(prefix), [fault](const fs::path& path) { fault->sync(path); })
  {}

  void begin_attempt()
  {
    Store::begin_write_attempt();
  }

  [[nodiscard]] bool confirm_tile(uint8_t level, uint64_t index)
  {
    return Store::confirm_full_tile(level, index);
  }

  [[nodiscard]] bool confirm_bundle(uint64_t index)
  {
    return Store::confirm_entry_bundle(index);
  }
};

// Overwrites a file with exactly the given bytes (to simulate corruption).
static void overwrite_file(const fs::path& p, const std::vector<uint8_t>& bytes)
{
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(
    reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
}

static std::vector<uint8_t> read_file(const fs::path& p)
{
  std::ifstream f(p, std::ios::binary);
  if (!f.good())
  {
    throw std::runtime_error("cannot open test file: " + p.string());
  }
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main()
{
  const auto nonce =
    std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() /
    ("merklecpp_tiles_" +
     std::to_string(
       static_cast<unsigned long long>(TileStoreProbe::current_process_id())) +
     "_" + std::to_string(static_cast<unsigned long long>(nonce)));

  try
  {
    // 1. Tile geometry, references, and index encoding.
    expect(
      merkle::tiles::TILE_WIDTH == (1U << merkle::tiles::TILE_HEIGHT),
      "tile width matches tile height");
    const TileRef default_ref;
    expect(
      default_ref.level == 0 && default_ref.index == 0,
      "TileRef defaults to the first leaf tile");

    size_t prefix_probes = 0;
    const auto prefix_length =
      merkle::tiles::detail::contiguous_prefix_length(10, [&](uint64_t index) {
        prefix_probes++;
        return index < 3;
      });
    expect(
      prefix_length == 3 && prefix_probes == 4,
      "contiguous prefix stops at the first gap");
    prefix_probes = 0;
    const auto empty_prefix_length =
      merkle::tiles::detail::contiguous_prefix_length(0, [&](uint64_t) {
        prefix_probes++;
        return true;
      });
    expect(
      empty_prefix_length == 0 && prefix_probes == 0,
      "empty contiguous prefix performs no probes");

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
    expect_eq(
      merkle::tiles::encode_tile_index(std::numeric_limits<uint64_t>::max()),
      "x018/x446/x744/x073/x709/x551/615",
      "encode uint64 max");
    expect_eq(
      merkle::tiles::TileStore::encode_index(1000),
      "x001/000",
      "store index encoder");

    merkle::tiles::TileStore store(dir);
    const size_t hsz = Hash().size();
    const auto full = make_hashes(merkle::tiles::TILE_WIDTH);

    // 2. Resource path layout (full tiles and bundles only).
    expect_eq(
      store.root().lexically_relative(dir).generic_string(),
      "sha256-256w",
      "SHA256 storage directory");
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
    expect_eq(
      merkle::tiles::TileStore::storage_directory_name("sha384"),
      "sha384-256w",
      "SHA384 retains 256-hash tile width");
    expect_eq(
      merkle::tiles::TileStore::storage_directory_name("sha3-256"),
      "sha3-256-256w",
      "custom algorithm storage directory");
    for (const std::string& invalid_name :
         {"", "-sha256", "sha256-", "SHA256", "sha_256", "sha/256"})
    {
      expect_throws(
        [&] {
          (void)merkle::tiles::TileStore::storage_directory_name(invalid_name);
        },
        "invalid algorithm short name rejected");
    }
    for (const std::string& mismatched_name : {"sha384", "sha512"})
    {
      expect_throws(
        [&] { (void)merkle::tiles::TileStore(dir, mismatched_name); },
        "algorithm short name must match hash output size");
    }

    using CustomStore = merkle::tiles::TileStoreT<32, custom_hash>;
    expect_throws(
      [&] { (void)CustomStore(dir); },
      "custom hash requires an explicit algorithm name");
    const CustomStore custom_store(dir, "custom-hash");
    expect_eq(
      custom_store.root().lexically_relative(dir).generic_string(),
      "custom-hash-256w",
      "explicit custom hash storage directory");

#ifdef HAVE_OPENSSL
    {
      using TileStore256 =
        merkle::tiles::TileStoreT<32, merkle::sha256_openssl>;
      const TileStore256 store256(dir);
      expect_eq(
        store256.root().lexically_relative(dir).generic_string(),
        "sha256-256w",
        "OpenSSL SHA256 storage directory");

      using TileStore384 =
        merkle::tiles::TileStoreT<48, merkle::sha384_openssl>;
      TileStore384 store384(dir);
      const auto full384 = make_hashesT<48>(merkle::tiles::TILE_WIDTH);
      expect_eq(
        store384.root().lexically_relative(dir).generic_string(),
        "sha384-256w",
        "SHA384 storage directory");
      store384.write_tile(TileRef{0, 0}, full384);
      expect(
        fs::file_size(store384.tile_path(TileRef{0, 0})) ==
          (uintmax_t)merkle::tiles::TILE_WIDTH * 48,
        "SHA384 full tile remains 256 hashes wide");
      expect(
        store384.has_full_tile(0, 0) &&
          store384.read_tile(TileRef{0, 0}) == full384,
        "SHA384 tile round-trip");

      using TileStore512 =
        merkle::tiles::TileStoreT<64, merkle::sha512_openssl>;
      TileStore512 store512(dir);
      const auto full512 = make_hashesT<64>(merkle::tiles::TILE_WIDTH);
      expect_eq(
        store512.root().lexically_relative(dir).generic_string(),
        "sha512-256w",
        "SHA512 storage directory");
      store512.write_tile(TileRef{0, 0}, full512);
      expect(
        fs::file_size(store512.tile_path(TileRef{0, 0})) ==
          (uintmax_t)merkle::tiles::TILE_WIDTH * 64,
        "SHA512 full tile remains 256 hashes wide");
      expect(
        store512.has_full_tile(0, 0) &&
          store512.read_tile(TileRef{0, 0}) == full512,
        "SHA512 tile round-trip");
    }
#endif

    // 2b. Production writes sync each directory link and the destination
    // directory in order.
    {
      const fs::path prefix = dir / "durable";
      const auto fault = std::make_shared<SyncFault>();
      FaultInjectingTileStore durable_store(prefix, fault);
      durable_store.write_tile(TileRef{1, 0}, full);
      const std::vector<fs::path> expected = {
        dir.parent_path(),
        dir,
        prefix,
        prefix / "sha256-256w",
        prefix / "sha256-256w" / "tile",
        prefix / "sha256-256w" / "tile" / "1"};
      expect(
        fault->calls.size() >= expected.size() &&
          std::equal(
            expected.begin(),
            expected.end(),
            fault->calls.end() - (std::ptrdiff_t)expected.size()),
        "directory parents and destination synced in order");
    }

    // 2c. A failed directory-link sync is retried even when the directory
    // created before the failure is already visible.
    {
      const fs::path prefix = dir / "directory_retry";
      const auto fault = std::make_shared<SyncFault>();
      fault->fail_path = prefix;
      fault->failures_remaining = 1;
      FaultInjectingTileStore retry_store(prefix, fault);
      const TileRef retry_ref{2, 7};
      expect_throws(
        [&] { retry_store.write_tile(retry_ref, full); },
        "directory sync failure propagated");
      expect(
        !retry_store.has_full_tile(retry_ref.level, retry_ref.index),
        "tile not published before directory chain is durable");
      retry_store.write_tile(retry_ref, full);
      expect(
        retry_store.read_tile(retry_ref) == full,
        "write succeeds after directory sync retry");
      expect(
        fault->call_count(prefix) == 2, "failed directory link sync retried");
    }

    // 2d. A failure syncing the destination directory leaves a complete,
    // visible file that can be re-confirmed on the next write attempt.
    {
      const fs::path prefix = dir / "publication_retry";
      const auto fault = std::make_shared<SyncFault>();
      FaultInjectingTileStore retry_store(prefix, fault);
      const TileRef retry_ref{3, 9};
      const auto tile_directory =
        retry_store.tile_path(retry_ref).parent_path();
      fault->fail_path = tile_directory;
      fault->failures_remaining = 1;
      expect_throws(
        [&] { retry_store.write_tile(retry_ref, full); },
        "publication directory sync failure propagated");
      expect(
        retry_store.has_full_tile(retry_ref.level, retry_ref.index) &&
          retry_store.read_tile(retry_ref) == full,
        "complete tile remains visible after publication sync failure");
      expect(
        !any_tmp_files(prefix),
        "publication sync failure leaves no temporary file");

      const size_t calls_before_confirm = fault->call_count(tile_directory);
      retry_store.begin_attempt();
      expect(
        retry_store.confirm_tile(retry_ref.level, retry_ref.index),
        "visible tile durability re-confirmed");
      expect(
        fault->call_count(tile_directory) == calls_before_confirm + 1,
        "destination directory sync retried");
      expect(
        retry_store.confirm_tile(retry_ref.level, retry_ref.index),
        "confirmed tile remains reusable");
      expect(
        fault->call_count(tile_directory) == calls_before_confirm + 1,
        "directory sync cached within one attempt");

      retry_store.begin_attempt();
      expect(
        !retry_store.confirm_tile(retry_ref.level, retry_ref.index + 1),
        "missing tile cannot be confirmed");
    }

    // 2e. Failures before atomic replacement clean up temporary files, and a
    // non-directory in the destination path is rejected.
    {
      const fs::path prefix = dir / "replacement_failure";
      merkle::tiles::TileStore failure_store(prefix);
      const TileRef blocked_ref{0, 1};
      fs::create_directories(failure_store.tile_path(blocked_ref));
      expect_throws(
        [&] { failure_store.write_tile(blocked_ref, full); },
        "replacement of a directory rejected");
      expect(
        !any_tmp_files(prefix),
        "failed atomic replacement cleans up temporary file");
    }
    {
      const fs::path prefix = dir / "directory_conflict";
      fs::create_directories(prefix);
      overwrite_file(prefix / "sha256-256w", {0x00});
      merkle::tiles::TileStore failure_store(prefix);
      expect_throws(
        [&] { failure_store.write_tile(TileRef{0, 0}, full); },
        "non-directory storage root rejected");
      expect(
        !any_tmp_files(prefix),
        "directory creation failure leaves no temporary file");
    }

    // 2f. A successful write must make progress. This shared guard prevents
    // the Windows WriteFile loop from spinning if it reports zero bytes.
    {
      expect_throws(
        [] { TileStoreProbe::check_write_progress(0); },
        "zero-byte write rejected");
      TileStoreProbe::check_write_progress(1);
    }

    // 3a. Full tile byte round-trip.
    const TileRef full_ref{0, 0};
    store.write_tile(full_ref, full);
    expect(store.has_full_tile(0, 0), "has_full_tile after write");
    expect(!store.has_full_tile(0, 5), "missing full tile");
    expect_throws(
      [&] { (void)store.read_tile(TileRef{0, 5}); },
      "missing tile read rejected");
    expect(
      fs::file_size(store.tile_path(full_ref)) ==
        (uintmax_t)merkle::tiles::TILE_WIDTH * hsz,
      "full tile file size");
    std::vector<uint8_t> expected_tile_bytes;
    expected_tile_bytes.reserve(full.size() * hsz);
    for (const auto& hash : full)
    {
      hash.serialise(expected_tile_bytes);
    }
    expect(
      read_file(store.tile_path(full_ref)) == expected_tile_bytes,
      "tile file contains concatenated raw hashes");
    const auto full_rt = store.read_tile(full_ref);
    expect(full_rt.size() == full.size(), "full tile width round-trip");
    for (size_t i = 0; i < full.size(); i++)
    {
      expect(full_rt[i] == full[i], "full tile hash round-trip");
    }

    // 3b. Wrong-width writes are rejected (only 256-wide tiles are valid).
    const std::vector<Hash> three(full.begin(), full.begin() + 3);
    expect_throws(
      [&] { store.write_tile(TileRef{0, 2}, three); },
      "width mismatch rejected");

    // 4. Entry bundles preserve empty, short, and multi-byte-length entries,
    // enforce their width and uint16 length limits, and can be re-confirmed.
    std::vector<std::vector<uint8_t>> entries(merkle::tiles::TILE_WIDTH);
    entries[1] = {0xA5};
    entries[2] = std::vector<uint8_t>(256, 0x5A);
    for (size_t i = 3; i < entries.size(); i++)
    {
      entries[i] = {(uint8_t)i, 0x7F};
    }
    const auto encoded_entries =
      merkle::tiles::TileStore::encode_entries(entries);
    expect(
      encoded_entries.size() > 7 && encoded_entries[0] == 0x00 &&
        encoded_entries[1] == 0x00 && encoded_entries[2] == 0x00 &&
        encoded_entries[3] == 0x01 && encoded_entries[4] == 0xA5 &&
        encoded_entries[5] == 0x01 && encoded_entries[6] == 0x00,
      "entry lengths use big-endian uint16 prefixes");
    expect(
      merkle::tiles::TileStore::decode_entries(
        encoded_entries, merkle::tiles::TILE_WIDTH) == entries,
      "entry codec round-trip");
    expect(
      merkle::tiles::TileStore::decode_entries({}, 0).empty(),
      "empty entry sequence round-trip");

    const std::vector<uint8_t> maximum_entry(0xFFFF, 0xC3);
    const auto maximum_encoded =
      merkle::tiles::TileStore::encode_entries({maximum_entry});
    const auto maximum_decoded =
      merkle::tiles::TileStore::decode_entries(maximum_encoded, 1);
    expect(
      maximum_encoded.size() == maximum_entry.size() + 2 &&
        maximum_encoded[0] == 0xFF && maximum_encoded[1] == 0xFF &&
        maximum_decoded.size() == 1 && maximum_decoded[0] == maximum_entry,
      "maximum uint16-sized entry round-trip");
    expect_throws(
      [] {
        (void)merkle::tiles::TileStore::encode_entries(
          {std::vector<uint8_t>(0x10000)});
      },
      "oversized entry rejected");

    expect(!store.has_entry_bundle(17), "missing entry bundle");
    expect_throws(
      [&] { (void)store.read_entry_bundle(17); },
      "missing entry bundle read rejected");
    const std::vector<std::vector<uint8_t>> short_bundle(
      entries.begin(), entries.end() - 1);
    expect_throws(
      [&] { store.write_entry_bundle(1, short_bundle); },
      "short entry bundle rejected");
    auto oversized_bundle = entries;
    oversized_bundle[0].resize(0x10000);
    expect_throws(
      [&] { store.write_entry_bundle(2, oversized_bundle); },
      "entry bundle containing oversized entry rejected");
    expect(
      !fs::exists(store.entries_path(1)) && !fs::exists(store.entries_path(2)),
      "invalid entry bundles are not published");

    store.write_entry_bundle(0, entries);
    expect(store.has_entry_bundle(0), "full entry bundle exists");
    expect(
      store.read_entry_bundle(0) == entries,
      "full entry bundle byte round-trip");
    expect(
      fs::file_size(store.entries_path(0)) == encoded_entries.size(),
      "entry bundle file size");
    expect(
      read_file(store.entries_path(0)) == encoded_entries,
      "entry bundle file uses the encoded wire format");

    {
      const fs::path prefix = dir / "bundle_confirmation";
      const auto fault = std::make_shared<SyncFault>();
      FaultInjectingTileStore confirming_store(prefix, fault);
      confirming_store.write_entry_bundle(4, entries);
      const auto entries_directory =
        confirming_store.entries_path(4).parent_path();
      const size_t calls_before_confirm = fault->call_count(entries_directory);
      confirming_store.begin_attempt();
      expect(
        confirming_store.confirm_bundle(4),
        "visible entry bundle durability confirmed");
      expect(
        fault->call_count(entries_directory) == calls_before_confirm + 1,
        "entry bundle directory synced for new attempt");
      expect(
        confirming_store.confirm_bundle(4),
        "confirmed entry bundle remains reusable");
      expect(
        fault->call_count(entries_directory) == calls_before_confirm + 1,
        "entry bundle sync cached within one attempt");
      confirming_store.begin_attempt();
      expect(
        !confirming_store.confirm_bundle(5),
        "missing entry bundle cannot be confirmed");
    }

    // 5. Corrupt / truncated files are rejected on read (integrity check), so
    // a torn write can never be served as a valid tile or bundle.
    {
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
      store.write_entry_bundle(0, entries);
      expect(store.has_entry_bundle(0), "bundle durable before truncation");
      expect(
        store.read_entry_bundle(0) == entries,
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
        [] { (void)merkle::tiles::TileStore::decode_entries({}, 1); },
        "decode_entries missing length prefix rejected");
      expect_throws(
        [] {
          (void)merkle::tiles::TileStore::decode_entries({0x00, 0x00, 0xFF}, 1);
        },
        "decode_entries trailing bytes rejected");
    }

// Windows file replacement semantics can reject racing same-path replaces even
// when the final tile remains valid, so keep this stress check POSIX-only.
#ifndef _WIN32
    // 6. Concurrent same-tile writes use unique temp files and leave no
    // temporary files behind after success. Each thread owns its store object;
    // sharing one object requires caller-provided synchronization.
    {
      const TileRef concurrent_ref{0, 42};
      std::atomic<bool> ok{true};
      std::vector<std::thread> threads;
      for (size_t i = 0; i < 8; i++)
      {
        threads.emplace_back([&] {
          try
          {
            merkle::tiles::TileStore thread_store(dir);
            thread_store.write_tile(concurrent_ref, full);
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
