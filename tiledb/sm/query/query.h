/**
 * @file   query.h
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2017-2018 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * This file defines class Query.
 */

#ifndef TILEDB_QUERY_H
#define TILEDB_QUERY_H

#include "tiledb/sm/enums/query_status.h"
#include "tiledb/sm/enums/query_type.h"
#include "tiledb/sm/fragment/fragment.h"
#include "tiledb/sm/misc/status.h"
#include "tiledb/sm/query/array_ordered_write_state.h"
#include "tiledb/sm/query/dense_cell_range_iter.h"
#include "tiledb/sm/storage_manager/storage_manager.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <tbb/task_scheduler_init.h>

namespace tiledb {
namespace sm {

class ArrayOrderedWriteState;
class Fragment;
class StorageManager;

/** Processes a (read/write) query. */
class Query {
 public:
  /* ********************************* */
  /*          TYPE DEFINITIONS         */
  /* ********************************* */

  /**
   * For each fixed-sized attributes, the second tile in the pair is
   * ignored. For var-sized attributes, the first is a pointer to the
   * offsets tile and the second is a pointer to the var-sized values tile.
   */
  typedef std::pair<std::shared_ptr<Tile>, std::shared_ptr<Tile>> TilePair;

  /** Information about a tile (across multiple attributes). */
  struct OverlappingTile {
    /** A fragment index. */
    unsigned fragment_idx_;
    /** The tile index in the fragment. */
    uint64_t tile_idx_;
    /** `true` if the overlap is full, and `false` if it is partial. */
    bool full_overlap_;
    /**
     * Maps attribute names to attribute tiles. Note that the coordinates
     * are a special attribute as well.
     */
    std::unordered_map<std::string, TilePair> attr_tiles_;

    /** Constructor. */
    OverlappingTile(
        unsigned fragment_idx,
        uint64_t tile_idx,
        const std::vector<std::string>& attributes,
        bool full_overlap = false)
        : fragment_idx_(fragment_idx)
        , tile_idx_(tile_idx)
        , full_overlap_(full_overlap) {
      attr_tiles_[constants::coords] = std::make_pair(nullptr, nullptr);
      for (const auto& attr : attributes) {
        if (attr != constants::coords) {
          attr_tiles_[attr] = std::make_pair(nullptr, nullptr);
        }
      }
    }
  };

  /** A vector of overlapping tiles. */
  typedef std::vector<std::shared_ptr<OverlappingTile>> OverlappingTileVec;

  /** A cell range belonging to a particular overlapping tile. */
  struct OverlappingCellRange {
    /**
     * The tile the cell range belongs to. If `nullptr`, then this is
     * an "empty" cell range, to be filled with the default empty
     * values.
     */
    std::shared_ptr<OverlappingTile> tile_;
    /** The starting cell in the range. */
    uint64_t start_;
    /** The ending cell in the range. */
    uint64_t end_;

    /** Constructor. */
    OverlappingCellRange(
        std::shared_ptr<OverlappingTile> tile, uint64_t start, uint64_t end)
        : tile_(std::move(tile))
        , start_(start)
        , end_(end) {
    }
  };

  /** A list of cell ranges. */
  typedef std::list<std::shared_ptr<OverlappingCellRange>>
      OverlappingCellRangeList;

  /**
   * Records the overlapping tile and position of the coordinates
   * in that tile.
   *
   * @tparam T The coords type
   */
  template <class T>
  struct OverlappingCoords {
    /** The overlapping tile the coords belong to. */
    std::shared_ptr<OverlappingTile> tile_;
    /** The coordinates. */
    const T* coords_;
    /** The position of the coordinates in the tile. */
    uint64_t pos_;

    /** Constructor. */
    OverlappingCoords(
        std::shared_ptr<OverlappingTile> tile, const T* coords, uint64_t pos)
        : tile_(std::move(tile))
        , coords_(coords)
        , pos_(pos) {
    }
  };

  /**
   * Type alias for a vector of OverlappingCoords.
   *
   * @tparam T The coords type
   */
  template <typename T>
  using OverlappingCoordsVec =
      std::vector<std::shared_ptr<OverlappingCoords<T>>>;

