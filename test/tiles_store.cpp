// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <merklecpp.h>
#include <merklecpp_pal.h>
#include <merklecpp_tiles.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::TileRef;

class TemporaryDirectory
{
private:
  fs::path path_;

public:
  TemporaryDirectory()
  {
    static std::atomic<uint64_t> sequence = 0;
    const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
      std::format(
              "merklecpp_tiles_{}_{}_{}",
              merkle::pal::process_id(),
              nonce,
              sequence++);
  }

  ~TemporaryDirectory()
  {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const fs::path& path() const
  {
    return path_;
  }
};

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
    throw std::runtime_error(
      std::format("cannot open test file: {}", p.string()));
  }
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static std::vector<std::vector<uint8_t>> make_entries()
{
  std::vector<std::vector<uint8_t>> entries(merkle::tiles::TILE_WIDTH);
  entries[1] = {0xA5};
  entries[2] = std::vector<uint8_t>(256, 0x5A);
  for (size_t i = 3; i < entries.size(); i++)
  {
    entries[i] = {(uint8_t)i, 0x7F};
  }
  return entries;
}

TEST_CASE("Tile geometry, references, and index encoding")
{
  // 1. Tile geometry, references, and index encoding.
  CHECK(merkle::tiles::TILE_WIDTH == (1U << merkle::tiles::TILE_HEIGHT));

  const TileRef default_ref;
  CHECK(default_ref.level == 0);
  CHECK(default_ref.index == 0);

  size_t prefix_probes = 0;
  const auto prefix_length =
    merkle::tiles::detail::contiguous_prefix_length(10, [&](uint64_t index) {
      prefix_probes++;
      return index < 3;
    });
  CHECK(prefix_length == 3);
  CHECK(prefix_probes == 4);

  prefix_probes = 0;
  const auto empty_prefix_length =
    merkle::tiles::detail::contiguous_prefix_length(0, [&](uint64_t) {
      prefix_probes++;
      return true;
    });
  CHECK(empty_prefix_length == 0);
  CHECK(prefix_probes == 0);

  CHECK(merkle::tiles::encode_tile_index(0) == "000");
  CHECK(merkle::tiles::encode_tile_index(5) == "005");
  CHECK(merkle::tiles::encode_tile_index(255) == "255");
  CHECK(merkle::tiles::encode_tile_index(999) == "999");
  CHECK(merkle::tiles::encode_tile_index(1000) == "x001/000");
  CHECK(merkle::tiles::encode_tile_index(1234067) == "x001/x234/067");
  CHECK(
    merkle::tiles::encode_tile_index(std::numeric_limits<uint64_t>::max()) ==
    "x018/x446/x744/x073/x709/x551/615");
  CHECK(merkle::tiles::TileStore::encode_index(1000) == "x001/000");
}

