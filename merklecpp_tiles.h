// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "merklecpp.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

// Tiled storage for merklecpp trees, following the full-tile geometry and path
// encoding of the C2SP tlog-tiles layout (https://c2sp.org/tlog-tiles). Only
// complete, immutable tiles are stored. Their hash values are produced by the
// tree's existing HASH_FUNCTION, so tile-derived proofs are byte-identical to
// those produced by merkle::TreeT (see doc/design/tlog-tiles.md).
//
// Thread safety: types in this header do not synchronize access internally.
// Callers must serialize all operations on shared objects and store prefixes,
// including const proof operations that update the tile cache.

namespace merkle // NOLINT(modernize-concat-nested-namespaces)
{
  namespace tiles
  {
    /// @brief Number of tree levels spanned by a single tile.
    static constexpr uint16_t TILE_HEIGHT = 8;

    /// @brief Number of hashes in a full tile (2**TILE_HEIGHT).
    static constexpr uint16_t TILE_WIDTH = 256;

    namespace detail
    {
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
      std::vector<std::string> parts;
      char buf[8];
      do
      {
        std::snprintf(buf, sizeof(buf), "%03u", (unsigned)(n % 1000));
        parts.emplace_back(buf);
        n /= 1000;
      } while (n > 0);

      std::string r;
      for (size_t i = 0; i < parts.size(); i++)
      {
        const std::string& part = parts[parts.size() - 1 - i];
        if (i != 0)
        {
          r += "/";
        }
        if (i + 1 < parts.size())
        {
          r += "x";
        }
        r += part;
      }
      return r;
    }

    /// @brief Identifies a single (full) tile within a tiled log.
    /// @note Only full, TILE_WIDTH-wide tiles are produced and consumed; the
    /// incomplete frontier is never tiled (see doc/design/tlog-tiles.md).
    struct TileRef
    {
      /// @brief The level of the tile (0 == leaf hashes).
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
    /// access to a store and to all stores that share its prefix.
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

      /// @brief Constructs a tile store rooted at @p prefix.
      /// @param prefix The directory under which the tiles live.
      explicit TileStoreT(std::filesystem::path prefix) :
        prefix(std::move(prefix))
      {}

      /// @brief The root directory of the store.
      [[nodiscard]] const std::filesystem::path& root() const
      {
        return prefix;
      }

      /// @brief Encodes a tile index (see encode_tile_index).
      static std::string encode_index(uint64_t n)
      {
        return encode_tile_index(n);
      }

      /// @brief The filesystem path of a tile.
      [[nodiscard]] std::filesystem::path tile_path(const TileRef& ref) const
      {
        return prefix /
          ("tile/" + std::to_string((unsigned)ref.level) + "/" +
           encode_tile_index(ref.index));
      }

      /// @brief The filesystem path of an entry bundle.
      [[nodiscard]] std::filesystem::path entries_path(uint64_t index) const
      {
        return prefix / ("tile/entries/" + encode_tile_index(index));
      }

      /// @brief Whether a full tile exists on disk.
      [[nodiscard]] bool has_full_tile(uint8_t level, uint64_t index) const
      {
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
        std::vector<uint8_t> bytes = read_file(tile_path(ref));
        const size_t expected = (size_t)TILE_WIDTH * HASH_SIZE;
        if (bytes.size() != expected)
        {
          throw std::runtime_error("unexpected tile size");
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
          (void)decode_entries(read_file(path), TILE_WIDTH);
          return true;
        }
        catch (const std::exception&)
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
        return decode_entries(read_file(entries_path(index)), TILE_WIDTH);
      }

