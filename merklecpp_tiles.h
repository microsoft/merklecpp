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
    /// complete 2**(8L)-leaf subtree. Full tiles (256 such entries) are
    /// therefore immutable and written exactly once; the incomplete frontier is
    /// never tiled, and partial tiles (complete entries, partial count) are the
    /// only resources that may be re-written (grown) or removed once superseded
    /// by a full tile.
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

      /// @brief Write-path options.
      struct Options
      {
        /// @brief Write the partial (rightmost) tiles for the current size.
        bool write_partial_tiles = true;

        /// @brief Roll up and write tiles at levels >= 1.
        bool write_higher_levels = true;

        /// @brief Remove partial tiles once superseded by a full tile (or a
        /// wider partial at the same index).
        bool remove_superseded_partials = true;
      };

      /// @brief Counts of work performed by a write_up_to call.
      struct Stats
      {
        /// @brief Number of full tiles written.
        uint64_t full_written = 0;

        /// @brief Number of partial tiles written.
        uint64_t partial_written = 0;

        /// @brief Number of superseded partial tiles removed.
        uint64_t partial_removed = 0;
      };

      /// @brief Constructs a writer over @p store.
      explicit TileWriterT(Store& store, Options options = {}) :
        store(store), options(options)
      {}

      /// @brief Writes all newly-complete full tiles and the partial tiles for
      /// a tree of @p size leaves.
      /// @param size The current tree size
      /// @param leaf_at Returns the level-0 leaf hash for a leaf index in
      /// [0, size); only ever queried for leaves of complete subtrees.
      /// @return Counts of tiles written/removed
      /// @note Incremental: full tiles already on disk are immutable and are
      /// never rewritten.
      Stats write_up_to(uint64_t size, const LeafFn& leaf_at)
      {
        Stats stats;
        const uint8_t max_level = options.write_higher_levels ? 63 : 0;

        for (uint8_t level = 0; level <= max_level; level++)
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

          if (!cursor_inited[level])
          {
            next_full[level] = full_prefix_length(level);
            cursor_inited[level] = true;
          }

          for (uint64_t n = next_full[level]; n < full_tiles; n++)
          {
            if (store.has_full_tile(level, n))
            {
              continue; // immutable: never rewrite an existing full tile
            }
            store.write_tile(
              TileRef{level, n, 0},
              collect(level, n * TILE_WIDTH, TILE_WIDTH, leaf_at));
            stats.full_written++;
          }
          if (full_tiles > next_full[level])
          {
            next_full[level] = full_tiles;
          }

          if (options.write_partial_tiles)
          {
            write_partial(
              level,
              full_tiles,
              (uint16_t)(entries % TILE_WIDTH),
              leaf_at,
              stats);
          }
        }

        return stats;
      }

    protected:
      /// @brief The tile store written to.
      Store& store;

      /// @brief Write-path options.
      Options options;

      /// @brief Per-level index of the next full tile to write.
      std::vector<uint64_t> next_full;

      /// @brief Per-level flag indicating next_full has been initialised.
      std::vector<bool> cursor_inited;

      /// @brief Per-level last partial tile written by this writer.
      std::vector<TileRef> last_partial;

      /// @brief Per-level flag indicating last_partial is valid.
      std::vector<bool> has_last_partial;

      /// @brief Number of complete level-@p level entries for a tree of @p
      /// size.
      static uint64_t entries_at_level(uint64_t size, uint8_t level)
      {
        const unsigned shift = 8u * (unsigned)level;
        return shift >= 64 ? 0 : (size >> shift);
      }

      /// @brief Ensures per-level bookkeeping vectors cover @p level.
      void ensure_level(uint8_t level)
      {
        const size_t needed = (size_t)level + 1;
        if (next_full.size() < needed)
        {
          next_full.resize(needed, 0);
          cursor_inited.resize(needed, false);
          last_partial.resize(needed, TileRef{});
          has_last_partial.resize(needed, false);
        }
      }

      /// @brief Length of the contiguous prefix of full tiles already on disk.
      [[nodiscard]] uint64_t full_prefix_length(uint8_t level) const
      {
        if (!store.has_full_tile(level, 0))
        {
          return 0;
        }
        uint64_t lo = 0; // present
        uint64_t hi = 1;
        while (store.has_full_tile(level, hi))
        {
          lo = hi;
          hi <<= 1;
        }
        while (hi - lo > 1)
        {
          const uint64_t mid = lo + (hi - lo) / 2;
          if (store.has_full_tile(level, mid))
          {
            lo = mid;
          }
          else
          {
            hi = mid;
          }
        }
        return lo + 1;
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
            out.push_back(perfect_root<HASH_SIZE, HASH_FUNCTION>(
              store.read_tile(TileRef{(uint8_t)(level - 1), g, 0})));
          }
        }
        return out;
      }

      /// @brief Writes (or removes) the partial tile at the rightmost index.
      void write_partial(
        uint8_t level,
        uint64_t index,
        uint16_t width,
        const LeafFn& leaf_at,
        Stats& stats)
      {
        if (width == 0)
        {
          // No partial at this size; a previous partial (if any) is now covered
          // by a full tile and may be removed.
          if (options.remove_superseded_partials && has_last_partial[level])
          {
            remove_partial_dir(level, last_partial[level].index);
            has_last_partial[level] = false;
            stats.partial_removed++;
          }
          return;
        }

        const TileRef ref{level, index, width};

        if (
          has_last_partial[level] && last_partial[level].index == index &&
          last_partial[level].width == width)
        {
          return; // identical partial already written by this writer
        }

        if (options.remove_superseded_partials && has_last_partial[level])
        {
          if (last_partial[level].index != index)
          {
            // Previous partial's index is now covered by a full tile.
            remove_partial_dir(level, last_partial[level].index);
            stats.partial_removed++;
          }
          else
          {
            // Same index, narrower width: drop the stale width file(s).
            remove_partial_dir(level, index);
          }
        }

        store.write_tile(
          ref, collect(level, index * TILE_WIDTH, width, leaf_at));
        stats.partial_written++;
        last_partial[level] = ref;
        has_last_partial[level] = true;
      }

      /// @brief Removes the partial-tile directory tile/<level>/<index>.p.
      void remove_partial_dir(uint8_t level, uint64_t index)
      {
        const std::filesystem::path dir =
          store.tile_path(TileRef{level, index, 1}).parent_path();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
      }
    };

    /// @brief Default tile store (SHA256, default hash function).
    using TileStore = TileStoreT<32, sha256_compress>;

    /// @brief Default tile writer (SHA256, default hash function).
    using TileWriter = TileWriterT<32, sha256_compress>;

#ifdef HAVE_OPENSSL
    /// @brief SHA384 tile store.
    using TileStore384 = TileStoreT<48, sha384_openssl>;

    /// @brief SHA512 tile store.
    using TileStore512 = TileStoreT<64, sha512_openssl>;

    /// @brief SHA384 tile writer.
    using TileWriter384 = TileWriterT<48, sha384_openssl>;

    /// @brief SHA512 tile writer.
    using TileWriter512 = TileWriterT<64, sha512_openssl>;
#endif
  }
}