  /** A cell range produced by the dense read algorithm. */
  template <class T>
  struct DenseCellRange {
    /**
     * The fragment index. `-1` stands for no fragment, which means
     * that the cell range must be filled with the fill value.
     */
    int fragment_idx_;
    /** The tile coordinates of the range. */
    const T* tile_coords_;
    /** The starting cell in the range. */
    uint64_t start_;
    /** The ending cell in the range. */
    uint64_t end_;

    /** Constructor. */
    DenseCellRange(
        int fragment_idx, const T* tile_coords, uint64_t start, uint64_t end)
        : fragment_idx_(fragment_idx)
        , tile_coords_(tile_coords)
        , start_(start)
        , end_(end) {
    }
  };

  /* ********************************* */
  /*     CONSTRUCTORS & DESTRUCTORS    */
  /* ********************************* */

  /** Constructor. */
  Query();

  /**
   * Constructor called when the query to be created continues to write/append
   * to the fragment that was created by the *common_query*.
   *
   * @param common_query The query into whose fragment to append.
   */
  Query(Query* common_query);

  /** Destructor. */
  ~Query();

  /* ********************************* */
  /*                 API               */
  /* ********************************* */

  /**
   * Computes info about the overlapping tiles, such as which fragment they
   * belong to, the tile index and the type of overlap.
   *
   * @tparam T The coords type.
   * @param tiles The tiles to be computed.
   * @return Status
   */
  template <class T>
  Status compute_overlapping_tiles(OverlappingTileVec* tiles) const;

  /**
   * Retrieves the tiles on a particular attribute from all input fragments
   * based on the tile info in `tiles`.
   *
   * @param attr_name The attribute name.
   * @param tiles The retrieved tiles will be stored in `tiles`.
   * @return Status
   */
  Status read_tiles(
      const std::string& attr_name, OverlappingTileVec* tiles) const;

  /**
   * Computes the overlapping coordinates for a given subarray.
   *
   * @tparam T The coords type.
   * @param tiles The tiles to get the overlapping coordinates from.
   * @param coords The coordinates to be retrieved.
   * @return Status
   */
  template <class T>
  Status compute_overlapping_coords(
      const OverlappingTileVec& tiles, OverlappingCoordsVec<T>* coords) const;

  /**
   * Retrieves the coordinates that overlap the subarray from the input
   * overlapping tile.
   *
   * @tparam T The coords type.
   * @param The overlapping tile.
   * @param coords The overlapping coordinates to retrieve.
   * @return Status
   */
  template <class T>
  Status compute_overlapping_coords(
      const std::shared_ptr<OverlappingTile>& tile,
      OverlappingCoordsVec<T>* coords) const;

  /**
   * Gets all the coordinates of the input tile into `coords`.
   *
   * @tparam T The coords type.
   * @param tile The overlapping tile to read the coordinates from.
   * @param coords The overlapping coordinates to copy into.
   * @return Status
   */
  template <class T>
  Status get_all_coords(
      const std::shared_ptr<OverlappingTile>& tile,
      OverlappingCoordsVec<T>* coords) const;

  /**
   * Sorts the input coordinates according to the input layout.
   *
   * @tparam T The coords type.
   * @param coords The coordinates to sort.
   * @return Status
   */
  template <class T>
  Status sort_coords(OverlappingCoordsVec<T>* coords) const;

  /**
   * Deduplicates the input coordinates, breaking ties giving preference
   * to the largest fragment index (i.e., it prefers more recent fragments).
   * Duplicate elements are set to `nullptr` (not removed from the vector).
   *
   * @tparam T The coords type.
   * @param coords The coordinates to dedup.
   * @return Status
   */
  template <class T>
  Status dedup_coords(OverlappingCoordsVec<T>* coords) const;

  /**
   * Compute the maximal cell ranges of contiguous cell positions.
   *
   * @tparam T The coords type.
   * @param coords The coordinates to compute the ranges from.
   * @param cell_ranges The cell ranges to compute.
   * @return Status
   */
  template <class T>
  Status compute_cell_ranges(
      const OverlappingCoordsVec<T>& coords,
      OverlappingCellRangeList* cell_ranges) const;