TEST_CASE("Tile store paths and hash namespaces")
{
  // 2. Resource path layout (full tiles and bundles only).
  const TemporaryDirectory temporary_directory;
  const fs::path& dir = temporary_directory.path();
  const merkle::tiles::TileStore store(dir);

  CHECK(store.root().lexically_relative(dir).generic_string() == "sha256-256w");
  CHECK(rel(store, store.tile_path(TileRef{0, 0})) == "tile/0/000");
  CHECK(
    rel(store, store.tile_path(TileRef{1, 1234067})) == "tile/1/x001/x234/067");
  CHECK(
    rel(store, store.tile_path(TileRef{merkle::tiles::MAX_TILE_LEVEL, 0})) ==
    "tile/63/000");
  CHECK_THROWS_AS(
    ((void)store.tile_path(
      TileRef{static_cast<uint8_t>(merkle::tiles::MAX_TILE_LEVEL + 1), 0})),
    std::runtime_error);
  CHECK(rel(store, store.entries_path(5)) == "tile/entries/005");
  CHECK(
    merkle::tiles::TileStore::storage_directory_name("sha384") ==
    "sha384-256w");
  CHECK(
    merkle::tiles::TileStore::storage_directory_name("sha3-256") ==
    "sha3-256-256w");

  for (const std::string& invalid_name :
       {"", "-sha256", "sha256-", "SHA256", "sha_256", "sha/256"})
  {
    CAPTURE(invalid_name);
    CHECK_THROWS_AS(
      merkle::tiles::TileStore::storage_directory_name(invalid_name),
      std::runtime_error);
  }
  for (const std::string& mismatched_name : {"sha384", "sha512"})
  {
    CAPTURE(mismatched_name);
    CHECK_THROWS_AS(
      (merkle::tiles::TileStore(dir, mismatched_name)), std::runtime_error);
  }

  using CustomStore =
    merkle::tiles::TileStoreT<merkle::Hash::size_bytes, custom_hash>;
  CHECK_THROWS_AS((void)CustomStore(dir), std::runtime_error);
  const CustomStore custom_store(dir, "custom-hash");
  CHECK(
    custom_store.root().lexically_relative(dir).generic_string() ==
    "custom-hash-256w");

#ifdef HAVE_OPENSSL
  using TileStore256 =
    merkle::tiles::TileStoreT<merkle::Hash::size_bytes, merkle::sha256_openssl>;
  const TileStore256 store256(dir);
  CHECK(
    store256.root().lexically_relative(dir).generic_string() == "sha256-256w");

  using TileStore384 = merkle::tiles::TileStoreT<
    merkle::Tree384::Hash::size_bytes,
    merkle::Tree384::hash_function>;
  for (const std::string& mismatched_name : {"sha256", "sha512"})
  {
    CAPTURE(mismatched_name);
    CHECK_THROWS_AS((TileStore384(dir, mismatched_name)), std::runtime_error);
  }
  TileStore384 store384(dir);
  const auto full384 =
    make_hashesT<merkle::Tree384::Hash::size_bytes>(merkle::tiles::TILE_WIDTH);
  CHECK(
    store384.root().lexically_relative(dir).generic_string() == "sha384-256w");
  store384.write_tile(TileRef{0, 0}, full384);
  CHECK(
    fs::file_size(store384.tile_path(TileRef{0, 0})) ==
    (uintmax_t)merkle::tiles::TILE_WIDTH * merkle::Tree384::Hash::size_bytes);
  CHECK(store384.has_full_tile(0, 0));
  CHECK(store384.read_tile(TileRef{0, 0}) == full384);

  using TileStore512 = merkle::tiles::TileStoreT<
    merkle::Tree512::Hash::size_bytes,
    merkle::Tree512::hash_function>;
  for (const std::string& mismatched_name : {"sha256", "sha384"})
  {
    CAPTURE(mismatched_name);
    CHECK_THROWS_AS((TileStore512(dir, mismatched_name)), std::runtime_error);
  }
  TileStore512 store512(dir);
  const auto full512 =
    make_hashesT<merkle::Tree512::Hash::size_bytes>(merkle::tiles::TILE_WIDTH);
  CHECK(
    store512.root().lexically_relative(dir).generic_string() == "sha512-256w");
  store512.write_tile(TileRef{0, 0}, full512);
  CHECK(
    fs::file_size(store512.tile_path(TileRef{0, 0})) ==
    (uintmax_t)merkle::tiles::TILE_WIDTH * merkle::Tree512::Hash::size_bytes);
  CHECK(store512.has_full_tile(0, 0));
  CHECK(store512.read_tile(TileRef{0, 0}) == full512);
#endif
}

