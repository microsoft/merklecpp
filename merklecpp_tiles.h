// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "merklecpp.h"
#include "merklecpp_pal.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Tiled storage for merklecpp trees, following the full-tile geometry, payload,
// and path encoding of the C2SP tlog-tiles layout
// (https://c2sp.org/tlog-tiles). C2SP defines SHA-256; merklecpp preserves the
// 256-hash tile width for other SHA output sizes and separates each format
// under an algorithm-qualified directory such as sha256-256w or sha384-256w.
// Only complete, immutable tiles are stored. Their hash values are produced by
// the tree's existing HASH_FUNCTION, so tile-derived proofs are byte-identical
// to those produced by merkle::TreeT (see doc/design/tlog-tiles.md).
//
// Thread safety: types in this header do not synchronize access internally.
// Callers must serialize access to each shared object, including const proof
// operations that update the tile cache, and all writers sharing a store
// prefix. Independent store objects may read while the serialized writer
// publishes tiles atomically.

namespace merkle // NOLINT(modernize-concat-nested-namespaces)
{
  namespace tiles
  {
    /// @brief Number of tree levels spanned by a single tile.
    static constexpr uint16_t TILE_HEIGHT = 8;

    /// @brief Number of hashes in a full tile (2**TILE_HEIGHT).
    static constexpr uint16_t TILE_WIDTH = uint16_t{1U << TILE_HEIGHT};

    /// @brief Highest tile level permitted by the tlog-tiles layout.
    static constexpr uint8_t MAX_TILE_LEVEL = 63;

    namespace detail
    {
      static constexpr std::string_view SHA256_ALGORITHM_SHORT_NAME = "sha256";
      static constexpr std::string_view SHA384_ALGORITHM_SHORT_NAME = "sha384";
      static constexpr std::string_view SHA512_ALGORITHM_SHORT_NAME = "sha512";
      static constexpr size_t ENTRY_LENGTH_PREFIX_SIZE = 2;
      static constexpr size_t MAX_ENTRY_SIZE = 0xFFFF;
      static constexpr size_t MAX_ENTRY_BUNDLE_SIZE =
        TILE_WIDTH * (ENTRY_LENGTH_PREFIX_SIZE + MAX_ENTRY_SIZE);

      template <typename Present>
      uint64_t contiguous_prefix_length(uint64_t limit, const Present& present)
      {
        uint64_t length = 0;
        while (length < limit && present(length))
        {
          length++;
        }
        return length;
      }
    }

    /// @brief Encodes a tile index as tlog-tiles path elements.
    /// @param n The tile index
    /// @return The index as zero-padded, '/'-separated 3-digit groups, where
    /// all but the last group are prefixed with 'x'. For example, 1234067 is
    /// encoded as "x001/x234/067" and 5 as "005".
    static inline std::string encode_tile_index(uint64_t n)
    {
      std::vector<uint16_t> parts;
      do
      {
        parts.emplace_back(static_cast<uint16_t>(n % 1000));
        n /= 1000;
      } while (n > 0);

      std::string r;
      r.reserve(parts.size() * 5);
      for (size_t i = 0; i < parts.size(); i++)
      {
        const auto part = parts[parts.size() - 1 - i];
        std::format_to(
          std::back_inserter(r),
          "{}{}{:03}",
          i == 0 ? "" : "/",
          i + 1 < parts.size() ? "x" : "",
          part);
      }
      return r;
    }

    /// @brief Identifies a single (full) tile within a tiled log.
    /// @note Only full, TILE_WIDTH-wide tiles are produced and consumed; the
    /// incomplete frontier is never tiled (see doc/design/tlog-tiles.md).
    struct TileRef
    {
      /// @brief The level of the tile (0 == leaf hashes, maximum 63).
      uint8_t level = 0;

      /// @brief The index of the tile within its level.
      uint64_t index = 0;
    };

    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class TileWriterT;

    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class EntryBundleWriterT;