  /**
   * Copies the cells for the input attribute and cell ranges, into
   * the corresponding result buffers.
   *
   * @param attribute The targeted attribute.
   * @param cell_ranges The cell ranges to copy cells for.
   * @return Status
   */
  Status copy_cells(
      const std::string& attribute,
      const OverlappingCellRangeList& cell_ranges) const;

  /**
   * Copies the cells for the input **fixed-sized** attribute and cell
   * ranges, into the corresponding result buffers.
   *
   * @param attribute The targeted attribute.
   * @param cell_ranges The cell ranges to copy cells for.
   * @return Status
   */
  Status copy_fixed_cells(
      const std::string& attribute,
      const OverlappingCellRangeList& cell_ranges) const;

  /**
   * Copies the cells for the input **var-sized** attribute and cell
   * ranges, into the corresponding result buffers.
   *
   * @param attribute The targeted attribute.
   * @param cell_ranges The cell ranges to copy cells for.
   * @return Status
   */
  Status copy_var_cells(
      const std::string& attribute,
      const OverlappingCellRangeList& cell_ranges) const;

  /**
   * Checks whether two hyper-rectangles overlap, and determines whether
   * the first rectangle contains the second.
   *
   * @tparam T The domain type.
   * @param a The first rectangle.
   * @param b The second rectangle.
   * @param dim_num The number of dimensions.
   * @param a_contains_b Determines whether the first rectangle contains the
   *     second.
   * @return `True` if the rectangles overlap, and `false` otherwise.
   */
  template <class T>
  bool overlap(
      const T* a, const T* b, unsigned dim_num, bool* a_contains_b) const;

  /** Returns the array schema.*/
  const ArraySchema* array_schema() const;

  /** Processes asynchronously the query. */
  Status async_process();

  /** Returns the list of ids of attributes involved in the query. */
  const std::vector<unsigned int>& attribute_ids() const;

  /** Retrieves the index of the buffer corresponding to the input attribute. */
  Status buffer_idx(const std::string& attribute, unsigned* bid) const;

  /** Finalizes and deletes the created fragments. */
  Status clear_fragments();

  /**
   * Retrieves the index of the coordinates buffer in the specified query
   * buffers.
   *
   * @param coords_buffer_i The index of the coordinates buffer to be retrieved.
   * @return Status
   */
  Status coords_buffer_i(int* coords_buffer_i) const;

  /**
   * Computes a vector of `subarrays` into which `subarray` must be partitioned,
   * such that each subarray in `subarrays` can be saferly answered by the
   * query without a memory overflow.
   *
   * @param subarray The input subarray.
   * @param subarrays The vector of subarray partitions to be retrieved.
   * @return Status
   */
  Status compute_subarrays(void* subarray, std::vector<void*>* subarrays) const;

  /**
   * Finalizes the query, properly finalizing and deleting the involved
   * fragments.
   */
  Status finalize();

  /** Returns the fragments involved in the query. */
  const std::vector<Fragment*>& fragments() const;

  /** Returns the metadata of the fragments involved in the query. */
  const std::vector<FragmentMetadata*>& fragment_metadata() const;

  /** Returns a vector with the fragment URIs. */
  std::vector<URI> fragment_uris() const;

  /** Returns the number of fragments involved in the query. */
  unsigned int fragment_num() const;

  /**
   * Initializes the query states. This must be called before the query is
   * submitted.
   */
  Status init();

  /**
   * Initializes the query.
   *
   * @param storage_manager The storage manager.
   * @param array_schema The array schema.
   * @param fragment_metadata The metadata of the involved fragments.
   * @param type The query type.
   * @param layout The cell layout.
   * @param subarray The subarray the query is constrained on. A nuullptr
   *     indicates the full domain.
   * @param attributes The names of the attributes involved in the query.
   * @param attribute_num The number of attributes.
   * @param buffers The query buffers with a one-to-one correspondences with
   *     the specified attributes. In a read query, the buffers will be
   *     populated with the query results. In a write query, the buffer
   *     contents will be appropriately written in a new fragment.
   * @param buffer_sizes The corresponding buffer sizes.
   * @param consolidation_fragment_uri This is used only in write queries.
   *     If it is different than empty, then it indicates that the query will
   *     be writing into a consolidation fragment with the input name.
   * @return Status
   */
  Status init(
      StorageManager* storage_manager,
      const ArraySchema* array_schema,
      const std::vector<FragmentMetadata*>& fragment_metadata,
      QueryType type,
      Layout layout,
      const void* subarray,
      const char** attributes,
      unsigned int attribute_num,
      void** buffers,
      uint64_t* buffer_sizes,
      const URI& consolidation_fragment_uri = URI(""));

