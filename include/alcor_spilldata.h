#pragma once

#include <vector>
#include <iostream>
#include <cstdint>
#include <unordered_map>
#include <map>

#include "alcor_lightdata.h"
#include "TH1.h"
#include "TTree.h"
#include "triggers.h"

// ============================================================================
//  Data structures
// ============================================================================

/**
 * @brief Pairs a device ID with a bitmask (used for dead and participant masks).
 *
 * Compact, flat representation suitable for ROOT TTree serialisation, as ROOT
 * cannot natively branch a @c std::map.
 */
struct data_mask_struct
{
    uint8_t  device; ///< Device (ALCOR chip) identifier.
    uint32_t mask;   ///< Bitmask encoding channel states (e.g. alive/dead).
};

/**
 * @brief Plain-old-data aggregate that owns all spill information for one beam spill.
 *
 * A "spill" is one burst of beam delivered to the detector.  The struct holds
 * two parallel representations of the same data:
 *  - **Working maps** (`dead_mask`, `participants_mask`, `frame_and_lightdata`)
 *    used during online processing where random-access by key is needed.
 *  - **Flat vectors** (`*_list`, `frame_reference`, `lightdata_list_in_frame`)
 *    used for ROOT TTree I/O, which requires contiguous, pointer-stable storage.
 *
 * @note The raw pointer members (`*_ptr`) exist solely so that ROOT's
 *       @c TTree::SetBranchAddress can receive a stable address even when the
 *       vectors are reallocated.  They are always kept in sync with the
 *       corresponding vector via @c clear() and @c prepare_tree_fill().
 */
struct alcor_spilldata_struct
{
    // --- Special members ------------------------------------------------
    alcor_spilldata_struct() noexcept = default;

    alcor_spilldata_struct(alcor_spilldata_struct &&)            noexcept = default;
    alcor_spilldata_struct &operator=(alcor_spilldata_struct &&) noexcept = default;

    alcor_spilldata_struct(const alcor_spilldata_struct &)            = default;
    alcor_spilldata_struct &operator=(const alcor_spilldata_struct &) = default;

    // --- Working maps (random-access processing) ------------------------
    std::map<uint8_t,  uint32_t>              dead_mask;          ///< device → dead-channel bitmask.
    std::map<uint8_t,  uint32_t>              participants_mask;   ///< device → participating-channel bitmask.
    std::map<uint32_t, alcor_lightdata_struct> frame_and_lightdata; ///< frame_id → light-data payload.

    // --- Flat vectors (ROOT TTree serialisation) -------------------------
    std::vector<data_mask_struct>      dead_mask_list;            ///< Flat copy of @c dead_mask for TTree output.
    std::vector<data_mask_struct>      participants_mask_list;    ///< Flat copy of @c participants_mask for TTree output.
    std::vector<uint32_t>              frame_reference;           ///< Ordered list of frame IDs written to the TTree.
    std::vector<alcor_lightdata_struct> lightdata_list_in_frame;  ///< Light-data entries parallel to @c frame_reference.

    // --- Branch-address pointers (ROOT internal use) --------------------
    //  @warning These are intentionally NOT initialised here. Taking the address
    //           of a sibling member in a default member initialiser is UB, and
    //           after any copy/move the pointers would dangle into the source
    //           object. They are set correctly by clear() and prepare_tree_fill().
    std::vector<data_mask_struct>       *dead_mask_list_ptr;          ///< Stable pointer for ROOT branch address.
    std::vector<data_mask_struct>       *participants_mask_list_ptr;  ///< Stable pointer for ROOT branch address.
    std::vector<uint32_t>               *frame_reference_ptr;         ///< Stable pointer for ROOT branch address.
    std::vector<alcor_lightdata_struct> *lightdata_list_in_frame_ptr; ///< Stable pointer for ROOT branch address.

    /**
     * @brief Resets all members to an empty state and re-synchronises the
     *        branch-address pointers with their respective vectors.
     */
    void clear();
};

// ============================================================================
//  alcor_spilldata class
// ============================================================================