TEST_CASE("Tile writes sync directory links in order")
{
  // 2b. Production writes sync each directory link and the destination
  // directory in order.
  const TemporaryDirectory temporary_directory;
  const fs::path& dir = temporary_directory.path();
  const fs::path prefix = dir / "durable";
  const auto fault = std::make_shared<SyncFault>();
  FaultInjectingTileStore store(prefix, fault);
  store.write_tile(TileRef{1, 0}, make_hashes(merkle::tiles::TILE_WIDTH));

  const std::vector<fs::path> expected = {
    dir.parent_path(),
    dir,
    prefix,
    prefix / "sha256-256w",
    prefix / "sha256-256w" / "tile",
    prefix / "sha256-256w" / "tile" / "1"};
  REQUIRE(fault->calls.size() >= expected.size());
  CHECK(std::equal(
    expected.begin(),
    expected.end(),
    fault->calls.end() - (std::ptrdiff_t)expected.size()));
}

TEST_CASE("Failed directory-link sync is retried")
{
  // 2c. A failed directory-link sync is retried even when the directory
  // created before the failure is already visible.
  const TemporaryDirectory temporary_directory;
  const fs::path prefix = temporary_directory.path() / "directory_retry";
  const auto fault = std::make_shared<SyncFault>();
  fault->fail_path = prefix;
  fault->failures_remaining = 1;
  FaultInjectingTileStore store(prefix, fault);
  const TileRef ref{2, 7};
  const auto full = make_hashes(merkle::tiles::TILE_WIDTH);

  CHECK_THROWS_AS((store.write_tile(ref, full)), std::runtime_error);
  CHECK_FALSE(store.has_full_tile(ref.level, ref.index));
  store.write_tile(ref, full);
  CHECK(store.read_tile(ref) == full);
  CHECK(fault->call_count(prefix) == 2);
}

TEST_CASE("Visible publication can be confirmed after sync failure")
{
  // 2d. A failure syncing the destination directory leaves a complete,
  // visible file that can be re-confirmed on the next write attempt.
  const TemporaryDirectory temporary_directory;
  const fs::path prefix = temporary_directory.path() / "publication_retry";
  const auto fault = std::make_shared<SyncFault>();
  FaultInjectingTileStore store(prefix, fault);
  const TileRef ref{3, 9};
  const auto full = make_hashes(merkle::tiles::TILE_WIDTH);
  const auto tile_directory = store.tile_path(ref).parent_path();
  fault->fail_path = tile_directory;
  fault->failures_remaining = 1;

  CHECK_THROWS_AS((store.write_tile(ref, full)), std::runtime_error);
  CHECK(store.has_full_tile(ref.level, ref.index));
  CHECK(store.read_tile(ref) == full);
  CHECK_FALSE(any_tmp_files(prefix));

  const size_t calls_before_confirm = fault->call_count(tile_directory);
  store.begin_attempt();
  CHECK(store.confirm_tile(ref.level, ref.index));
  CHECK(fault->call_count(tile_directory) == calls_before_confirm + 1);
  CHECK(store.confirm_tile(ref.level, ref.index));
  CHECK(fault->call_count(tile_directory) == calls_before_confirm + 1);

  store.begin_attempt();
  CHECK_FALSE(store.confirm_tile(ref.level, ref.index + 1));
}

TEST_CASE("Replacement and directory conflicts clean up temporary files")
{
  // 2e. Failures before atomic replacement clean up temporary files, and a
  // non-directory in the destination path is rejected.
  const TemporaryDirectory temporary_directory;
  const fs::path& dir = temporary_directory.path();
  const auto full = make_hashes(merkle::tiles::TILE_WIDTH);

  const fs::path replacement_prefix = dir / "replacement_failure";
  merkle::tiles::TileStore replacement_store(replacement_prefix);
  const TileRef blocked_ref{0, 1};
  fs::create_directories(replacement_store.tile_path(blocked_ref));
  CHECK_THROWS_AS(
    (replacement_store.write_tile(blocked_ref, full)), std::runtime_error);
  CHECK_FALSE(any_tmp_files(replacement_prefix));

  const fs::path conflict_prefix = dir / "directory_conflict";
  fs::create_directories(conflict_prefix);
  overwrite_file(conflict_prefix / "sha256-256w", {0x00});
  merkle::tiles::TileStore conflict_store(conflict_prefix);
  CHECK_THROWS_AS(
    (conflict_store.write_tile(TileRef{0, 0}, full)), std::runtime_error);
  CHECK_FALSE(any_tmp_files(conflict_prefix));
}