  /**
   * Initializes the query. This is invoked for an internal async query.
   * The fragments and states are not immediately intialized. They
   * are instead initialized when the query is processed. This is
   * because the thread that initializes is different from that that
   * processes the query. The thread that processes the query must
   * initialize the fragments in the case of write queries, so that
   * the new fragment is named using the appropriate thread id.
   *
   * @param storage_manager The storage manager.
   * @param array_schema The array schema.
   * @param fragment_metadata The metadata of the involved fragments.
   * @param type The query type.
   * @param layout The cell layout.
   * @param subarray The subarray the query is constrained on. A nuullptr
   *     indicates the full domain.
   * @param attributes_ids The ids of the attributes involved in the query.
   * @param buffers The query buffers with a one-to-one correspondences with
   *     the specified attributes. In a read query, the buffers will be
   *     populated with the query results. In a write query, the buffer
   *     contents will be appropriately written in a new fragment.
   * @param buffer_sizes The corresponding buffer sizes.
   * @param add_coords If *true*, the coordinates attribute will be added
   *     to the provided *attribute_ids*. This is important for internal async
   *     read queries on sparse arrays, where the user had not specified
   *     the retrieval of the coordinates.
   * @return Status
   */
  Status init(
      StorageManager* storage_manager,
      const ArraySchema* array_schema,
      const std::vector<FragmentMetadata*>& fragment_metadata,
      QueryType type,
      Layout layoyt,
      const void* subarray,
      const std::vector<unsigned int>& attribute_ids,
      void** buffers,
      uint64_t* buffer_sizes,
      bool add_coords = false);

  /** Returns the lastly created fragment uri. */
  URI last_fragment_uri() const;

  /** Returns the cell layout. */
  Layout layout() const;

  /**
   * Returns true if the query cannot write to some buffer due to
   * an overflow.
   */
  bool overflow() const;

  /**
   * Checks if a particular query buffer (corresponding to some attribute)
   * led to an overflow based on an attribute id.
   */
  bool overflow(unsigned int attribute_id) const;

  /**
   * Checks if a particular query buffer (corresponding to some attribute)
   * led to an overflow based on an attribute name.
   *
   * @param attribute_name The attribute whose overflow to retrieve.
   * @param overflow The overflow status to be retieved.
   * @return Status (error is attribute is not involved in the query).
   */
  Status overflow(const char* attribute_name, unsigned int* overflow) const;

  /** Perform a dense read */
  Status dense_read();

  /**
   * Perform a dense read.
   *
   * @tparam The domain type.
   * @return Status
   */
  template <class T>
  Status dense_read();

  /** Perform a sparse read */
  Status sparse_read();

  /**
   * Perform a sparse read.
   *
   * @tparam The domain type.
   * @return Status
   */
  template <class T>
  Status sparse_read();

  /** Executes a read query. */
  Status read();

  /** Sets the array schema. */
  void set_array_schema(const ArraySchema* array_schema);

  /**
   * Sets the buffers to the query for a set of attributes.
   *
   * @param attributes The attributes the query will focus on.
   * @param attribute_num The number of attributes.
   * @param buffers The buffers that either have the input data to be written,
   *     or will hold the data to be read. Note that there is one buffer per
   *     fixed-sized attribute, and two buffers for each variable-sized
   *     attribute (the first holds the offsets, and the second the actual
   *     values).
   * @param buffer_sizes There must be an one-to-one correspondence with
   *     *buffers*. In the case of writes, they contain the sizes of *buffers*.
   *     In the case of reads, they initially contain the allocated sizes of
   *     *buffers*, but after the termination of the function they will contain
   *     the sizes of the useful (read) data in the buffers.
   * @return Status
   */
  Status set_buffers(
      const char** attributes,
      unsigned int attribute_num,
      void** buffers,
      uint64_t* buffer_sizes);

