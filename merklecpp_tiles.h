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

namespace merkle // NOLINT(modernize-concat-nested-namespaces)
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
        const std::vector<uint8_t> bytes(text.begin(), text.end());
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

      /// @brief Whether a full entry bundle exists on disk.
      [[nodiscard]] bool has_entry_bundle(uint64_t index) const
      {
        return std::filesystem::exists(entries_path(index, 0));
      }

      /// @brief Writes an entry bundle to disk atomically.
      /// @param index The bundle index
      /// @param width The bundle width; 0 denotes a full bundle (TILE_WIDTH)
      /// @param entries The raw log entries (exactly width, or TILE_WIDTH)
      /// @note Entries are stored in the tlog-tiles entry-bundle format: a
      /// sequence of big-endian uint16 length-prefixed byte strings.
      void write_entry_bundle(
        uint64_t index,
        uint16_t width,
        const std::vector<std::vector<uint8_t>>& entries)
      {
        const uint16_t expected = width == 0 ? TILE_WIDTH : width;
        if (entries.size() != expected)
        {
          throw std::runtime_error("entry bundle width mismatch");
        }
        write_file_atomically(
          entries_path(index, width), encode_entries(entries));
      }

      /// @brief Reads an entry bundle from disk.
      /// @param index The bundle index
      /// @param width The bundle width; 0 denotes a full bundle (TILE_WIDTH)
      /// @return The raw log entries
      [[nodiscard]] std::vector<std::vector<uint8_t>> read_entry_bundle(
        uint64_t index, uint16_t width = 0) const
      {
        const uint16_t count = width == 0 ? TILE_WIDTH : width;
        return decode_entries(read_file(entries_path(index, width)), count);
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
          if (pos + 2 > bytes.size())
          {
            throw std::runtime_error("truncated entry bundle");
          }
          const auto len =
            (uint16_t)(((uint16_t)bytes[pos] << 8) | bytes[pos + 1]);
          pos += 2;
          if (pos + len > bytes.size())
          {
            throw std::runtime_error("truncated entry bundle");
          }
          out.emplace_back(
            bytes.begin() + static_cast<std::ptrdiff_t>(pos),
            bytes.begin() + static_cast<std::ptrdiff_t>(pos + len));
          pos += len;
        }
        return out;
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
        return {
          std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
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
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
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
    /// @note @p available_size is the largest tree size whose tiles are durably
    /// written; any complete subtree within it is resolvable. A missing tile
    /// (e.g. a frontier partial that was not written) yields false so that a
    /// proof builder can fall back to another source.
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
      /// @p available_size leaves.
      TileHashSourceT(const Store& store, uint64_t available_size) :
        store(store), available_size(available_size)
      {}

      bool subtree_root(uint8_t level, uint64_t index, Hash& out) const override
      {
        const uint8_t L = level / TILE_HEIGHT;
        const uint8_t r = level % TILE_HEIGHT;
        const uint64_t span = (uint64_t)1 << r; // entries spanned in level L
        const uint64_t first = index << r; // first level-L entry index

        const uint64_t entries = entries_at_level(available_size, L);
        if (first + span > entries)
        {
          return false; // extends into the incomplete frontier
        }

        const uint64_t n = first / TILE_WIDTH;
        const uint64_t off = first % TILE_WIDTH;

        // Choose the full or current partial tile holding these entries.
        TileRef ref{L, n, 0};
        if ((n + 1) * TILE_WIDTH > entries)
        {
          ref.width = (uint16_t)(entries - n * TILE_WIDTH);
        }
        if (!store.has_tile(ref))
        {
          return false;
        }

        const std::vector<Hash> tile = store.read_tile(ref);
        if (span == 1)
        {
          out = tile.at(off);
          return true;
        }
        const std::vector<Hash> sub(
          tile.begin() + off, tile.begin() + off + span);
        out = perfect_root<HASH_SIZE, HASH_FUNCTION>(sub);
        return true;
      }

    protected:
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      const Store& store;
      uint64_t available_size;

      static uint64_t entries_at_level(uint64_t size, uint8_t level)
      {
        const unsigned shift = 8U * (unsigned)level;
        return shift >= 64 ? 0 : (size >> shift);
      }
    };

    /// @brief Builds and verifies inclusion and consistency proofs.
    /// @note Proofs are assembled from a HashSource using the tree's
    /// HASH_FUNCTION, so an inclusion proof is byte-identical to the one
    /// produced by merkle::TreeT::path()/past_path() and verifies with
    /// PathT::verify().
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
        while ((k << 1) < n)
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
    /// @note Appends grow an in-memory tree; checkpoint() durably writes the
    /// tiles. Compaction (dropping from memory the leaves already covered by a
    /// durably-written full tile) is optional: enable it per checkpoint with
    /// Config::compact_on_checkpoint, or call compact() explicitly. Proofs are
    /// served from the combination of the resident tree and the tiles, so they
    /// remain available for compacted (dropped) leaves.
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
        /// @brief Root directory for tiles and the checkpoint.
        std::filesystem::path prefix;

        /// @brief Number of most-recent leaves to keep resident when
        /// compacting (i.e. never dropped from memory).
        uint64_t retention_margin = 0;

        /// @brief If set, checkpoint() compacts after writing tiles, dropping
        /// from memory the leaves already covered by a full tile. Off by
        /// default: tiles are written but the tree keeps every leaf resident.
        bool compact_on_checkpoint = false;

        /// @brief Tile writer options.
        typename Writer::Options writer = {};
      };

      explicit TiledTreeT(Config config) :
        config(std::move(config)),
        store(this->config.prefix),
        writer(store, this->config.writer)
      {}

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

      /// @brief The tree size of the last checkpoint (the durable tile size).
      [[nodiscard]] uint64_t checkpoint_size() const
      {
        return tiles_size;
      }

      /// @brief Access to the underlying tree.
      Tree& tree_ref()
      {
        return tree;
      }

      /// @brief Access to the underlying tile store.
      Store& store_ref()
      {
        return store;
      }

      /// @brief Writes tiles and a checkpoint; compacts only if
      /// Config::compact_on_checkpoint is set.
      /// @return Counts of the tiles written/removed by this checkpoint
      Stats checkpoint()
      {
        Stats stats;
        const uint64_t n = tree.num_leaves();
        if (n == 0)
        {
          return stats;
        }

        stats = writer.write_up_to(n, [this](uint64_t i) -> const Hash& {
          return tree.leaf((size_t)i);
        });
        store.write_checkpoint(n, tree.root());
        tiles_size = n;

        if (config.compact_on_checkpoint)
        {
          compact();
        }
        return stats;
      }

      /// @brief Drops from the in-memory tree every leaf already covered by a
      /// durably-written full tile, keeping retention_margin recent leaves.
      /// @return The new minimum (smallest still-resident) leaf index
      /// @note Only leaves whose full tile already exists are dropped, so
      /// inclusion and consistency proofs remain available (served from the
      /// tiles). Has no effect until tiling has produced full tiles. The flush
      /// target is a multiple of TILE_WIDTH, so the coverage invariant
      /// (min_index <= durable full-tile coverage) always holds.
      uint64_t compact()
      {
        const uint64_t covered = (tiles_size / TILE_WIDTH) * TILE_WIDTH;
        uint64_t target = covered > config.retention_margin ?
          covered - config.retention_margin :
          0;
        target = (target / TILE_WIDTH) * TILE_WIDTH;
        if (target > tree.min_index())
        {
          tree.flush_to((size_t)target);
        }
        return tree.min_index();
      }

      /// @brief Rolls the tree back so that @p index becomes the last leaf,
      /// removing all leaves after it (same semantics as TreeT::retract_to).
      /// @note Tiles are immutable, so entries already committed to tiles
      /// cannot be rolled back: this throws if the resulting size would be
      /// smaller than checkpoint_size(). Only un-checkpointed (not-yet-tiled)
      /// entries may be rolled back. (Retracting the underlying tree directly
      /// via tree_ref() bypasses this guard and can leave stale tiles -- do not
      /// do that.)
      void retract_to(size_t index)
      {
        if ((uint64_t)index + 1 < tiles_size)
        {
          throw std::runtime_error(
            "TiledTree::retract_to: cannot roll back entries already committed "
            "to immutable tiles (resulting size < checkpoint size)");
        }
        tree.retract_to(index);
      }

      /// @brief Inclusion proof for @p index in a tree of @p proof_size leaves.
      /// @note Served from tiles (flushed past) combined with the resident tree
      /// (recent frontier); @p proof_size may exceed checkpoint_size().
      std::shared_ptr<Path> inclusion_proof(uint64_t index, uint64_t proof_size)
      {
        MemoryHashSourceT<HASH_SIZE, HASH_FUNCTION> mem(tree);
        TileHashSourceT<HASH_SIZE, HASH_FUNCTION> tile_src(store, tiles_size);
        CombinedHashSourceT<HASH_SIZE, HASH_FUNCTION> combined(mem, tile_src);
        ProofEngineT<HASH_SIZE, HASH_FUNCTION> engine(combined);
        return engine.inclusion_proof(index, proof_size);
      }

      /// @brief Consistency proof between tree sizes @p m and @p n.
      std::vector<Hash> consistency_proof(uint64_t m, uint64_t n)
      {
        MemoryHashSourceT<HASH_SIZE, HASH_FUNCTION> mem(tree);
        TileHashSourceT<HASH_SIZE, HASH_FUNCTION> tile_src(store, tiles_size);
        CombinedHashSourceT<HASH_SIZE, HASH_FUNCTION> combined(mem, tile_src);
        ProofEngineT<HASH_SIZE, HASH_FUNCTION> engine(combined);
        return engine.consistency_proof(m, n);
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
    };

    /// @brief Writes tlog-tiles entry bundles (raw log entries) for a growing
    /// log.
    /// @note Entry bundles are level-0 only and application-owned: merklecpp
    /// stores leaf hashes, while the raw entries (and the leaf-hash derivation
    /// linking each entry to its level-0 tile hash) are the application's
    /// responsibility. Full bundles are immutable and written once; only the
    /// rightmost partial bundle may grow or be removed once superseded.
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

      /// @brief Write-path options.
      struct Options
      {
        /// @brief Write the partial (rightmost) bundle for the current size.
        bool write_partial = true;

        /// @brief Remove a partial bundle once superseded by a full bundle (or
        /// a wider partial at the same index).
        bool remove_superseded_partials = true;
      };

      /// @brief Counts of work performed by a write_up_to call.
      struct Stats
      {
        uint64_t full_written = 0;
        uint64_t partial_written = 0;
        uint64_t partial_removed = 0;
      };

      explicit EntryBundleWriterT(Store& store, Options options = {}) :
        store(store), options(options)
      {}

      /// @brief Writes all newly-complete full bundles and the partial bundle
      /// for a log of @p size entries.
      /// @param size The current number of entries
      /// @param entry_at Returns the raw bytes of the entry at an index in
      /// [0, size)
      /// @return Counts of bundles written/removed
      Stats write_up_to(uint64_t size, const EntryFn& entry_at)
      {
        Stats stats;
        const uint64_t full = size / TILE_WIDTH;

        if (!cursor_inited)
        {
          next_full = full_prefix_length();
          cursor_inited = true;
        }

        for (uint64_t n = next_full; n < full; n++)
        {
          if (store.has_entry_bundle(n))
          {
            continue; // immutable: never rewrite an existing full bundle
          }
          store.write_entry_bundle(
            n, 0, collect(n * TILE_WIDTH, TILE_WIDTH, entry_at));
          stats.full_written++;
        }
        if (full > next_full)
        {
          next_full = full;
        }

        if (options.write_partial)
        {
          write_partial(full, (uint16_t)(size % TILE_WIDTH), entry_at, stats);
        }
        return stats;
      }

    protected:
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
      Store& store;
      Options options;
      uint64_t next_full = 0;
      bool cursor_inited = false;
      uint64_t last_partial_index = 0;
      uint16_t last_partial_width = 0;
      bool has_last_partial = false;

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

      [[nodiscard]] uint64_t full_prefix_length() const
      {
        if (!store.has_entry_bundle(0))
        {
          return 0;
        }
        uint64_t lo = 0;
        uint64_t hi = 1;
        while (store.has_entry_bundle(hi))
        {
          lo = hi;
          hi <<= 1;
        }
        while (hi - lo > 1)
        {
          const uint64_t mid = lo + (hi - lo) / 2;
          if (store.has_entry_bundle(mid))
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

      void write_partial(
        uint64_t index, uint16_t width, const EntryFn& entry_at, Stats& stats)
      {
        if (width == 0)
        {
          if (options.remove_superseded_partials && has_last_partial)
          {
            remove_partial(last_partial_index);
            has_last_partial = false;
            stats.partial_removed++;
          }
          return;
        }

        if (
          has_last_partial && last_partial_index == index &&
          last_partial_width == width)
        {
          return;
        }

        if (options.remove_superseded_partials && has_last_partial)
        {
          if (last_partial_index != index)
          {
            remove_partial(last_partial_index);
            stats.partial_removed++;
          }
          else
          {
            remove_partial(index);
          }
        }

        store.write_entry_bundle(
          index, width, collect(index * TILE_WIDTH, width, entry_at));
        stats.partial_written++;
        last_partial_index = index;
        last_partial_width = width;
        has_last_partial = true;
      }

      void remove_partial(uint64_t index)
      {
        const std::filesystem::path dir =
          store.entries_path(index, 1).parent_path();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
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