TEST_CASE("Exclusive file creation rejects file and symlink collisions")
{
  // 2f. Unique temporary-file creation must fail rather than overwrite an
  // existing path, including a pre-created symlink on POSIX.
  const TemporaryDirectory temporary_directory;
  const fs::path& dir = temporary_directory.path();
  fs::create_directories(dir);
  const fs::path existing_path = dir / "preexisting-temp";
  const std::vector<uint8_t> original = {0x11, 0x22, 0x33};
  overwrite_file(existing_path, original);

  std::string collision_error;
  try
  {
    merkle::pal::write_and_sync_file(existing_path, {0xAA});
  }
  catch (const std::runtime_error& ex)
  {
    collision_error = ex.what();
  }
  REQUIRE_FALSE(collision_error.empty());
  CHECK(collision_error.find(existing_path.string()) != std::string::npos);
#ifdef _WIN32
  CHECK(collision_error.find(": error ") != std::string::npos);
#endif
  CHECK(read_file(existing_path) == original);

#ifndef _WIN32
  const fs::path symlink_path = dir / "preexisting-temp-symlink";
  fs::create_symlink(existing_path, symlink_path);
  CHECK_THROWS_AS(
    (merkle::pal::write_and_sync_file(symlink_path, {0xBB})),
    std::runtime_error);
  CHECK(read_file(existing_path) == original);
#endif
}

TEST_CASE("Successful writes must make progress")
{
  // 2g. A successful write must make progress. This shared guard prevents
  // the Windows WriteFile loop from spinning if it reports zero bytes.
  CHECK_THROWS_AS(
    merkle::pal::require_write_progress(0, "test"), std::runtime_error);
  CHECK_NOTHROW(merkle::pal::require_write_progress(1, "test"));
}

#ifndef _WIN32
TEST_CASE("Interrupted sync operations are retried")
{
  // 2h. Interrupted sync operations are retried, while other failures are
  // returned immediately with errno intact.
  size_t attempts = 0;
  const int retry_result = merkle::pal::detail::retry_on_eintr([&]() {
    attempts++;
    if (attempts == 1)
    {
      errno = EINTR;
      return -1;
    }
    return 0;
  });
  CHECK(retry_result == 0);
  CHECK(attempts == 2);

  attempts = 0;
  const int error_result = merkle::pal::detail::retry_on_eintr([&]() {
    attempts++;
    errno = EIO;
    return -1;
  });
  CHECK(error_result == -1);
  CHECK(attempts == 1);
  CHECK(errno == EIO);
}
#endif

TEST_CASE("Full tiles use raw bytes and round-trip")
{
  // 3a. Full tile byte round-trip.
  const TemporaryDirectory temporary_directory;
  merkle::tiles::TileStore store(temporary_directory.path());
  const size_t hash_size = Hash().size();
  const auto full = make_hashes(merkle::tiles::TILE_WIDTH);
  const TileRef full_ref{0, 0};

  store.write_tile(full_ref, full);
  CHECK(store.has_full_tile(0, 0));
  CHECK_FALSE(store.has_full_tile(0, 5));
  CHECK_THROWS_AS((void)store.read_tile(TileRef{0, 5}), std::runtime_error);
  CHECK(
    fs::file_size(store.tile_path(full_ref)) ==
    (uintmax_t)merkle::tiles::TILE_WIDTH * hash_size);

  std::vector<uint8_t> expected_tile_bytes;
  expected_tile_bytes.reserve(full.size() * hash_size);
  for (const auto& hash : full)
  {
    hash.serialise(expected_tile_bytes);
  }
  CHECK(read_file(store.tile_path(full_ref)) == expected_tile_bytes);

  const auto full_roundtrip = store.read_tile(full_ref);
  REQUIRE(full_roundtrip.size() == full.size());
  for (size_t i = 0; i < full.size(); i++)
  {
    CAPTURE(i);
    CHECK(full_roundtrip[i] == full[i]);
  }

  // 3b. Wrong-width writes are rejected (only 256-wide tiles are valid).
  const std::vector<Hash> three(full.begin(), full.begin() + 3);
  CHECK_THROWS_AS((store.write_tile(TileRef{0, 2}, three)), std::runtime_error);
}