  /** Sets the query buffers. */
  void set_buffers(void** buffers, uint64_t* buffer_sizes);

  /**
   * Sets the callback function and its data input that will be called
   * upon the completion of an asynchronous query.
   */
  void set_callback(
      const std::function<void(void*)>& callback, void* callback_data);

  /** Sets and initializes the fragment metadata. */
  Status set_fragment_metadata(
      const std::vector<FragmentMetadata*>& fragment_metadata);

  /**
   * Sets the cell layout of the query. The function will return an error
   * if the queried array is a key-value store (because it has its default
   * layout.
   */
  Status set_layout(Layout layout);

  /** Sets the query status. */
  void set_status(QueryStatus status);

  /** Sets the storage manager. */
  void set_storage_manager(StorageManager* storage_manager);

  /**
   * Sets the query subarray. If it is null, then the subarray will be set to
   * the entire domain.
   *
   * @param subarray The subarray to be set.
   * @return Status
   */
  Status set_subarray(const void* subarray);

  /** Sets the query type. */
  void set_type(QueryType type);

  /** Returns the query status. */
  QueryStatus status() const;

  /** Returns the storage manager. */
  StorageManager* storage_manager() const;

  /** Returns the subarray in which the query is constrained. */
  const void* subarray() const;

  /** Returns the query type. */
  QueryType type() const;

  /** Executes a write query. */
  Status write();

  /**
   * Executes a write query, but the query writes the cells in the global
   * cell order, and also the cells are read from the input buffers,
   * not the internal buffers.
   */
  Status write(void** buffers, uint64_t* buffer_sizes);

 private:
  /* ********************************* */
  /*         PRIVATE ATTRIBUTES        */
  /* ********************************* */

  /** The names of the attributes involved in the query. */
  std::vector<std::string> attributes_;

  /** The array schema. */
  const ArraySchema* array_schema_;

  /**
   * The araay ordered write state. It handles write queries that
   * must write cells provided in a layout that is different
   * than the global cell order.
   */
  ArrayOrderedWriteState* array_ordered_write_state_;

  /** The ids of the attributes involved in the query. */
  std::vector<unsigned int> attribute_ids_;

  /**
   * The query buffers (one per involved attribute, two per variable-sized
   * attribute.
   */
  void** buffers_;

  /** The corresponding buffer sizes. */
  uint64_t* buffer_sizes_;

  /** Number of buffers. */
  unsigned buffer_num_;

  /** A function that will be called upon the completion of an async query. */
  std::function<void(void*)> callback_;

  /** The data input to the callback function. */
  void* callback_data_;

  /**
   * This is not *nullptr* in case of async write where the current query object
   * continues to write/append to the *common_query_*'s new fragment.
   */
  Query* common_query_;

  /**
   * If non-empty, then this holds the name of the consolidation fragment to be
   * created by this query. This also implies that the query type is WRITE.
   */
  URI consolidation_fragment_uri_;

  /** The query status. */
  QueryStatus status_;

  /** The TBB thread scheduler. */
  std::unique_ptr<tbb::task_scheduler_init> tbb_sched_;

  /** The fragments involved in the query. */
  std::vector<Fragment*> fragments_;

  /** Indicates whether the fragments have been initialized. */
  bool fragments_init_;

  /** Indicates if the stored fragments belong to the query object or not. */
  bool fragments_borrowed_;

  /** The metadata of the fragments involved in the query. */
  std::vector<FragmentMetadata*> fragment_metadata_;

  /** The cell layout. */
  Layout layout_;

  /** The storage manager. */
  StorageManager* storage_manager_;

  /**
   * The subarray the query is constrained on. A nullptr implies the
   * entire domain.
   */
  void* subarray_;

  /** The query type. */
  QueryType type_;

  /* ********************************* */
  /*           PRIVATE METHODS         */
  /* ********************************* */

  /** Adds the coordinates attribute if it does not exist. */
  void add_coords();

  /** Checks if attributes has been appropriately set for a query. */
  Status check_attributes();