/**
 * @brief High-level accessor class for one beam-spill worth of ALCOR data.
 *
 * Wraps an @c alcor_spilldata_struct and adds:
 *  - Typed getters/setters (both by value and by reference).
 *  - Frame-level helpers (trigger queries, selective frame suppression).
 *  - ROOT TTree I/O (@c link_to_tree / @c write_to_tree / @c prepare_tree_fill).
 *
 * Inherits from @c alcor_lightdata to reuse common hit-decoding infrastructure.
 */
class alcor_spilldata : public alcor_lightdata
{
public:
    // -----------------------------------------------------------------------
    //  Constructors
    // -----------------------------------------------------------------------

    /// @brief Default constructor — initialises to an empty spill.
    alcor_spilldata() { spilldata.clear(); }

    /**
     * @brief Construct from an existing spill-data struct (copied).
     * @param v  Source struct; copied into internal storage.
     */
    explicit alcor_spilldata(const alcor_spilldata_struct &v) : spilldata(v) {}

    // -----------------------------------------------------------------------
    //  Getters — by value (deep copy)
    // -----------------------------------------------------------------------

    /// @brief Returns a full copy of the internal spill-data struct.
    alcor_spilldata_struct                        get_spilldata()            const { return spilldata; }

    /// @brief Returns a copy of the frame → light-data map.
    std::map<uint32_t, alcor_lightdata_struct>    get_frame()                const { return spilldata.frame_and_lightdata; }

    /// @brief Returns a copy of the device → participants-mask map.
    std::map<uint8_t, uint32_t>                   get_participants_mask()    const { return spilldata.participants_mask; }

    /// @brief Returns a copy of the device → dead-mask map.
    std::map<uint8_t, uint32_t>                   get_dead_mask()            const { return spilldata.dead_mask; }

    /// @brief Returns a copy of the flat light-data vector (TTree representation).
    std::vector<alcor_lightdata_struct>            get_frame_list()           const { return spilldata.lightdata_list_in_frame; }

    /// @brief Returns a copy of the flat frame-reference vector (TTree representation).
    std::vector<uint32_t>                         get_frame_reference_list() const { return spilldata.frame_reference; }

    // -----------------------------------------------------------------------
    //  Getters — by reference (zero-copy, caller must not outlive this object)
    // -----------------------------------------------------------------------

    /// @brief Returns a mutable reference to the internal spill-data struct.
    alcor_spilldata_struct                        &get_spilldata_link()            { return spilldata; }

    /// @brief Returns a mutable reference to the frame → light-data map.
    std::map<uint32_t, alcor_lightdata_struct>    &get_frame_link()                { return spilldata.frame_and_lightdata; }

    /// @brief Returns a mutable reference to the participants-mask map.
    std::map<uint8_t, uint32_t>                   &get_participants_mask_link()    { return spilldata.participants_mask; }

    /// @brief Returns a mutable reference to the dead-mask map.
    std::map<uint8_t, uint32_t>                   &get_dead_mask_link()            { return spilldata.dead_mask; }

    /// @brief Returns a mutable reference to the flat light-data vector.
    std::vector<alcor_lightdata_struct>            &get_frame_list_link()           { return spilldata.lightdata_list_in_frame; }

    /// @brief Returns a mutable reference to the flat frame-reference vector.
    std::vector<uint32_t>                         &get_frame_reference_list_link() { return spilldata.frame_reference; }

    // -----------------------------------------------------------------------
    //  Per-frame hit-vector getters
    // -----------------------------------------------------------------------