TEST_CASE("Entry encoding enforces bounds and round-trips")
{
  // 4. Entry bundles preserve empty, short, and multi-byte-length entries,
  // enforce their width and uint16 length limits, and can be re-confirmed.
  const auto entries = make_entries();
  const auto encoded_entries =
    merkle::tiles::TileStore::encode_entries(entries);
  REQUIRE(encoded_entries.size() > 7);
  CHECK(encoded_entries[0] == 0x00);
  CHECK(encoded_entries[1] == 0x00);
  CHECK(encoded_entries[2] == 0x00);
  CHECK(encoded_entries[3] == 0x01);
  CHECK(encoded_entries[4] == 0xA5);
  CHECK(encoded_entries[5] == 0x01);
  CHECK(encoded_entries[6] == 0x00);
  CHECK(
    merkle::tiles::TileStore::decode_entries(
      encoded_entries, merkle::tiles::TILE_WIDTH) == entries);
  CHECK(merkle::tiles::TileStore::decode_entries({}, 0).empty());
  CHECK_THROWS_AS(
    (merkle::tiles::TileStore::decode_entries(
      {}, std::numeric_limits<size_t>::max())),
    std::runtime_error);

  const std::vector<uint8_t> maximum_entry(0xFFFF, 0xC3);
  const auto maximum_encoded =
    merkle::tiles::TileStore::encode_entries({maximum_entry});
  const auto maximum_decoded =
    merkle::tiles::TileStore::decode_entries(maximum_encoded, 1);
  CHECK(maximum_encoded.size() == maximum_entry.size() + 2);
  CHECK(maximum_encoded[0] == 0xFF);
  CHECK(maximum_encoded[1] == 0xFF);
  REQUIRE(maximum_decoded.size() == 1);
  CHECK(maximum_decoded[0] == maximum_entry);
  CHECK_THROWS_AS(
    (merkle::tiles::TileStore::encode_entries({std::vector<uint8_t>(0x10000)})),
    std::runtime_error);
}

TEST_CASE("Entry bundle storage validates and confirms publications")
{
  const TemporaryDirectory temporary_directory;
  const fs::path& dir = temporary_directory.path();
  merkle::tiles::TileStore store(dir);
  const auto entries = make_entries();
  const auto encoded_entries =
    merkle::tiles::TileStore::encode_entries(entries);

  CHECK_FALSE(store.has_entry_bundle(17));
  CHECK_THROWS_AS((void)store.read_entry_bundle(17), std::runtime_error);
  const std::vector<std::vector<uint8_t>> short_bundle(
    entries.begin(), entries.end() - 1);
  CHECK_THROWS_AS(
    (store.write_entry_bundle(1, short_bundle)), std::runtime_error);
  auto oversized_bundle = entries;
  oversized_bundle[0].resize(0x10000);
  CHECK_THROWS_AS(
    (store.write_entry_bundle(2, oversized_bundle)), std::runtime_error);
  CHECK_FALSE(fs::exists(store.entries_path(1)));
  CHECK_FALSE(fs::exists(store.entries_path(2)));

  store.write_entry_bundle(0, entries);
  CHECK(store.has_entry_bundle(0));
  CHECK(store.read_entry_bundle(0) == entries);
  CHECK(fs::file_size(store.entries_path(0)) == encoded_entries.size());
  CHECK(read_file(store.entries_path(0)) == encoded_entries);

  const fs::path prefix = dir / "bundle_confirmation";
  const auto fault = std::make_shared<SyncFault>();
  FaultInjectingTileStore confirming_store(prefix, fault);
  confirming_store.write_entry_bundle(4, entries);
  const auto entries_directory = confirming_store.entries_path(4).parent_path();
  const size_t calls_before_confirm = fault->call_count(entries_directory);
  confirming_store.begin_attempt();
  CHECK(confirming_store.confirm_bundle(4));
  CHECK(fault->call_count(entries_directory) == calls_before_confirm + 1);
  CHECK(confirming_store.confirm_bundle(4));
  CHECK(fault->call_count(entries_directory) == calls_before_confirm + 1);
  confirming_store.begin_attempt();
  CHECK_FALSE(confirming_store.confirm_bundle(5));
}

