// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "merklecpp.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Tiled storage for merklecpp trees, following the C2SP tlog-tiles layout
// (https://c2sp.org/tlog-tiles). The tile geometry, path encoding and partial
// tile rules match the specification; the hash values stored in tiles are
// produced by the tree's existing HASH_FUNCTION, so tile-derived proofs are
// byte-identical to those produced by merkle::TreeT (see
// doc/design/tlog-tiles.md).

namespace merkle
{
  namespace tiles
  {
    /// @brief Number of tree levels spanned by a single tile.
    static constexpr uint16_t TILE_HEIGHT = 8;

    /// @brief Number of hashes in a full tile (2**TILE_HEIGHT).
    static constexpr uint16_t TILE_WIDTH = 256;

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

    /// @brief Identifies a single tile within a tiled log.
    struct TileRef
    {
      /// @brief The level of the tile (0 == leaf hashes).
      uint8_t level = 0;

      /// @brief The index of the tile within its level.
      uint64_t index = 0;

      /// @brief The width of the tile; 0 denotes a full tile (TILE_WIDTH).
      uint16_t width = 0;

      /// @brief The number of hashes the tile holds.
      [[nodiscard]] uint16_t num_hashes() const
      {
        return width == 0 ? TILE_WIDTH : width;
      }

      /// @brief Whether the tile is partial.
      [[nodiscard]] bool is_partial() const
      {
        return width != 0;
      }
    };

    /// @brief Reads and writes tlog-tiles tile and checkpoint files on a local
    /// filesystem.
    /// @tparam HASH_SIZE Size of each hash in bytes
    /// @tparam HASH_FUNCTION The tree's node hash function (carried for use by
    /// later components; tile I/O itself does not hash).
    template <
      size_t HASH_SIZE,
      void HASH_FUNCTION(
        const HashT<HASH_SIZE>&, const HashT<HASH_SIZE>&, HashT<HASH_SIZE>&)>
    class TileStoreT
    {
    public:
      /// @brief The type of hashes stored in tiles.
      using Hash = HashT<HASH_SIZE>;

      /// @brief Constructs a tile store rooted at @p prefix.
      /// @param prefix The directory under which tiles and the checkpoint live.
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
        std::string rel = "tile/" + std::to_string((unsigned)ref.level) + "/" +
          encode_tile_index(ref.index);
        if (ref.is_partial())
        {
          rel += ".p/" + std::to_string((unsigned)ref.width);
        }
        return prefix / rel;
      }

      /// @brief The filesystem path of an entry bundle.
      [[nodiscard]] std::filesystem::path entries_path(
        uint64_t index, uint16_t width = 0) const
      {
        std::string rel = "tile/entries/" + encode_tile_index(index);
        if (width != 0)
        {
          rel += ".p/" + std::to_string((unsigned)width);
        }
        return prefix / rel;
      }

      /// @brief The filesystem path of the checkpoint.
      [[nodiscard]] std::filesystem::path checkpoint_path() const
      {
        return prefix / "checkpoint";
      }

      /// @brief Whether a full tile exists on disk.
      [[nodiscard]] bool has_full_tile(uint8_t level, uint64_t index) const
      {
        return std::filesystem::exists(tile_path(TileRef{level, index, 0}));
      }

      /// @brief Whether a specific tile exists on disk.
      [[nodiscard]] bool has_tile(const TileRef& ref) const
      {
        return std::filesystem::exists(tile_path(ref));
      }

      /// @brief Writes a tile to disk atomically.
      /// @param ref The tile to write
      /// @param hashes The tile's hashes (exactly ref.num_hashes() of them)
      void write_tile(const TileRef& ref, const std::vector<Hash>& hashes)
      {
        if (hashes.size() != ref.num_hashes())
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
      /// @return The tile's hashes (ref.num_hashes() of them)
      [[nodiscard]] std::vector<Hash> read_tile(const TileRef& ref) const
      {
        std::vector<uint8_t> bytes = read_file(tile_path(ref));
        const size_t expected = (size_t)ref.num_hashes() * HASH_SIZE;
        if (bytes.size() != expected)
        {
          throw std::runtime_error("unexpected tile size");
        }

        std::vector<Hash> hashes;
        hashes.reserve(ref.num_hashes());
        size_t position = 0;
        for (uint16_t i = 0; i < ref.num_hashes(); i++)
        {
          hashes.emplace_back(bytes, position);
        }
        return hashes;
      }

      /// @brief Writes the (unsigned) checkpoint: tree size and root hash.
      void write_checkpoint(uint64_t size, const Hash& root_hash)
      {
        std::string text =
          std::to_string(size) + "\n" + root_hash.to_string() + "\n";
        std::vector<uint8_t> bytes(text.begin(), text.end());
        write_file_atomically(checkpoint_path(), bytes);
      }

      /// @brief Reads the checkpoint, if present.
      /// @param size Set to the checkpoint tree size on success
      /// @param root_hash Set to the checkpoint root hash on success
      /// @return Whether a checkpoint was read
      [[nodiscard]] bool read_checkpoint(uint64_t& size, Hash& root_hash) const
      {
        std::ifstream f(checkpoint_path(), std::ios::binary);
        if (!f.good())
        {
          return false;
        }
        std::string size_line;
        std::string root_line;
        if (!std::getline(f, size_line) || !std::getline(f, root_line))
        {
          throw std::runtime_error("malformed checkpoint");
        }
        trim(size_line);
        trim(root_line);
        size = std::stoull(size_line);
        root_hash = Hash(root_line);
        return true;
      }

    protected:
      /// @brief The root directory of the store.
      std::filesystem::path prefix;

      /// @brief Removes trailing carriage-return/whitespace from a line.
      static void trim(std::string& s)
      {
        while (!s.empty() &&
               (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' ||
                s.back() == '\t'))
        {
          s.pop_back();
        }
      }

      /// @brief Reads an entire file into a byte vector.
      static std::vector<uint8_t> read_file(const std::filesystem::path& path)
      {
        std::ifstream f(path, std::ios::binary);
        if (!f.good())
        {
          throw std::runtime_error("cannot open file: " + path.string());
        }
        return std::vector<uint8_t>(
          (std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>());
      }

      /// @brief Writes a file atomically via a temporary file and rename.
      /// @note Immutable tiles are never left half-written after a crash.
      static void write_file_atomically(
        const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
      {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
          throw std::runtime_error(
            "cannot create directory " + path.parent_path().string() + ": " +
            ec.message());
        }

        std::filesystem::path tmp = path;
        tmp += ".tmp";
        {
          std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
          if (!f.good())
          {
            throw std::runtime_error("cannot open file: " + tmp.string());
          }
          if (!bytes.empty())
          {
            f.write(
              reinterpret_cast<const char*>(bytes.data()),
              (std::streamsize)bytes.size());
          }
          f.flush();
          if (!f.good())
          {
            throw std::runtime_error("error writing file: " + tmp.string());
          }
        }
        std::filesystem::rename(tmp, path);
      }
    };

    /// @brief Default tile store (SHA256, default hash function).
    using TileStore = TileStoreT<32, sha256_compress>;

#ifdef HAVE_OPENSSL
    /// @brief SHA384 tile store.
    using TileStore384 = TileStoreT<48, sha384_openssl>;

    /// @brief SHA512 tile store.
    using TileStore512 = TileStoreT<64, sha512_openssl>;
#endif
  }
}