    /**
     * @brief Returns a mutable reference to the trigger-hit list for a given frame.
     * @param index_of_frame  Frame ID key in the frame map.
     */
    std::vector<trigger_event>        &get_frame_trigger_hits  (uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].trigger_hits;   }

    /**
     * @brief Returns a mutable reference to the timing-hit list for a given frame.
     * @param index_of_frame  Frame ID key in the frame map.
     */
    std::vector<alcor_finedata_struct> &get_frame_timing_hits  (uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].timing_hits;    }

    /**
     * @brief Returns a mutable reference to the tracking-hit list for a given frame.
     * @param index_of_frame  Frame ID key in the frame map.
     */
    std::vector<alcor_finedata_struct> &get_frame_tracking_hits (uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].tracking_hits;  }

    /**
     * @brief Returns a mutable reference to the Cherenkov-hit list for a given frame.
     * @param index_of_frame  Frame ID key in the frame map.
     */
    std::vector<alcor_finedata_struct> &get_frame_cherenkov_hits(uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].cherenkov_hits; }

    // -----------------------------------------------------------------------
    //  Setters — by value (deep copy)
    // -----------------------------------------------------------------------

    /// @brief Replaces the internal spill-data struct (copied).
    void set_spilldata          (alcor_spilldata_struct v)                    { spilldata = v; }

    /// @brief Replaces the frame → light-data map (copied).
    void set_frame              (std::map<uint32_t, alcor_lightdata_struct> v){ spilldata.frame_and_lightdata = v; }

    /// @brief Replaces the participants-mask map (copied).
    void set_participants_mask  (std::map<uint8_t, uint32_t> v)               { spilldata.participants_mask   = v; }

    /// @brief Replaces the dead-mask map (copied).
    void set_dead_mask          (std::map<uint8_t, uint32_t> v)               { spilldata.dead_mask           = v; }

    // -----------------------------------------------------------------------
    //  Setters — by reference (moves/aliases the source)
    // -----------------------------------------------------------------------

    /// @brief Replaces the internal spill-data struct (move-assigned from reference).
    void set_spilldata_link         (alcor_spilldata_struct &v)                    { spilldata                    = v; }

    /// @brief Replaces the frame map (move-assigned from reference).
    void set_frame_link             (std::map<uint32_t, alcor_lightdata_struct> &v){ spilldata.frame_and_lightdata = v; }

    /// @brief Replaces the participants-mask map (move-assigned from reference).
    void set_participants_mask_link (std::map<uint8_t, uint32_t> &v)               { spilldata.participants_mask   = v; }

    /// @brief Replaces the dead-mask map (move-assigned from reference).
    void set_dead_mask_link         (std::map<uint8_t, uint32_t> &v)               { spilldata.dead_mask           = v; }

    // -----------------------------------------------------------------------
    //  Utility methods
    // -----------------------------------------------------------------------

    /**
     * @brief Resets the spill to an empty state, also clearing the deletion registry.
     */
    void clear() { spilldata.clear(); frame_reference_for_deletion.clear(); }

    /**
     * @brief Returns @c true if the given frame contains at least one trigger hit.
     * @param index_of_frame  Frame ID to query.
     */
    bool has_trigger(uint32_t index_of_frame) { return !spilldata.frame_and_lightdata[index_of_frame].trigger_hits.empty(); }

    /**
     * @brief Appends a trigger event to the hit list of the specified frame.
     * @param index_of_frame  Target frame ID.
     * @param trg             Trigger event to append.
     */
    void add_trigger_to_frame(uint32_t index_of_frame, trigger_event trg) { spilldata.frame_and_lightdata[index_of_frame].trigger_hits.push_back(trg); }

    /**
     * @brief Marks a frame to be excluded from the next TTree fill.
     *
     * Frames flagged here are skipped in @c prepare_tree_fill() and their
     * light-data is discarded.  This is the mechanism for online event selection.
     *
     * @param index_of_frame  Frame ID to suppress.
     */
    void do_not_write_frame(uint32_t index_of_frame) { frame_reference_for_deletion[index_of_frame] = true; }

    /**
     * @brief Returns, per device, the list of channels that are participating
     *        but **not** marked as dead.
     *
     * Computes @c (participants_mask & ~dead_mask) for each device, then
     * decodes the result into a vector of active channel indices.
     *
     * Supports both the flat-list representation (filled after a TTree @c GetEntry)
     * and the map-based representation (filled during online processing).
     *
     * @return Map of device ID → vector of active (non-dead) channel indices.
     */
    std::map<uint32_t, std::vector<uint8_t>> get_not_dead_participants();

    /**
     * @brief Returns @c true if this object contains any spill data.
     *
     * Checks whether either the map or list representation of the participants
     * mask is non-empty; an empty spill has neither.
     */
    bool has_data() { return !spilldata.participants_mask.empty() || !spilldata.participants_mask_list.empty(); }

    // -----------------------------------------------------------------------
    //  ROOT TTree I/O
    // -----------------------------------------------------------------------

    /**
     * @brief Binds the flat-vector members to branches of an existing input TTree.
     *
     * Call once before entering the event loop that calls @c TTree::GetEntry.
     * Uses @c SetBranchAddress with the stable @c *_ptr pointers so that ROOT
     * can refill the vectors in-place on each @c GetEntry call.
     *
     * @param input_tree  Pointer to the TTree to read from; no-op if @c nullptr.
     */
    void link_to_tree(TTree *input_tree);

    /**
     * @brief Creates branches on an output TTree and binds them to the flat vectors.
     *
     * Call once before the event loop that calls @c TTree::Fill.
     * The branches written are: @c dead_mask, @c participants_mask,
     * @c frame, and @c lightdata.
     *
     * @param output_tree  Pointer to the TTree to write to; no-op if @c nullptr.
     */
    void write_to_tree(TTree *output_tree);

    /**
     * @brief Converts the working maps into flat vectors ready for @c TTree::Fill.
     *
     * Steps performed:
     *  1. Clears and resets the flat vectors and their branch-address pointers.
     *  2. Transfers @c dead_mask and @c participants_mask into their list counterparts.
     *  3. Iterates over @c frame_and_lightdata, skipping frames registered via
     *     @c do_not_write_frame(), and moves surviving frames into the flat vectors.
     *  4. Clears the now-consumed working maps.
     *
     * @note After this call the working maps are empty; they must be repopulated
     *       before the next spill is processed.
     */
    void prepare_tree_fill();

    /**
     * @brief Placeholder called after @c TTree::GetEntry — currently a no-op.
     *
     * Reserved for post-read bookkeeping (e.g. rebuilding working maps from
     * flat vectors) if needed in future revisions.
     */
    void get_entry() {}