    /// @brief Reads and writes tlog-tiles tile files on a local filesystem.
    /// @tparam HASH_SIZE Size of each hash in bytes
    /// @tparam HASH_FUNCTION The tree's node hash function (carried for use by
    /// later components; tile I/O itself does not hash).
    /// @warning No internal synchronization is provided. Callers must serialize
    /// access to each store object and all writers sharing its prefix.
    /// Independent store objects may read while the serialized writer publishes
    /// tiles atomically.
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class TileStoreT
    {
      friend class TileWriterT<HASH_SIZE, HASH_FUNCTION>;
      friend class EntryBundleWriterT<HASH_SIZE, HASH_FUNCTION>;

    public:
      /// @brief The type of hashes stored in tiles.
      using Hash = HashT<HASH_SIZE>;

      /// @brief Constructs a tile store below an algorithm-qualified directory
      /// under @p prefix.
      /// @param prefix The directory under which the format directory lives
      /// @note Built-in SHA functions select sha256, sha384, or sha512.
      explicit TileStoreT(std::filesystem::path prefix) :
        TileStoreT(std::move(prefix), default_hash_algorithm_short_name())
      {}

      /// @brief Constructs a tile store for an explicitly named hash algorithm.
      /// @param prefix The directory under which the format directory lives
      /// @param hash_algorithm_short_name Lowercase algorithm short name
      TileStoreT(
        std::filesystem::path prefix,
        const std::string& hash_algorithm_short_name) :
        prefix(storage_root(std::move(prefix), hash_algorithm_short_name))
      {}

      /// @brief The algorithm-qualified root directory of the store.
      [[nodiscard]] const std::filesystem::path& root() const
      {
        return prefix;
      }

      /// @brief The format directory for @p hash_algorithm_short_name.
      /// @note TILE_WIDTH is fixed at 256 for every hash output size.
      static std::string storage_directory_name(
        const std::string& hash_algorithm_short_name)
      {
        validate_hash_algorithm_short_name(hash_algorithm_short_name);
        return std::format("{}-{}w", hash_algorithm_short_name, TILE_WIDTH);
      }

      /// @brief Encodes a tile index (see encode_tile_index).
      static std::string encode_index(uint64_t n)
      {
        return encode_tile_index(n);
      }

      /// @brief The filesystem path of a tile.
      /// @throws std::runtime_error if the tile level exceeds 63
      [[nodiscard]] std::filesystem::path tile_path(const TileRef& ref) const
      {
        if (ref.level > MAX_TILE_LEVEL)
        {
          throw std::runtime_error("tile level out of range");
        }
        const auto relative_path = std::format(
          "tile/{}/{}",
          static_cast<unsigned>(ref.level),
          encode_tile_index(ref.index));
        return prefix / relative_path;
      }

      /// @brief The filesystem path of an entry bundle.
      [[nodiscard]] std::filesystem::path entries_path(uint64_t index) const
      {
        const auto relative_path =
          std::format("tile/entries/{}", encode_tile_index(index));
        return prefix / relative_path;
      }

      /// @brief Whether a full tile exists on disk.
      [[nodiscard]] bool has_full_tile(uint8_t level, uint64_t index) const
      {
        if (level > MAX_TILE_LEVEL)
        {
          return false;
        }
        std::error_code ec;
        const auto path = tile_path(TileRef{level, index});
        if (!std::filesystem::is_regular_file(path, ec) || ec)
        {
          return false;
        }
        return std::filesystem::file_size(path, ec) ==
          (uintmax_t)TILE_WIDTH * HASH_SIZE &&
          !ec;
      }

      /// @brief Writes a tile to disk atomically.
      /// @param ref The tile to write
      /// @param hashes The tile's hashes (exactly TILE_WIDTH of them)
      void write_tile(const TileRef& ref, const std::vector<Hash>& hashes)
      {
        if (hashes.size() != TILE_WIDTH)
        {
          throw std::runtime_error("tile width mismatch");
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(hashes.size() * HASH_SIZE);
        for (const auto& h : hashes)
        {
          h.serialise(bytes);
        }

        write_file_atomically(tile_path(ref), bytes);
      }

      /// @brief Reads a tile from disk.
      /// @param ref The tile to read
      /// @return The tile's hashes (TILE_WIDTH of them)
      [[nodiscard]] std::vector<Hash> read_tile(const TileRef& ref) const
      {
        const size_t expected = (size_t)TILE_WIDTH * HASH_SIZE;
        const auto path = tile_path(ref);
        std::vector<uint8_t> bytes = read_file(path, expected);
        if (bytes.size() != expected)
        {
          throw std::runtime_error(std::format(
            "unexpected tile size for {}: expected {} bytes, got {}",
            path.string(),
            expected,
            bytes.size()));
        }

        std::vector<Hash> hashes;
        hashes.reserve(TILE_WIDTH);
        size_t position = 0;
        for (uint16_t i = 0; i < TILE_WIDTH; i++)
        {
          hashes.emplace_back(bytes, position);
        }
        return hashes;
      }

      /// @brief Whether a full entry bundle exists on disk.
      [[nodiscard]] bool has_entry_bundle(uint64_t index) const
      {
        std::error_code ec;
        const auto path = entries_path(index);
        if (!std::filesystem::is_regular_file(path, ec) || ec)
        {
          return false;
        }
        try
        {
          (void)decode_entries(
            read_file(path, detail::MAX_ENTRY_BUNDLE_SIZE), TILE_WIDTH);
          return true;
        }
        catch (const std::runtime_error&)
        {
          return false;
        }
      }

      /// @brief Writes a full entry bundle to disk atomically.
      /// @param index The bundle index
      /// @param entries The raw log entries (exactly TILE_WIDTH of them)
      /// @note Entries are stored in the tlog-tiles entry-bundle format: a
      /// sequence of big-endian uint16 length-prefixed byte strings.
      void write_entry_bundle(
        uint64_t index, const std::vector<std::vector<uint8_t>>& entries)
      {
        if (entries.size() != TILE_WIDTH)
        {
          throw std::runtime_error("entry bundle width mismatch");
        }
        write_file_atomically(entries_path(index), encode_entries(entries));
      }

      /// @brief Reads a full entry bundle from disk.
      /// @param index The bundle index
      /// @return The raw log entries (TILE_WIDTH of them)
      [[nodiscard]] std::vector<std::vector<uint8_t>> read_entry_bundle(
        uint64_t index) const
      {
        const auto path = entries_path(index);
        const auto bytes = read_file(path, detail::MAX_ENTRY_BUNDLE_SIZE);
        try
        {
          return decode_entries(bytes, TILE_WIDTH);
        }
        catch (const std::runtime_error& error)
        {
          throw std::runtime_error(std::format(
            "invalid entry bundle {}: {}", path.string(), error.what()));
        }
      }

      /// @brief Encodes log entries into the tlog-tiles entry-bundle format.
      static std::vector<uint8_t> encode_entries(
        const std::vector<std::vector<uint8_t>>& entries)
      {
        size_t encoded_size = 0;
        for (const auto& e : entries)
        {
          if (e.size() > detail::MAX_ENTRY_SIZE)
          {
            throw std::runtime_error(
              "entry too large for uint16 length prefix");
          }
          encoded_size += detail::ENTRY_LENGTH_PREFIX_SIZE + e.size();
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(encoded_size);
        for (const auto& e : entries)
        {
          bytes.push_back((uint8_t)((e.size() >> 8) & 0xFF));
          bytes.push_back((uint8_t)(e.size() & 0xFF));
          bytes.insert(bytes.end(), e.begin(), e.end());
        }
        return bytes;
      }

      /// @brief Decodes @p count entries from the entry-bundle format.
      static std::vector<std::vector<uint8_t>> decode_entries(
        const std::vector<uint8_t>& bytes, size_t count)
      {
        if (count > bytes.size() / detail::ENTRY_LENGTH_PREFIX_SIZE)
        {
          throw std::runtime_error("truncated entry bundle");
        }
        std::vector<std::vector<uint8_t>> out;
        out.reserve(count);
        size_t pos = 0;
        for (size_t i = 0; i < count; i++)
        {
          if (bytes.size() - pos < 2)
          {
            throw std::runtime_error("truncated entry bundle");
          }
          const auto len =
            (uint16_t)(((uint16_t)bytes[pos] << 8) | bytes[pos + 1]);
          pos += 2;
          if (len > bytes.size() - pos)
          {
            throw std::runtime_error("truncated entry bundle");
          }
          out.emplace_back(
            bytes.begin() + static_cast<std::ptrdiff_t>(pos),
            bytes.begin() + static_cast<std::ptrdiff_t>(pos + len));
          pos += len;
        }
        if (pos != bytes.size())
        {
          throw std::runtime_error("trailing bytes in entry bundle");
        }
        return out;
      }

      /// @cond INTERNAL
    protected:
      using DirectorySync = std::function<void(const std::filesystem::path&)>;

      TileStoreT(std::filesystem::path prefix, DirectorySync directory_sync) :
        TileStoreT(
          std::move(prefix),
          default_hash_algorithm_short_name(),
          std::move(directory_sync))
      {}

      TileStoreT(
        std::filesystem::path prefix,
        const std::string& hash_algorithm_short_name,
        DirectorySync directory_sync) :
        prefix(storage_root(std::move(prefix), hash_algorithm_short_name)),
        directory_sync(std::move(directory_sync))
      {}

      /// @brief The algorithm-qualified root directory of the store.
      std::filesystem::path prefix;

      DirectorySync directory_sync;
      std::set<std::filesystem::path> durable_directory_entries;
      std::set<std::filesystem::path> durable_directory_contents;

      static std::string default_hash_algorithm_short_name()
      {
        if constexpr (HASH_SIZE == merkle::Tree::Hash::size_bytes)
        {
          if constexpr (HASH_FUNCTION == merkle::Tree::hash_function)
          {
            return std::string(detail::SHA256_ALGORITHM_SHORT_NAME);
          }
#ifdef HAVE_OPENSSL
          if constexpr (HASH_FUNCTION == sha256_openssl)
          {
            return std::string(detail::SHA256_ALGORITHM_SHORT_NAME);
          }
#endif
        }
#ifdef HAVE_OPENSSL
        else if constexpr (HASH_SIZE == merkle::Tree384::Hash::size_bytes)
        {
          if constexpr (HASH_FUNCTION == merkle::Tree384::hash_function)
          {
            return std::string(detail::SHA384_ALGORITHM_SHORT_NAME);
          }
        }
        else if constexpr (HASH_SIZE == merkle::Tree512::Hash::size_bytes)
        {
          if constexpr (HASH_FUNCTION == merkle::Tree512::hash_function)
          {
            return std::string(detail::SHA512_ALGORITHM_SHORT_NAME);
          }
        }
#endif
        throw std::runtime_error(
          "TileStoreT requires a hash algorithm short name");
      }

      static void validate_hash_algorithm_short_name(
        const std::string& hash_algorithm_short_name)
      {
        if (
          hash_algorithm_short_name.empty() ||
          hash_algorithm_short_name.front() == '-' ||
          hash_algorithm_short_name.back() == '-')
        {
          throw std::runtime_error("invalid hash algorithm short name");
        }
        for (const char c : hash_algorithm_short_name)
        {
          if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '-')
          {
            throw std::runtime_error("invalid hash algorithm short name");
          }
        }
      }

      static std::filesystem::path storage_root(
        std::filesystem::path prefix,
        const std::string& hash_algorithm_short_name)
      {
        if (
          (hash_algorithm_short_name == detail::SHA256_ALGORITHM_SHORT_NAME &&
           HASH_SIZE != merkle::Hash::size_bytes) ||
          (hash_algorithm_short_name == detail::SHA384_ALGORITHM_SHORT_NAME &&
           HASH_SIZE != merkle::Hash384::size_bytes) ||
          (hash_algorithm_short_name == detail::SHA512_ALGORITHM_SHORT_NAME &&
           HASH_SIZE != merkle::Hash512::size_bytes))
        {
          throw std::runtime_error(
            "hash algorithm short name does not match hash size");
        }
        prefix /= storage_directory_name(hash_algorithm_short_name);
        return prefix;
      }

      [[nodiscard]] bool confirm_full_tile(uint8_t level, uint64_t index)
      {
        const auto path = tile_path(TileRef{level, index});
        if (!has_full_tile(level, index))
        {
          return false;
        }
        confirm_file_durable(path);
        return true;
      }

      [[nodiscard]] bool confirm_entry_bundle(uint64_t index)
      {
        const auto path = entries_path(index);
        if (!has_entry_bundle(index))
        {
          return false;
        }
        confirm_file_durable(path);
        return true;
      }

      void begin_write_attempt()
      {
        durable_directory_contents.clear();
      }

      /// @brief Reads an entire file into a byte vector.
      static std::vector<uint8_t> read_file(
        const std::filesystem::path& path, size_t max_size)
      {
        std::ifstream f(path, std::ios::binary);
        if (!f.good())
        {
          throw std::runtime_error(
            std::format("cannot open file: {}", path.string()));
        }

        std::vector<uint8_t> bytes;
        std::array<uint8_t, 4096> buffer{};
        while (f)
        {
          const size_t remaining = max_size - bytes.size();
          const size_t request =
            remaining < buffer.size() ? remaining + 1 : buffer.size();
          f.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(request));
          const auto count = static_cast<size_t>(f.gcount());
          if (count == 0)
          {
            break;
          }
          if (count > remaining)
          {
            throw std::runtime_error(
              std::format("file exceeds maximum size: {}", path.string()));
          }
          bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
        }
        if (f.bad())
        {
          throw std::runtime_error(
            std::format("error reading file: {}", path.string()));
        }
        return bytes;
      }

      /// @brief Writes a file atomically via a synced temporary file.
      /// @note Uses unique temp names, cleans them up on errors, and syncs the
      /// file before publishing it with an atomic replace. POSIX builds also
      /// confirm each directory entry in the path and sync the destination
      /// directory after rename. Existing files are only reused after these
      /// syncs are re-confirmed by the current write attempt.
      void write_file_atomically(
        const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
      {
        create_directories_durably(path.parent_path());

        const std::filesystem::path tmp = temp_path(path);
        TempFileGuard guard(tmp);
        merkle::pal::write_and_sync_file(tmp, bytes);
        guard.arm();
        const auto parent = directory_or_dot(path.parent_path());
        durable_directory_contents.erase(parent);
        merkle::pal::replace_file(tmp, path);
        sync_directory_contents(parent);
        guard.dismiss();
      }

      static std::filesystem::path directory_or_dot(
        const std::filesystem::path& directory)
      {
        return directory.empty() ? std::filesystem::path(".") : directory;
      }

      void create_directories_durably(
        const std::filesystem::path& requested_directory)
      {
        const auto directory = directory_or_dot(requested_directory);
        if (durable_directory_entries.contains(directory))
        {
          return;
        }

        auto parent = directory_or_dot(directory.parent_path());
        if (parent != directory)
        {
          create_directories_durably(parent);
        }

        std::error_code ec;
        const bool exists = std::filesystem::exists(directory, ec);
        if (ec)
        {
          throw std::runtime_error(std::format(
            "cannot inspect directory {}: {}",
            directory.string(),
            ec.message()));
        }
        if (exists)
        {
          const bool is_directory =
            std::filesystem::is_directory(directory, ec);
          if (ec)
          {
            throw std::runtime_error(std::format(
              "cannot inspect directory {}: {}",
              directory.string(),
              ec.message()));
          }
          if (!is_directory)
          {
            throw std::runtime_error(std::format(
              "cannot create directory {}: path exists and is not a directory",
              directory.string()));
          }
        }
        else
        {
          const bool created = std::filesystem::create_directory(directory, ec);
          if (ec)
          {
            throw std::runtime_error(std::format(
              "cannot create directory {}: {}",
              directory.string(),
              ec.message()));
          }
          if (!created && !std::filesystem::is_directory(directory, ec))
          {
            throw std::runtime_error(
              ec ?
                std::format(
                  "cannot create directory {}: {}",
                  directory.string(),
                  ec.message()) :
                std::format("cannot create directory {}", directory.string()));
          }
        }

        if (parent != directory)
        {
          sync_directory_for_durability(parent);
          durable_directory_contents.insert(parent);
        }
        durable_directory_entries.insert(directory);
      }

      void confirm_file_durable(const std::filesystem::path& path)
      {
        const auto parent = directory_or_dot(path.parent_path());
        create_directories_durably(parent);
        sync_directory_contents(parent);
      }

      void sync_directory_contents(const std::filesystem::path& directory)
      {
        const auto path = directory_or_dot(directory);
        if (durable_directory_contents.contains(path))
        {
          return;
        }
        sync_directory_for_durability(path);
        durable_directory_contents.insert(path);
      }

      class TempFileGuard
      {
      public:
        explicit TempFileGuard(std::filesystem::path path) :
          path(std::move(path))
        {}

        ~TempFileGuard()
        {
          if (active)
          {
            std::error_code ec;
            std::filesystem::remove(path, ec);
          }
        }

        void arm()
        {
          active = true;
        }

        void dismiss()
        {
          active = false;
        }

      private:
        std::filesystem::path path;
        bool active = false;
      };

      static std::filesystem::path temp_path(const std::filesystem::path& path)
      {
        static std::atomic<uint64_t> counter{0};
        const auto stamp =
          std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path tmp = path;
        tmp += std::format(
          ".tmp.{}.{}.{}",
          merkle::pal::process_id(),
          (uint64_t)stamp,
          counter.fetch_add(1, std::memory_order_relaxed));
        return tmp;
      }

      void sync_directory_for_durability(const std::filesystem::path& path)
      {
        if (directory_sync)
        {
          directory_sync(path);
          return;
        }
        merkle::pal::sync_directory_on_disk(path);
      }
      /// @endcond
    };

    /// @brief Computes the Merkle Tree Hash of a perfect (balanced) subtree.
    /// @param leaves The subtree's leaves; the count MUST be a power of two.
    /// @return The subtree root, computed with the tree's HASH_FUNCTION.
    /// @note This is exactly a merkle::TreeT full-node hash, which is why tile
    /// entries (such roots) are immutable: an unbalanced subtree would still
    /// change as leaves are added and must therefore never be tiled.
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    inline HashT<HASH_SIZE> perfect_root(
      const std::vector<HashT<HASH_SIZE>>& leaves)
    {
      if (leaves.empty())
      {
        throw std::runtime_error("perfect_root requires at least one leaf");
      }
      if ((leaves.size() & (leaves.size() - 1)) != 0)
      {
        throw std::runtime_error(
          "perfect_root requires a power-of-two number of leaves");
      }

      std::vector<HashT<HASH_SIZE>> level = leaves;
      while (level.size() > 1)
      {
        std::vector<HashT<HASH_SIZE>> next;
        next.reserve(level.size() / 2);
        for (size_t i = 0; i + 1 < level.size(); i += 2)
        {
          HashT<HASH_SIZE> h;
          HASH_FUNCTION(level[i], level[i + 1], h);
          next.push_back(h);
        }
        level.swap(next);
      }
      return level.front();
    }

    /// @brief Computes and persists tlog-tiles tiles for a growing tree.
    /// @tparam HASH_SIZE Size of each hash in bytes
    /// @tparam HASH_FUNCTION The tree's node hash function
    /// @note Only balanced subtrees are tiled: a level-L entry is the root of a
    /// complete 2**(8L)-leaf subtree. Only full tiles (256 such entries) are
    /// written; they are therefore immutable and written exactly once. Entries
    /// beyond the last full-tile boundary remain in memory until a later flush
    /// completes the next tile.
    /// @warning No internal synchronization is provided. Callers must serialize
    /// access to a writer and its store.
    /// @warning A writer trusts existing full tiles as output from the same
    /// tree and hash function. Callers resuming a store must establish that
    /// ownership and restore the matching tree state.
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class TileWriterT
    {
    public:
      /// @brief The type of hashes stored in tiles.
      using Hash = HashT<HASH_SIZE>;

      /// @brief The associated tile store type.
      using Store = TileStoreT<HASH_SIZE, HASH_FUNCTION>;

      /// @brief Supplies the level-0 leaf hash for a given leaf index.
      using LeafFn = std::function<const Hash&(uint64_t)>;

      /// @brief Counts of work performed by a write_up_to call.
      struct Stats
      {
        /// @brief Number of full tiles written.
        uint64_t full_written = 0;
      };

      /// @brief Constructs a writer over @p store.
      explicit TileWriterT(Store& store) : store(store) {}

      /// @brief Writes all newly-complete full tiles for a tree of @p size
      /// leaves.
      /// @param size The current tree size
      /// @param leaf_at Returns the level-0 leaf hash for a leaf index in
      /// [0, size); only ever queried for leaves of complete subtrees.
      /// @return Counts of tiles written
      /// @note Incremental: full tiles already on disk are immutable and are
      /// never rewritten once validated and confirmed durable. Malformed files
      /// are replaced. Entries that do not complete a tile are not written.
      /// Tiles are always rolled up through MAX_TILE_LEVEL, so the on-disk set
      /// always contains the higher-level roll-ups that proof generation relies
      /// on.
      Stats write_up_to(uint64_t size, const LeafFn& leaf_at)
      {
        Stats stats;
        store.begin_write_attempt();

        // The loop stops early once a level has no complete entries (see the
        // entries == 0 break below).
        for (uint8_t level = 0; level <= MAX_TILE_LEVEL; level++)
        {
          // Number of complete (balanced) level-L entries available; this
          // deliberately excludes the incomplete frontier subtree.
          const uint64_t entries = entries_at_level(size, level);
          if (entries == 0)
          {
            break;
          }
          ensure_level(level);

          const uint64_t full_tiles = entries / TILE_WIDTH;

          if (cursor_inited[level] == 0)
          {
            next_full[level] = full_prefix_length(level, full_tiles);
            cursor_inited[level] = 1;
          }

          for (uint64_t n = next_full[level]; n < full_tiles; n++)
          {
            if (store.confirm_full_tile(level, n))
            {
              continue; // immutable: never rewrite an existing full tile
            }
            store.write_tile(
              TileRef{level, n},
              collect(level, n * TILE_WIDTH, TILE_WIDTH, leaf_at));
            stats.full_written++;
          }
          if (full_tiles > next_full[level])
          {
            next_full[level] = full_tiles;
          }
        }

        return stats;
      }

    protected:
      /// @brief The tile store written to.
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      Store& store;

      /// @brief Per-level index of the next full tile to write.
      std::vector<uint64_t> next_full;

      /// @brief Per-level flag indicating next_full has been initialised.
      std::vector<uint8_t> cursor_inited;

      /// @brief Number of complete level-@p level entries for a tree of @p
      /// size.
      static uint64_t entries_at_level(uint64_t size, uint8_t level)
      {
        const unsigned shift =
          static_cast<unsigned>(TILE_HEIGHT) * static_cast<unsigned>(level);
        return shift >= 64 ? 0 : (size >> shift);
      }

      /// @brief Ensures per-level bookkeeping vectors cover @p level.
      void ensure_level(uint8_t level)
      {
        const size_t needed = (size_t)level + 1;
        if (next_full.size() < needed)
        {
          next_full.resize(needed, 0);
          cursor_inited.resize(needed, 0);
        }
      }

      /// @brief Length of the confirmed contiguous prefix, bounded by @p limit.
      [[nodiscard]] uint64_t full_prefix_length(uint8_t level, uint64_t limit)
      {
        return detail::contiguous_prefix_length(limit, [&](uint64_t index) {
          return store.confirm_full_tile(level, index);
        });
      }

      /// @brief Collects @p count consecutive level-@p level entries, each the
      /// root of a complete (balanced) subtree.
      std::vector<Hash> collect(
        uint8_t level,
        uint64_t first_entry,
        uint64_t count,
        const LeafFn& leaf_at)
      {
        std::vector<Hash> out;
        out.reserve(count);
        for (uint64_t i = 0; i < count; i++)
        {
          const uint64_t g = first_entry + i;
          if (level == 0)
          {
            out.push_back(leaf_at(g));
          }
          else
          {
            // Roll up the complete child full tile (256 complete entries).
            out.push_back(
              perfect_root<HASH_SIZE, HASH_FUNCTION>(
                store.read_tile(TileRef{(uint8_t)(level - 1), g})));
          }
        }
        return out;
      }
    };

    /// @brief Writes tlog-tiles entry bundles (raw log entries) for a growing
    /// log.
    /// @note Entry bundles are level-0 only and application-owned: merklecpp
    /// stores leaf hashes, while the raw entries (and the leaf-hash derivation
    /// linking each entry to its level-0 tile hash) are the application's
    /// responsibility. Only full bundles (TILE_WIDTH entries) are written; they
    /// are immutable and written exactly once. The incomplete tail stays with
    /// the application until it grows into a full bundle, mirroring the
    /// un-tiled Merkle frontier.
    /// @warning No internal synchronization is provided. Callers must serialize
    /// access to a writer and its store.
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class EntryBundleWriterT
    {
    public:
      using Store = TileStoreT<HASH_SIZE, HASH_FUNCTION>;

      /// @brief Supplies the raw bytes of the log entry at a given index.
      using EntryFn = std::function<std::vector<uint8_t>(uint64_t)>;

      /// @brief Counts of work performed by a write_up_to call.
      struct Stats
      {
        /// @brief Number of full bundles written.
        uint64_t full_written = 0;
      };

      explicit EntryBundleWriterT(Store& store) : store(store) {}

      /// @brief Writes all newly-complete full bundles for a log of @p size
      /// entries.
      /// @param size The current number of entries
      /// @param entry_at Returns the raw bytes of the entry at an index in
      /// [0, size); only ever queried for entries of complete bundles.
      /// @return Counts of bundles written
      /// @note Incremental: full bundles already on disk are immutable and are
      /// never rewritten once validated and confirmed durable. Malformed files
      /// are replaced. The incomplete tail is never bundled.
      Stats write_up_to(uint64_t size, const EntryFn& entry_at)
      {
        Stats stats;
        store.begin_write_attempt();
        const uint64_t full = size / TILE_WIDTH;

        if (!cursor_inited)
        {
          next_full = full_prefix_length(full);
          cursor_inited = true;
        }

        for (uint64_t n = next_full; n < full; n++)
        {
          if (store.confirm_entry_bundle(n))
          {
            continue; // immutable: never rewrite an existing full bundle
          }
          store.write_entry_bundle(
            n, collect(n * TILE_WIDTH, TILE_WIDTH, entry_at));
          stats.full_written++;
        }
        if (full > next_full)
        {
          next_full = full;
        }
        return stats;
      }

    protected:
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      Store& store;
      uint64_t next_full = 0;
      bool cursor_inited = false;

      std::vector<std::vector<uint8_t>> collect(
        uint64_t first, uint64_t count, const EntryFn& entry_at)
      {
        std::vector<std::vector<uint8_t>> out;
        out.reserve(count);
        for (uint64_t i = 0; i < count; i++)
        {
          out.push_back(entry_at(first + i));
        }
        return out;
      }

      [[nodiscard]] uint64_t full_prefix_length(uint64_t limit)
      {
        return detail::contiguous_prefix_length(limit, [&](uint64_t index) {
          return store.confirm_entry_bundle(index);
        });
      }
    };

    /// @brief Default tile store (SHA256, default hash function).
    using TileStore =
      TileStoreT<merkle::Tree::Hash::size_bytes, merkle::Tree::hash_function>;

    /// @brief Default tile writer (SHA256, default hash function).
    using TileWriter =
      TileWriterT<merkle::Tree::Hash::size_bytes, merkle::Tree::hash_function>;

    /// @brief Default entry-bundle writer (SHA256, default hash function).
    using EntryBundleWriter = EntryBundleWriterT<
      merkle::Tree::Hash::size_bytes,
      merkle::Tree::hash_function>;

  }
}
