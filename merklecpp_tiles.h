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
// Callers must serialize all operations on shared objects and store prefixes,
// including const proof operations that update the tile cache.

namespace merkle // NOLINT(modernize-concat-nested-namespaces)
{
  namespace tiles
  {
    /// @brief Number of tree levels spanned by a single tile.
    static constexpr uint16_t TILE_HEIGHT = 8;

    /// @brief Number of hashes in a full tile (2**TILE_HEIGHT).
    static constexpr uint16_t TILE_WIDTH = uint16_t{1U << TILE_HEIGHT};

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
        return hash_algorithm_short_name + "-" + std::to_string(TILE_WIDTH) +
          "w";
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
        if constexpr (HASH_SIZE == 32)
        {
          if constexpr (HASH_FUNCTION == sha256_compress)
          {
            return "sha256";
          }
#ifdef HAVE_OPENSSL
          if constexpr (HASH_FUNCTION == sha256_openssl)
          {
            return "sha256";
          }
#endif
        }
#ifdef HAVE_OPENSSL
        else if constexpr (HASH_SIZE == 48)
        {
          if constexpr (HASH_FUNCTION == sha384_openssl)
          {
            return "sha384";
          }
        }
        else if constexpr (HASH_SIZE == 64)
        {
          if constexpr (HASH_FUNCTION == sha512_openssl)
          {
            return "sha512";
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
          (hash_algorithm_short_name == "sha256" && HASH_SIZE != 32) ||
          (hash_algorithm_short_name == "sha384" && HASH_SIZE != 48) ||
          (hash_algorithm_short_name == "sha512" && HASH_SIZE != 64))
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
          if (!CloseHandle(handle))
          {
            throw std::runtime_error(
              system_error_message("error closing file " + path.string()));
          }
          close_handle = false;
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

    /// @brief Default tile store (SHA256, default hash function).
    using TileStore = TileStoreT<32, sha256_compress>;

  }
}