private:
    alcor_spilldata_struct              spilldata;                   ///< Owned spill-data payload.
    std::unordered_map<uint32_t, bool>  frame_reference_for_deletion; ///< Frame IDs suppressed from TTree output.
};

// ============================================================================
//  Free-function merge / reduce utilities
// ============================================================================

/**
 * @brief Merges @p rhs into @p lhs by moving all hit vectors (trigger, timing,
 *        tracking, Cherenkov) from @p rhs into @p lhs.
 *
 * @param lhs  Destination light-data struct (modified in place).
 * @param rhs  Source light-data struct (consumed; left in a valid but unspecified state).
 */
void merge_lightdata(alcor_lightdata_struct &lhs, alcor_lightdata_struct &&rhs);

/**
 * @brief Merges @p rhs into @p lhs for all three working maps.
 *
 * - Dead and participant masks are OR-combined per device.
 * - Frame data is inserted if the key is new, or merged via @c merge_lightdata
 *   if the key already exists.
 *
 * @param lhs  Destination struct (modified in place).
 * @param rhs  Source struct (consumed).
 */
void merge(alcor_spilldata_struct &lhs, alcor_spilldata_struct &&rhs);

/**
 * @brief Convenience overload of @c merge for the wrapper class.
 * @param lhs  Destination (modified in place).
 * @param rhs  Source (consumed).
 */
void merge(alcor_spilldata &lhs, alcor_spilldata &&rhs);

/**
 * @brief Addition operator for @c alcor_spilldata_struct — currently unreliable.
 * @warning This operator is under revision; results may be incorrect.
 *          Prefer @c merge() for production code.
 */
alcor_spilldata_struct operator+(alcor_spilldata_struct lhs, alcor_spilldata_struct rhs);

/**
 * @brief Addition operator for @c alcor_spilldata — currently unreliable.
 * @warning This operator is under revision; results may be incorrect.
 *          Prefer @c merge() for production code.
 */
alcor_spilldata operator+(alcor_spilldata lhs, const alcor_spilldata &rhs);