      /// @brief Encodes log entries into the tlog-tiles entry-bundle format.
      static std::vector<uint8_t> encode_entries(
        const std::vector<std::vector<uint8_t>>& entries)
      {
        std::vector<uint8_t> bytes;
        for (const auto& e : entries)
        {
          if (e.size() > 0xFFFF)
          {
            throw std::runtime_error(
              "entry too large for uint16 length prefix");
          }
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

    protected:
      using DirectorySync = std::function<void(const std::filesystem::path&)>;

      TileStoreT(std::filesystem::path prefix, DirectorySync directory_sync) :
        prefix(std::move(prefix)), directory_sync(std::move(directory_sync))
      {}

      /// @brief The root directory of the store.
      std::filesystem::path prefix;

      DirectorySync directory_sync;
      std::set<std::filesystem::path> durable_directory_entries;
      std::set<std::filesystem::path> durable_directory_contents;

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
      static std::vector<uint8_t> read_file(const std::filesystem::path& path)
      {
        std::ifstream f(path, std::ios::binary);
        if (!f.good())
        {
          throw std::runtime_error("cannot open file: " + path.string());
        }
        return {
          std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
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
        write_and_sync_file(tmp, bytes);
        const auto parent = directory_or_dot(path.parent_path());
        durable_directory_contents.erase(parent);
        replace_file(tmp, path);
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
        if (durable_directory_entries.count(directory) != 0)
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
          throw std::runtime_error(
            "cannot inspect directory " + directory.string() + ": " +
            ec.message());
        }
        if (exists)
        {
          const bool is_directory =
            std::filesystem::is_directory(directory, ec);
          if (ec)
          {
            throw std::runtime_error(
              "cannot inspect directory " + directory.string() + ": " +
              ec.message());
          }
          if (!is_directory)
          {
            throw std::runtime_error(
              "cannot create directory " + directory.string() +
              ": path exists and is not a directory");
          }
        }
        else
        {
          const bool created = std::filesystem::create_directory(directory, ec);
          if (ec)
          {
            throw std::runtime_error(
              "cannot create directory " + directory.string() + ": " +
              ec.message());
          }
          if (!created && !std::filesystem::is_directory(directory, ec))
          {
            throw std::runtime_error(
              "cannot create directory " + directory.string() +
              (ec ? ": " + ec.message() : ""));
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
        if (durable_directory_contents.count(path) != 0)
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

        void dismiss()
        {
          active = false;
        }

      private:
        std::filesystem::path path;
        bool active = true;
      };

      static std::filesystem::path temp_path(const std::filesystem::path& path)
      {
        static std::atomic<uint64_t> counter{0};
        const auto stamp =
          std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path tmp = path;
        tmp += ".tmp." + std::to_string(process_id()) + "." +
          std::to_string((uint64_t)stamp) + "." +
          std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        return tmp;
      }

      static uint64_t process_id()
      {
#ifdef _WIN32
        return (uint64_t)GetCurrentProcessId();
#else
        return (uint64_t)::getpid();
#endif
      }

      static std::string system_error_message(const std::string& what)
      {
#ifdef _WIN32
        return what + ": error " + std::to_string(GetLastError());
#else
        return what + ": " + std::strerror(errno);
#endif
      }

      static void require_write_progress(
        size_t written, const std::filesystem::path& path)
      {
        if (written == 0)
        {
          throw std::runtime_error("short write: " + path.string());
        }
      }

      static void write_and_sync_file(
        const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
      {
#ifdef _WIN32
        HANDLE handle = CreateFileW(
          path.wstring().c_str(),
          GENERIC_WRITE,
          0,
          nullptr,
          CREATE_ALWAYS,
          FILE_ATTRIBUTE_NORMAL,
          nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
          throw std::runtime_error("cannot open file: " + path.string());
        }
        bool close_handle = true;
        try
        {
          size_t written = 0;
          while (written < bytes.size())
          {
            const auto remaining = bytes.size() - written;
            const auto chunk = (DWORD)std::min<size_t>(
              remaining, (size_t)std::numeric_limits<DWORD>::max());
            DWORD done = 0;
            if (!WriteFile(
                  handle, bytes.data() + written, chunk, &done, nullptr))
            {
              throw std::runtime_error(
                system_error_message("error writing file " + path.string()));
            }
            require_write_progress((size_t)done, path);
            written += done;
          }
          if (!FlushFileBuffers(handle))
          {
            throw std::runtime_error(
              system_error_message("error syncing file " + path.string()));
          }
          close_handle = false;
          if (!CloseHandle(handle))
          {
            throw std::runtime_error(
              system_error_message("error closing file " + path.string()));
          }
        }
        catch (...)
        {
          if (close_handle)
          {
            CloseHandle(handle);
          }
          throw;
        }
#else
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
#  ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#  endif
        int fd = ::open(path.c_str(), flags, 0666);
        if (fd < 0)
        {
          throw std::runtime_error(
            system_error_message("cannot open file " + path.string()));
        }
        try
        {
          size_t written = 0;
          while (written < bytes.size())
          {
            const ssize_t done =
              ::write(fd, bytes.data() + written, bytes.size() - written);
            if (done < 0)
            {
              if (errno == EINTR)
              {
                continue;
              }
              throw std::runtime_error(
                system_error_message("error writing file " + path.string()));
            }
            require_write_progress((size_t)done, path);
            written += (size_t)done;
          }
          if (::fsync(fd) != 0)
          {
            throw std::runtime_error(
              system_error_message("error syncing file " + path.string()));
          }
          if (::close(fd) != 0)
          {
            fd = -1;
            throw std::runtime_error(
              system_error_message("error closing file " + path.string()));
          }
          fd = -1;
        }
        catch (...)
        {
          if (fd >= 0)
          {
            ::close(fd);
          }
          throw;
        }
#endif
      }

      static void replace_file(
        const std::filesystem::path& tmp, const std::filesystem::path& path)
      {
#ifdef _WIN32
        if (!MoveFileExW(
              tmp.wstring().c_str(),
              path.wstring().c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
          throw std::runtime_error(system_error_message(
            "cannot rename temp file " + tmp.string() + " to " +
            path.string()));
        }
#else
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
          throw std::runtime_error(
            "cannot rename temp file " + tmp.string() + " to " + path.string() +
            ": " + ec.message());
        }
#endif
      }

      void sync_directory_for_durability(const std::filesystem::path& path)
      {
        if (directory_sync)
        {
          directory_sync(path);
          return;
        }
        sync_directory_on_disk(path);
      }

      static void sync_directory_on_disk(const std::filesystem::path& path)
      {
#ifndef _WIN32
        int flags = O_RDONLY;
#  ifdef O_DIRECTORY
        flags |= O_DIRECTORY;
#  endif
#  ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#  endif
        const int fd = ::open(path.c_str(), flags);
        if (fd < 0)
        {
          throw std::runtime_error(
            system_error_message("cannot open directory " + path.string()));
        }
        if (::fsync(fd) != 0)
        {
          const std::string message =
            system_error_message("error syncing directory " + path.string());
          ::close(fd);
          throw std::runtime_error(message);
        }
        if (::close(fd) != 0)
        {
          throw std::runtime_error(
            system_error_message("error closing directory " + path.string()));
        }
#else
        (void)path;
#endif
      }
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
      /// Tiles are always rolled up at every level (0..63), so the on-disk set
      /// always contains the higher-level roll-ups that proof generation relies
      /// on.
      Stats write_up_to(uint64_t size, const LeafFn& leaf_at)
      {
        Stats stats;
        store.begin_write_attempt();

        // tlog-tiles defines levels 0..63; the loop stops early once a level
        // has no complete entries (see the entries == 0 break below).
        for (uint8_t level = 0; level <= 63; level++)
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
        const unsigned shift = 8U * (unsigned)level;
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

    /// @brief Abstract source of Merkle subtree roots for proof generation.
    /// @note Implementations resolve the root of a complete (balanced) subtree
    /// from tiles, from an in-memory tree, or from a combination of the two.
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    struct HashSourceT
    {
      /// @brief The type of hashes resolved.
      using Hash = HashT<HASH_SIZE>;

      virtual ~HashSourceT() = default;

      /// @brief Resolves MTH(D[index << level : (index + 1) << level]).
      /// @param level The subtree height (the subtree spans 2**level leaves)
      /// @param index The subtree index at that height
      /// @param out Set to the subtree root on success
      /// @return Whether the complete, balanced subtree could be resolved
      virtual bool subtree_root(
        uint8_t level, uint64_t index, Hash& out) const = 0;

      /// @brief Resolves the level-0 leaf hash at @p index.
      virtual bool leaf(uint64_t index, Hash& out) const
      {
        return subtree_root(0, index, out);
      }
    };

    /// @brief Resolves subtree roots from tlog-tiles tile files.
    /// @note @p available_size is rounded down to a whole number of full tiles:
    /// only complete, durably-written full tiles are read. A complete subtree
    /// within that full-tile prefix is resolvable; anything reaching into the
    /// incomplete frontier yields false so that a proof builder can fall back
    /// to another source (e.g. an in-memory tree).
    /// @warning No internal synchronization is provided. Even const operations
    /// update the internal LRU cache, so callers must serialize all access to a
    /// shared source.
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class TileHashSourceT : public HashSourceT<HASH_SIZE, HASH_FUNCTION>
    {
    public:
      using Hash = HashT<HASH_SIZE>;
      using Store = TileStoreT<HASH_SIZE, HASH_FUNCTION>;

      /// @brief Constructs a source over @p store for trees up to
      /// @p available_size leaves. @p available_size is rounded down to a whole
      /// number of full tiles, since only full tiles are durable.
      TileHashSourceT(const Store& store, uint64_t available_size) :
        store(store), available_size((available_size / TILE_WIDTH) * TILE_WIDTH)
      {
        tile_cache.reserve(TILE_CACHE_SIZE);
      }

      bool subtree_root(uint8_t level, uint64_t index, Hash& out) const override
      {
        // The subtree covers leaves [index << level, (index + 1) << level). It
        // is resolvable only when it lies entirely within the full-tile-covered
        // prefix; the incomplete frontier is served from another source.
        if (level >= 64 || index >= (available_size >> level))
        {
          return false;
        }
        resolve(level, index, out);
        return true;
      }

    protected:
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      const Store& store;
      uint64_t available_size; // full-tile prefix length (a multiple of WIDTH)

      /// @brief Combines the @p span entries at @p off of @p tile into a root.
      static Hash roll_up(
        const std::vector<Hash>& tile, uint64_t off, uint64_t span)
      {
        if (span == 1)
        {
          return tile.at(off);
        }
        return perfect_root<HASH_SIZE, HASH_FUNCTION>(std::vector<Hash>(
          tile.begin() + (std::ptrdiff_t)off,
          tile.begin() + (std::ptrdiff_t)(off + span)));
      }

      /// @brief Resolves a complete subtree known to lie within the full-tile
      /// prefix, reading the highest-level full tile that holds it (and rolling
      /// up); descends to lower full tiles when a higher-level full tile has not
      /// completed. Terminates because full level-0 tiles always cover the
      /// prefix.
      void resolve(uint8_t level, uint64_t index, Hash& out) const
      {
        if (level <= TILE_HEIGHT)
        {
          // Spans 2**level <= TILE_WIDTH leaves: held by one level-0 tile.
          const uint64_t span = (uint64_t)1 << level;
          const uint64_t start = index << level;
          const std::vector<Hash> tile =
            read_tile(TileRef{0, start / TILE_WIDTH});
          out = roll_up(tile, start % TILE_WIDTH, span);
          return;
        }

        const uint8_t L = level / TILE_HEIGHT;
        const uint8_t r = level % TILE_HEIGHT;
        const uint64_t first = index << r; // first level-L entry
        const uint64_t n = first / TILE_WIDTH; // level-L tile index
        const unsigned full_shift = 8U * ((unsigned)L + 1U);
        const uint64_t full_tiles =
          full_shift >= 64 ? 0 : (available_size >> full_shift);

        if (n < full_tiles)
        {
          // One full level-L tile holds all 2**r entries of this subtree.
          const std::vector<Hash> tile = read_tile(TileRef{L, n});
          out = roll_up(tile, first % TILE_WIDTH, (uint64_t)1 << r);
          return;
        }

        // No full level-L tile here: split into two level-(level-1) subtrees.
        Hash lo;
        Hash hi;
        resolve((uint8_t)(level - 1), index * 2, lo);
        resolve((uint8_t)(level - 1), index * 2 + 1, hi);
        HASH_FUNCTION(lo, hi, out);
      }

      struct TileCacheEntry
      {
        TileRef ref;
        std::vector<Hash> hashes;
      };

      static constexpr size_t TILE_CACHE_SIZE = 64;
      mutable std::vector<TileCacheEntry> tile_cache;

      std::vector<Hash> read_tile(const TileRef& ref) const
      {
        for (auto it = tile_cache.begin(); it != tile_cache.end(); it++)
        {
          if (it->ref.level == ref.level && it->ref.index == ref.index)
          {
            TileCacheEntry entry = std::move(*it);
            tile_cache.erase(it);
            std::vector<Hash> hashes = entry.hashes;
            tile_cache.push_back(std::move(entry));
            return hashes;
          }
        }

        if (tile_cache.size() >= TILE_CACHE_SIZE)
        {
          tile_cache.erase(tile_cache.begin());
        }
        tile_cache.push_back(TileCacheEntry{ref, store.read_tile(ref)});
        return tile_cache.back().hashes;
      }
    };

    /// @brief Builds and verifies inclusion and consistency proofs.
    /// @note Proofs are assembled from a HashSource using the tree's
    /// HASH_FUNCTION, so an inclusion proof is byte-identical to the one
    /// produced by merkle::TreeT::path()/past_path() and verifies with
    /// PathT::verify().
    /// @warning Thread safety is inherited from the supplied HashSource. Callers
    /// must serialize operations when the source is shared.
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class ProofEngineT
    {
    public:
      using Hash = HashT<HASH_SIZE>;
      using Path = PathT<HASH_SIZE, HASH_FUNCTION>;
      using Source = HashSourceT<HASH_SIZE, HASH_FUNCTION>;

      explicit ProofEngineT(const Source& source) : source(source) {}

      /// @brief The Merkle root of a tree of @p size leaves.
      Hash root(uint64_t size) const
      {
        if (size == 0)
        {
          throw std::runtime_error("empty tree has no root");
        }
        Hash out;
        if (!mth_range(0, size, out))
        {
          throw std::runtime_error("unresolved subtree while computing root");
        }
        return out;
      }

      /// @brief Inclusion proof for leaf @p index in a tree of @p size leaves.
      /// @note Equivalent to TreeT::path(index) when size == num_leaves(), and
      /// to TreeT::past_path(index, size - 1) otherwise.
      std::shared_ptr<Path> inclusion_proof(uint64_t index, uint64_t size) const
      {
        if (index >= size)
        {
          throw std::runtime_error("leaf index out of bounds");
        }

        std::list<typename Path::Element> elements; // leaf -> root order
        uint64_t lo = 0;
        uint64_t hi = size;
        while (hi - lo > 1)
        {
          const uint64_t k = largest_pow2_lt(hi - lo);
          typename Path::Element e;
          if (index - lo < k)
          {
            if (!mth_range(lo + k, hi, e.hash))
            {
              throw std::runtime_error("unresolved subtree in inclusion proof");
            }
            e.direction = Path::PATH_RIGHT;
            hi = lo + k;
          }
          else
          {
            if (!mth_range(lo, lo + k, e.hash))
            {
              throw std::runtime_error("unresolved subtree in inclusion proof");
            }
            e.direction = Path::PATH_LEFT;
            lo = lo + k;
          }
          elements.push_front(std::move(e));
        }

        Hash leaf;
        if (!source.leaf(index, leaf))
        {
          throw std::runtime_error("unresolved leaf in inclusion proof");
        }
        return std::make_shared<Path>(
          leaf, index, std::move(elements), size - 1);
      }

      /// @brief Consistency proof that a tree of @p m leaves is a prefix of a
      /// tree of @p n leaves (RFC 6962).
      std::vector<Hash> consistency_proof(uint64_t m, uint64_t n) const
      {
        if (m == 0 || m > n)
        {
          throw std::runtime_error("invalid consistency proof sizes");
        }
        std::vector<Hash> proof;
        if (m == n)
        {
          return proof;
        }
        subproof(m, 0, n, true, proof);
        return proof;
      }

      /// @brief Consistency proof between the trees whose last leaves are at
      /// indices @p first_index and @p second_index (first_index <=
      /// second_index).
      /// @note Equivalent to consistency_proof(first_index + 1,
      /// second_index + 1): it proves the tree of the first first_index + 1
      /// leaves is a prefix of the tree of the first second_index + 1 leaves.
      std::vector<Hash> consistency_proof_from_indices(
        uint64_t first_index, uint64_t second_index) const
      {
        return consistency_proof(first_index + 1, second_index + 1);
      }

      /// @brief Verifies an RFC 6962 consistency proof reconciling the roots of
      /// trees of @p m and @p n leaves.
      static bool verify_consistency(
        uint64_t m,
        uint64_t n,
        const Hash& first_hash,
        const Hash& second_hash,
        std::vector<Hash> proof)
      {
        if (m > n)
        {
          return false;
        }
        if (m == n)
        {
          return proof.empty() && first_hash == second_hash;
        }
        if (m == 0)
        {
          return proof.empty();
        }

        if (is_pow2(m))
        {
          proof.insert(proof.begin(), first_hash);
        }
        if (proof.empty())
        {
          return false;
        }

        uint64_t fn = m - 1;
        uint64_t sn = n - 1;
        while ((fn & 1) != 0)
        {
          fn >>= 1;
          sn >>= 1;
        }

        Hash fr = proof[0];
        Hash sr = proof[0];
        for (size_t i = 1; i < proof.size(); i++)
        {
          if (sn == 0)
          {
            return false;
          }
          const Hash& c = proof[i];
          if ((fn & 1) != 0 || fn == sn)
          {
            HASH_FUNCTION(c, fr, fr);
            HASH_FUNCTION(c, sr, sr);
            if ((fn & 1) == 0)
            {
              while ((fn & 1) == 0 && fn != 0)
              {
                fn >>= 1;
                sn >>= 1;
              }
            }
          }
          else
          {
            HASH_FUNCTION(sr, c, sr);
          }
          fn >>= 1;
          sn >>= 1;
        }

        return fr == first_hash && sr == second_hash && sn == 0;
      }

    protected:
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      const Source& source;

      static bool is_pow2(uint64_t n)
      {
        return n != 0 && (n & (n - 1)) == 0;
      }

      static uint64_t largest_pow2_lt(uint64_t n)
      {
        uint64_t k = 1;
        while (k <= (n - 1) / 2)
        {
          k <<= 1;
        }
        return k;
      }

      static uint8_t log2_exact(uint64_t n)
      {
        uint8_t r = 0;
        while (n > 1)
        {
          n >>= 1;
          r++;
        }
        return r;
      }

      /// @brief MTH(D[a:b]) via the source; falls back to splitting when a
      /// perfect subtree cannot be resolved directly.
      bool mth_range(uint64_t a, uint64_t b, Hash& out) const
      {
        const uint64_t w = b - a;
        if (w == 0)
        {
          return false;
        }
        if (w == 1)
        {
          return source.leaf(a, out);
        }
        if (is_pow2(w) && (a % w == 0))
        {
          if (source.subtree_root(log2_exact(w), a / w, out))
          {
            return true;
          }
        }
        const uint64_t k = largest_pow2_lt(w);
        Hash left;
        Hash right;
        if (!mth_range(a, a + k, left) || !mth_range(a + k, b, right))
        {
          return false;
        }
        HASH_FUNCTION(left, right, out);
        return true;
      }

      void subproof(
        uint64_t m,
        uint64_t lo,
        uint64_t hi,
        bool complete,
        std::vector<Hash>& proof) const
      {
        if (m == hi - lo)
        {
          if (!complete)
          {
            Hash h;
            if (!mth_range(lo, hi, h))
            {
              throw std::runtime_error(
                "unresolved subtree in consistency proof");
            }
            proof.push_back(h);
          }
          return;
        }
        const uint64_t k = largest_pow2_lt(hi - lo);
        Hash h;
        if (m <= k)
        {
          subproof(m, lo, lo + k, complete, proof);
          if (!mth_range(lo + k, hi, h))
          {
            throw std::runtime_error("unresolved subtree in consistency proof");
          }
        }
        else
        {
          subproof(m - k, lo + k, hi, false, proof);
          if (!mth_range(lo, lo + k, h))
          {
            throw std::runtime_error("unresolved subtree in consistency proof");
          }
        }
        proof.push_back(h);
      }
    };

    /// @brief Resolves subtree roots from an in-memory merkle::TreeT.
    /// @note Resolves only complete subtrees that are fully resident (not
    /// flushed), returning false otherwise so that a builder can fall back to
    /// another source. Performs no hashing changes (see TreeT::subtree_root).
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class MemoryHashSourceT : public HashSourceT<HASH_SIZE, HASH_FUNCTION>
    {
    public:
      using Hash = HashT<HASH_SIZE>;
      using Tree = TreeT<HASH_SIZE, HASH_FUNCTION>;

      explicit MemoryHashSourceT(Tree& tree) : tree(tree) {}

      bool subtree_root(uint8_t level, uint64_t index, Hash& out) const override
      {
        return tree.subtree_root(level, (size_t)index, out);
      }

    protected:
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      Tree& tree;
    };

    /// @brief Resolves subtree roots from a primary source, falling back to a
    /// secondary source.
    /// @note Used to combine an in-memory tree (primary: no I/O, serves the
    /// resident frontier) with tile files (secondary: serve the flushed past).
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class CombinedHashSourceT : public HashSourceT<HASH_SIZE, HASH_FUNCTION>
    {
    public:
      using Hash = HashT<HASH_SIZE>;
      using Source = HashSourceT<HASH_SIZE, HASH_FUNCTION>;

      CombinedHashSourceT(const Source& primary, const Source& secondary) :
        primary(primary), secondary(secondary)
      {}

      bool subtree_root(uint8_t level, uint64_t index, Hash& out) const override
      {
        return primary.subtree_root(level, index, out) ||
          secondary.subtree_root(level, index, out);
      }

    protected:
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      const Source& primary;
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      const Source& secondary;
    };

    /// @brief A merkle tree backed by tlog-tiles storage.
    /// @note Appends grow an in-memory tree; flush() durably writes only full
    /// (balanced) tiles, so the incomplete frontier is never tiled and stays
    /// resident in memory. Compaction (dropping from memory the leaves already
    /// covered by a full tile) is optional: enable it per flush with
    /// Config::compact_on_flush, or call compact() explicitly; it never drops
    /// the un-tiled frontier. Proofs are served from the combination of the
    /// resident tree (frontier) and the full tiles (compacted past).
    /// @note TiledTree creates a new tiled tree and cannot reopen one from tile
    /// files alone. Construction rejects a non-empty tile namespace because
    /// the files do not identify their tree or record enough state to restore
    /// it. Use TileWriter directly only when the caller owns and restores that
    /// state.
    /// @warning No internal synchronization is provided. Callers must serialize
    /// all access to a shared tree, including proof operations.
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class TiledTreeT
    {
    public:
      using Hash = HashT<HASH_SIZE>;
      using Tree = TreeT<HASH_SIZE, HASH_FUNCTION>;
      using Path = PathT<HASH_SIZE, HASH_FUNCTION>;
      using Store = TileStoreT<HASH_SIZE, HASH_FUNCTION>;
      using Writer = TileWriterT<HASH_SIZE, HASH_FUNCTION>;
      using Stats = typename Writer::Stats;

      /// @brief Configuration for a tiled tree.
      struct Config
      {
        /// @brief Root directory for a new tiled tree.
        /// @note The directory itself may exist, but its tile subdirectory must
        /// be absent or empty.
        std::filesystem::path prefix;

        /// @brief Number of most-recent leaves to keep resident when
        /// compacting (i.e. never dropped from memory).
        /// @note Compaction may retain one additional tiled boundary leaf so
        /// rollback to exactly immutable_size() remains possible.
        uint64_t retention_margin = 0;

        /// @brief If set, flush() compacts after writing tiles, dropping
        /// from memory the leaves already covered by a full tile. Off by
        /// default: tiles are written but the tree keeps every leaf resident.
        bool compact_on_flush = false;
      };

      explicit TiledTreeT(Config config) :
        config(std::move(config)), store(this->config.prefix), writer(store)
      {
        require_empty_tile_namespace();
      }

      TiledTreeT(const TiledTreeT&) = delete;
      TiledTreeT& operator=(const TiledTreeT&) = delete;

      /// @brief Moves a tiled tree, rebinding its writer to the moved store.
      TiledTreeT(TiledTreeT&& other) noexcept :
        config(std::move(other.config)),
        store(std::move(other.store)),
        writer(store),
        tree(std::move(other.tree)),
        tiles_size(std::exchange(other.tiles_size, 0)),
        sealed_size(std::exchange(other.sealed_size, 0))
      {}

      TiledTreeT& operator=(TiledTreeT&&) = delete;

      /// @brief Appends a leaf hash.
      void append(const Hash& leaf_hash)
      {
        tree.insert(leaf_hash);
      }

      /// @brief The number of leaves (including flushed ones).
      [[nodiscard]] uint64_t size() const
      {
        return tree.num_leaves();
      }

      /// @brief The current Merkle root.
      Hash root()
      {
        return tree.root();
      }

      /// @brief The number of leaves covered by the last fully successful flush.
      /// @note This is always a multiple of TILE_WIDTH. It advances only after
      /// every required tile level has been written successfully, and controls
      /// proof reads and compaction.
      [[nodiscard]] uint64_t flushed_size() const
      {
        return tiles_size;
      }

      /// @brief The rollback seal for ranges a flush may have published.
      /// @note A flush seals its full-tile boundary before writing. If the write
      /// fails, this may exceed flushed_size(); keep the same tree contents and
      /// retry the flush.
      [[nodiscard]] uint64_t immutable_size() const
      {
        return sealed_size;
      }

      /// @brief Access to the underlying tree.
      /// @warning Mutating the tree directly bypasses tiled-tree bookkeeping.
      /// In particular, direct retraction can make flushed_size() and
      /// immutable_size() exceed size() and can make flushed_size() regress.
      /// Use TiledTreeT operations whenever they are available.
      Tree& tree_ref()
      {
        return tree;
      }

      /// @brief Access to the underlying tile store.
      /// @warning Files written or changed through this reference are trusted
      /// by later flushes without checking that their hashes match this tree.
      /// Mismatched files can silently invalidate proofs after compaction.
      Store& store_ref()
      {
        return store;
      }

      /// @brief Writes newly-complete full tiles to disk; compacts only if
      /// Config::compact_on_flush is set.
      /// @return Counts of the full tiles written by this flush
      /// @note The full-tile boundary is made immutable before any tile write.
      /// Only after every required tile level succeeds does flushed_size()
      /// advance to that boundary. On failure, immutable_size() may advance
      /// while flushed_size() does not; the tree remains resident and the flush
      /// can be retried without rewriting finalized tiles.
      Stats flush()
      {
        Stats stats;
        const uint64_t n = tree.num_leaves();
        if (n == 0)
        {
          return stats;
        }

        const uint64_t covered = (n / TILE_WIDTH) * TILE_WIDTH;
        if (covered > sealed_size)
        {
          sealed_size = covered;
        }

        stats = writer.write_up_to(n, [this](uint64_t i) -> const Hash& {
          return tree.leaf((size_t)i);
        });
        tiles_size = covered;

        if (config.compact_on_flush)
        {
          compact();
        }
        return stats;
      }

      /// @brief Drops old leaves covered by durably-written full tiles, keeping
      /// retention_margin recent leaves and one tiled boundary leaf.
      /// @return The new minimum (smallest still-resident) leaf index
      /// @note Only leaves covered by a full tile are dropped, so the un-tiled
      /// frontier is always retained in memory and inclusion/consistency proofs
      /// remain available (the past from tiles, the frontier from memory). The
      /// leaf at flushed_size() - 1 also remains resident so retract_to() can
      /// represent a tree whose size is exactly immutable_size(). Has no effect
      /// until tiling has produced full tiles.
      uint64_t compact()
      {
        const uint64_t covered = (tiles_size / TILE_WIDTH) * TILE_WIDTH;
        uint64_t target = covered > config.retention_margin ?
          covered - config.retention_margin :
          0;
        target = (target / TILE_WIDTH) * TILE_WIDTH;
        // TreeT cannot retract below min_index(). Keep the final tiled leaf
        // resident so rollback to a size of exactly immutable_size() remains
        // representable after compaction.
        if (covered > 0 && target == covered)
        {
          target--;
        }
        if (target > tree.min_index())
        {
          tree.flush_to((size_t)target);
        }
        return tree.min_index();
      }

      /// @brief Rolls the tree back so that @p index becomes the last leaf,
      /// removing all leaves after it (same semantics as TreeT::retract_to).
      /// @note Only full tiles are immutable: this throws if the resulting size
      /// would be smaller than immutable_size(). A failed flush may advance
      /// immutable_size() without advancing flushed_size().
      void retract_to(size_t index)
      {
        if ((uint64_t)index + 1 < sealed_size)
        {
          throw std::runtime_error(
            "TiledTree::retract_to: cannot roll back entries sealed for "
            "immutable tiles (resulting size < immutable size)");
        }
        tree.retract_to(index);
      }

      /// @brief Inclusion proof for @p index in a tree of @p proof_size leaves.
      /// @note Served from tiles (flushed past) combined with the resident tree
      /// (recent frontier); @p proof_size may exceed flushed_size().
      std::shared_ptr<Path> inclusion_proof(uint64_t index, uint64_t proof_size)
      {
        return with_engine([&](const auto& engine) {
          return engine.inclusion_proof(index, proof_size);
        });
      }

      /// @brief Consistency proof between tree sizes @p m and @p n.
      std::vector<Hash> consistency_proof(uint64_t m, uint64_t n)
      {
        return with_engine(
          [&](const auto& engine) { return engine.consistency_proof(m, n); });
      }

      /// @brief Consistency proof between the trees whose last leaves are at
      /// indices @p first_index and @p second_index (first_index <=
      /// second_index).
      /// @note Equivalent to consistency_proof(first_index + 1,
      /// second_index + 1).
      std::vector<Hash> consistency_proof_from_indices(
        uint64_t first_index, uint64_t second_index)
      {
        return consistency_proof(first_index + 1, second_index + 1);
      }

    protected:
      Config config;
      Store store;
      Writer writer;
      Tree tree;
      uint64_t tiles_size = 0;
      uint64_t sealed_size = 0;

      void require_empty_tile_namespace() const
      {
        const auto tile_root = store.root() / "tile";
        std::error_code ec;
        const bool exists = std::filesystem::exists(tile_root, ec);
        if (ec)
        {
          throw std::runtime_error(
            "TiledTree: cannot inspect tile namespace " + tile_root.string() +
            ": " + ec.message());
        }
        if (!exists)
        {
          return;
        }

        const bool is_directory = std::filesystem::is_directory(tile_root, ec);
        if (ec)
        {
          throw std::runtime_error(
            "TiledTree: cannot inspect tile namespace " + tile_root.string() +
            ": " + ec.message());
        }
        const bool is_empty =
          is_directory && std::filesystem::is_empty(tile_root, ec);
        if (ec)
        {
          throw std::runtime_error(
            "TiledTree: cannot inspect tile namespace " + tile_root.string() +
            ": " + ec.message());
        }
        if (!is_empty)
        {
          throw std::runtime_error(
            "TiledTree: tile namespace is not empty; reopening an existing "
            "tiled tree is not supported");
        }
      }

      /// @brief Builds a proof engine over the combined resident-tree
      /// (frontier) and full-tile (flushed past) source, and invokes @p fn with
      /// it.
      /// @note The sources and engine are stack-local; @p fn must consume the
      /// engine before returning (proofs are returned by value, holding hash
      /// copies, so the result outlives the engine).
      template <typename Fn>
      auto with_engine(Fn fn)
      {
        MemoryHashSourceT<HASH_SIZE, HASH_FUNCTION> mem(tree);
        TileHashSourceT<HASH_SIZE, HASH_FUNCTION> tile_src(store, tiles_size);
        CombinedHashSourceT<HASH_SIZE, HASH_FUNCTION> combined(mem, tile_src);
        ProofEngineT<HASH_SIZE, HASH_FUNCTION> engine(combined);
        return fn(engine);
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
    using TileStore = TileStoreT<32, sha256_compress>;

    /// @brief Default tile writer (SHA256, default hash function).
    using TileWriter = TileWriterT<32, sha256_compress>;

    /// @brief Default abstract hash source (SHA256, default hash function).
    using HashSource = HashSourceT<32, sha256_compress>;

    /// @brief Default tile-backed hash source (SHA256, default hash function).
    using TileHashSource = TileHashSourceT<32, sha256_compress>;

    /// @brief Default proof engine (SHA256, default hash function).
    using ProofEngine = ProofEngineT<32, sha256_compress>;

    /// @brief Default in-memory hash source (SHA256, default hash function).
    using MemoryHashSource = MemoryHashSourceT<32, sha256_compress>;

    /// @brief Default combined hash source (SHA256, default hash function).
    using CombinedHashSource = CombinedHashSourceT<32, sha256_compress>;

    /// @brief Default tiled tree (SHA256, default hash function).
    using TiledTree = TiledTreeT<32, sha256_compress>;

    /// @brief Default entry-bundle writer (SHA256, default hash function).
    using EntryBundleWriter = EntryBundleWriterT<32, sha256_compress>;

#ifdef HAVE_OPENSSL
    /// @brief SHA384 tile store.
    using TileStore384 = TileStoreT<48, sha384_openssl>;

    /// @brief SHA512 tile store.
    using TileStore512 = TileStoreT<64, sha512_openssl>;

    /// @brief SHA384 tile writer.
    using TileWriter384 = TileWriterT<48, sha384_openssl>;

    /// @brief SHA512 tile writer.
    using TileWriter512 = TileWriterT<64, sha512_openssl>;

    /// @brief SHA384 hash source, tile-backed source and proof engine.
    using HashSource384 = HashSourceT<48, sha384_openssl>;
    using TileHashSource384 = TileHashSourceT<48, sha384_openssl>;
    using ProofEngine384 = ProofEngineT<48, sha384_openssl>;

    /// @brief SHA512 hash source, tile-backed source and proof engine.
    using HashSource512 = HashSourceT<64, sha512_openssl>;
    using TileHashSource512 = TileHashSourceT<64, sha512_openssl>;
    using ProofEngine512 = ProofEngineT<64, sha512_openssl>;

    /// @brief SHA384 memory/combined sources and tiled tree.
    using MemoryHashSource384 = MemoryHashSourceT<48, sha384_openssl>;
    using CombinedHashSource384 = CombinedHashSourceT<48, sha384_openssl>;
    using TiledTree384 = TiledTreeT<48, sha384_openssl>;

    /// @brief SHA512 memory/combined sources and tiled tree.
    using MemoryHashSource512 = MemoryHashSourceT<64, sha512_openssl>;
    using CombinedHashSource512 = CombinedHashSourceT<64, sha512_openssl>;
    using TiledTree512 = TiledTreeT<64, sha512_openssl>;

    /// @brief SHA384/512 entry-bundle writers.
    using EntryBundleWriter384 = EntryBundleWriterT<48, sha384_openssl>;
    using EntryBundleWriter512 = EntryBundleWriterT<64, sha512_openssl>;
#endif
  }
}