  /**
   * Checks if the buffer sizes are correct in the case of writing
   * in a dense array in an ordered layout.
   *
   * @return Status
   */
  Status check_buffer_sizes_ordered() const;

  /** Checks if `subarray` falls inside the array domain. */
  Status check_subarray(const void* subarray) const;

  /** Checks if `subarray` falls inside the array domain. */
  template <class T>
  Status check_subarray(const T* subarray) const;

  /**
   * For the given cell range, it computes all the result dense cell ranges
   * across fragments, given precedence to more recent fragments.
   *
   * @tparam T The domain type.
   * @param tile_coords The tile coordinates in the array domain.
   * @param frag_its The fragment dence cell range iterators.
   * @param start The start position of the range this function focuses on.
   * @param end The end position of the range this function focuses on.
   * @param dense_cell_ranges The cell ranges where the results are appended to.
   * @return Status
   *
   * @note The input dense cell range iterators will be appropriately
   *     incremented.
   */
  template <class T>
  Status compute_dense_cell_ranges(
      const T* tile_coords,
      std::vector<DenseCellRangeIter<T>>& frag_its,
      uint64_t start,
      uint64_t end,
      std::list<DenseCellRange<T>>* dense_cell_ranges);

  /**
   * Computes the dense overlapping tiles and cell ranges based on the
   * input dense cell ranges. Note that the function also computes
   * the maximal ranges of contiguous cells for each fragment/tile pair.
   *
   * @tparam T The domain type.
   * @param dense_cell_ranges The dense cell ranges the overlapping tiles
   *     and cell ranges will be derived from.
   * @param tiles The overlapping tiles to be computed.
   * @param overlapping_cell_ranges The overlapping cell ranges to be
   *     computed.
   * @return Status
   */
  template <class T>
  Status compute_dense_overlapping_tiles_and_cell_ranges(
      const std::list<DenseCellRange<T>>& dense_cell_ranges,
      const OverlappingCoordsVec<T>& coords,
      OverlappingTileVec* tiles,
      OverlappingCellRangeList* overlapping_cell_ranges);

  /** Returns the empty fill value based on the input datatype. */
  const void* fill_value(Datatype type) const;

  /** Initializes the fragments (for a read query). */
  Status init_fragments(
      const std::vector<FragmentMetadata*>& fragment_metadata);

  /** Initializes the query states. */
  Status init_states();

  /**
   * Initializes the fragment dense cell range iterators. There is one vector
   * per tile overlapping with the query subarray, which stores one cell range
   * iterator per fragment.
   *
   * @tparam T The domain type.
   * @param iters The iterators to be initialized.
   * @param overlapping_tile_idx_coords A map from global tile index to a pair
   *     (overlapping tile index, overlapping tile coords).
   */
  template <class T>
  Status init_tile_fragment_dense_cell_range_iters(
      std::vector<std::vector<DenseCellRangeIter<T>>>* iters,
      std::unordered_map<uint64_t, std::pair<uint64_t, std::vector<T>>>*
          overlapping_tile_idx_coords);

  /** Creates a new fragment (for a write query). */
  Status new_fragment();

  /**
   * Returns a new fragment name, which is in the form: <br>
   * .__thread-id_timestamp. For instance,
   *  __6426153_1458759561320
   *
   * Note that this is a temporary name, initiated by a new write process.
   * After the new fragmemt is finalized, the array will change its name
   * by removing the leading '.' character.
   *
   * @return A new special fragment name on success, or "" (empty string) on
   *     error.
   */
  std::string new_fragment_name() const;

  /**
   * Opens the existing fragments.
   *
   * @param metadata The metadata of the array fragments.
   * @return Status
   */
  Status open_fragments(const std::vector<FragmentMetadata*>& metadata);

  /** Sets the query attributes. */
  Status set_attributes(const char** attributes, unsigned int attribute_num);

  /**
   * Sets the input buffer sizes to zero. The function assumes that the buffer
   * sizes correspond to the attribute buffers specified upon query creation.
   */
  void zero_out_buffer_sizes(uint64_t* buffer_sizes) const;

  /** Memsets all set buffers to zero. Used only in read queries. */
  void zero_out_buffers();
};

}  // namespace sm
}  // namespace tiledb

#endif  // TILEDB_QUERY_H