TEST_CASE("Corrupt tiles and entry bundles are rejected")
{
  // 5. Corrupt / truncated files are rejected on read (integrity check), so
  // a torn write can never be served as a valid tile or bundle.
  const TemporaryDirectory temporary_directory;
  merkle::tiles::TileStore store(temporary_directory.path());
  const size_t hash_size = Hash().size();
  const auto full = make_hashes(merkle::tiles::TILE_WIDTH);
  const TileRef full_ref{0, 0};
  const auto entries = make_entries();
  store.write_tile(full_ref, full);

  overwrite_file(store.tile_path(full_ref), std::vector<uint8_t>(hash_size, 0));
  CHECK_FALSE(store.has_full_tile(0, 0));
  CHECK_THROWS_AS((void)store.read_tile(full_ref), std::runtime_error);
  store.write_tile(full_ref, full);
  CHECK(store.has_full_tile(0, 0));
  CHECK(store.read_tile(full_ref) == full);

  overwrite_file(
    store.tile_path(full_ref),
    std::vector<uint8_t>((merkle::tiles::TILE_WIDTH + 1) * hash_size, 0));
  CHECK_FALSE(store.has_full_tile(0, 0));
  CHECK_THROWS_AS((void)store.read_tile(full_ref), std::runtime_error);

  store.write_entry_bundle(0, entries);
  CHECK(store.has_entry_bundle(0));
  CHECK(store.read_entry_bundle(0) == entries);
  overwrite_file(store.entries_path(0), {0x00, 0x05, 0x01});
  CHECK_FALSE(store.has_entry_bundle(0));
  CHECK_THROWS_AS((void)store.read_entry_bundle(0), std::runtime_error);

  auto encoded = merkle::tiles::TileStore::encode_entries(entries);
  encoded.push_back(0xFF);
  overwrite_file(store.entries_path(0), encoded);
  CHECK_FALSE(store.has_entry_bundle(0));
  CHECK_THROWS_AS((void)store.read_entry_bundle(0), std::runtime_error);

  CHECK_THROWS_AS(
    (merkle::tiles::TileStore::decode_entries({0xFF, 0xFF, 0x00}, 1)),
    std::runtime_error);
  CHECK_THROWS_AS(
    (merkle::tiles::TileStore::decode_entries({}, 1)), std::runtime_error);
  CHECK_THROWS_AS(
    (merkle::tiles::TileStore::decode_entries({0x00, 0x00, 0xFF}, 1)),
    std::runtime_error);
}

// Windows file replacement semantics can reject racing same-path replaces even
// when the final tile remains valid, so keep this stress check POSIX-only.
#ifndef _WIN32
TEST_CASE("Concurrent same-tile writes leave a valid tile")
{
  // 6. Concurrent same-tile writes use unique temp files and leave no
  // temporary files behind after success. Each thread owns its store object;
  // sharing one object requires caller-provided synchronization.
  const TemporaryDirectory temporary_directory;
  const fs::path& dir = temporary_directory.path();
  const merkle::tiles::TileStore store(dir);
  const auto full = make_hashes(merkle::tiles::TILE_WIDTH);
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

  CHECK(ok.load());
  CHECK(store.read_tile(concurrent_ref) == full);
  CHECK_FALSE(any_tmp_files(dir));
}
#endif
