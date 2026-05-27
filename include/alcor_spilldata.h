#pragma once

/**
 * @file AlcorSpilldata.h
 * @brief Per-spill metadata: dead-channel masks, active-participant masks,
 *        and the @ref AlcorLightdataStruct payload for the spill.
 *
 * **Two-layer design**  This header exposes two related
 * types with split responsibilities:
 *
 * - @ref AlcorSpilldataStruct — the **POD data side**.  Owns the vectors and
 *   maps that hold spill content.  Move-only (deep-copying a multi-MB spill
 *   is never the intent).  Contains no pointers, no I/O, no ROOT state.
 *
 * - @ref AlcorSpilldata — the **wrapper / I/O side**.  Owns one
 *   `AlcorSpilldataStruct` plus the four pointer slots required by ROOT's
 *   `TTree::SetBranchAddress`.  Non-copyable AND non-movable: ROOT branches
 *   bind to the address of the wrapper's `_ptr_` slots, so any wrapper move
 *   would leave those branches dangling.  Hold by reference, by
 *   `std::unique_ptr`, or as a class member.
 *
 * The split closes three latent bugs in the previous one-class design:
 *  - F1: wrapper moves dangling branch addresses.
 *  - F2: struct copy duplicating `*_ptr` slots into the destination.
 *  - F3: by-value setter `set_spilldata(AlcorSpilldataStruct v)` making
 *    `*_ptr` point at a dead temporary.
 *
 * Migration notes:
 *  - The previous by-value getters (`get_spilldata()`, `get_frame()`, …) and
 *    setters (`set_spilldata(...)`, …) are removed — they had no callers and
 *    would not compile against the move-only struct.  Use @ref data() and
 *    the existing `_link` accessors instead.
 *  - The previous `operator+(AlcorSpilldata, …)` overloads (marked
 *    unreliable) are removed.
 *  - The previous `merge(AlcorSpilldata&, AlcorSpilldata&&)` wrapper is
 *    removed; it self-defeatingly deep-copied via the by-value getter.
 *    Use `merge(lhs.data(), std::move(rhs.data()))` instead.
 */

#include <cstdint>
#include <iostream>
#include <map>
#include <unordered_map>
#include <vector>

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
struct DataMaskStruct
{
    uint8_t device; ///< Device (ALCOR chip) identifier.
    uint32_t mask;  ///< Bitmask encoding channel states (e.g. alive/dead).
};

/**
 * @brief POD aggregate that owns all spill information for one beam spill.
 *
 * Holds two parallel representations of the same data:
 *  - **Working maps** (`dead_mask`, `participants_mask`, `frame_and_lightdata`)
 *    used during online processing where random-access by key is needed.
 *  - **Flat vectors** (`*_list`, `frame_reference`, `lightdata_list_in_frame`)
 *    used for ROOT TTree I/O, which requires contiguous, pointer-stable storage.
 *
 * **Move-only.**  Copying a populated spill is never intended (multi-MB of
 * Hit data + maps); accidental copies should be compile errors.  Pass by
 * `const AlcorSpilldataStruct&` for read access, by `AlcorSpilldataStruct&&`
 * when transferring ownership.
 *
 * **No branch-address pointers here.**  The `*_ptr` slots that used to live
 * on this struct now live on @ref AlcorSpilldata so that copy/move semantics
 * do not affect ROOT's binding state.
 */
struct AlcorSpilldataStruct
{
    // --- Working maps (random-access processing) ------------------------
    std::map<uint8_t, uint32_t> dead_mask;                        ///< device → dead-channel bitmask.
    std::map<uint8_t, uint32_t> participants_mask;                ///< device → participating-channel bitmask.
    std::map<uint32_t, AlcorLightdataStruct> frame_and_lightdata; ///< frame_id → light-data payload.

    // --- Flat vectors (ROOT TTree serialisation) -------------------------
    std::vector<DataMaskStruct> dead_mask_list;                ///< Flat copy of @c dead_mask for TTree output.
    std::vector<DataMaskStruct> participants_mask_list;        ///< Flat copy of @c participants_mask for TTree output.
    std::vector<uint32_t> frame_reference;                     ///< Ordered list of frame IDs written to the TTree.
    std::vector<AlcorLightdataStruct> lightdata_list_in_frame; ///< Light-data entries parallel to @c frame_reference.

    // --- Special members: move-only --------------------------------------
    AlcorSpilldataStruct() noexcept = default;
    AlcorSpilldataStruct(const AlcorSpilldataStruct &) = delete;
    AlcorSpilldataStruct &operator=(const AlcorSpilldataStruct &) = delete;
    AlcorSpilldataStruct(AlcorSpilldataStruct &&) noexcept = default;
    AlcorSpilldataStruct &operator=(AlcorSpilldataStruct &&) noexcept = default;

    /// @brief Resets all members to empty (containers cleared + capacity dropped).
    void clear();
};

// ============================================================================
//  AlcorSpilldata class
// ============================================================================

/**
 * @brief High-level accessor / I/O wrapper for one beam-spill of ALCOR data.
 *
 * Owns one @ref AlcorSpilldataStruct (move-only POD) plus the four
 * pointer-to-vector slots that ROOT's `TTree::SetBranchAddress` requires.
 * Adds:
 *  - Typed getters returning references into the wrapped data.
 *  - Frame-level helpers (trigger queries, selective frame suppression).
 *  - ROOT TTree I/O (`link_to_tree`, `write_to_tree`, `prepare_tree_fill`).
 *
 * **Non-copyable, non-movable.**  ROOT branches bind to the address of this
 * object's `_ptr_` slots; any move would leave those branches dangling into
 * the source.  Hold by reference, by `std::unique_ptr`, or as a class
 * member — the project's existing patterns.
 *
 * Inherits from @c AlcorLightdata to reuse common Hit-decoding infrastructure.
 */
class AlcorSpilldata : public AlcorLightdata
{
public:
    // -----------------------------------------------------------------------
    //  Construction
    // -----------------------------------------------------------------------

    /// @brief Default constructor — empty spill, branch pointers wired to data.
    AlcorSpilldata() noexcept { sync_ptrs_(); }

    /**
     * @brief Construct from an existing data struct (moved in).
     * @param d  Source struct; moved into internal storage.
     */
    explicit AlcorSpilldata(AlcorSpilldataStruct &&d) noexcept
        : spilldata(std::move(d)) { sync_ptrs_(); }

    AlcorSpilldata(const AlcorSpilldata &) = delete;
    AlcorSpilldata &operator=(const AlcorSpilldata &) = delete;
    AlcorSpilldata(AlcorSpilldata &&) = delete;
    AlcorSpilldata &operator=(AlcorSpilldata &&) = delete;

    // -----------------------------------------------------------------------
    //  Data access
    // -----------------------------------------------------------------------

    /// @brief Read-only access to the underlying data struct.
    const AlcorSpilldataStruct &data() const noexcept { return spilldata; }

    /// @brief Mutable access to the underlying data struct.
    AlcorSpilldataStruct &data() noexcept { return spilldata; }

    // -----------------------------------------------------------------------
    //  Reference getters into the working maps (zero-copy)
    // -----------------------------------------------------------------------

    /// @brief Mutable reference to the underlying data struct (legacy alias).
    AlcorSpilldataStruct &get_spilldata_link() noexcept { return spilldata; }

    /// @brief Mutable reference to the frame → light-data map.
    std::map<uint32_t, AlcorLightdataStruct> &get_frame_link() noexcept { return spilldata.frame_and_lightdata; }

    /// @brief Mutable reference to the participants-mask map.
    std::map<uint8_t, uint32_t> &get_participants_mask_link() noexcept { return spilldata.participants_mask; }

    /// @brief Mutable reference to the dead-mask map.
    std::map<uint8_t, uint32_t> &get_dead_mask_link() noexcept { return spilldata.dead_mask; }

    /// @brief Mutable reference to the flat light-data vector.
    std::vector<AlcorLightdataStruct> &get_frame_list_link() noexcept { return spilldata.lightdata_list_in_frame; }

    /// @brief Mutable reference to the flat frame-reference vector.
    std::vector<uint32_t> &get_frame_reference_list_link() noexcept { return spilldata.frame_reference; }

    // -----------------------------------------------------------------------
    //  Per-frame Hit-vector getters
    // -----------------------------------------------------------------------

    std::vector<TriggerEvent> &get_frame_trigger_hits(uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].trigger_hits; }
    std::vector<AlcorFinedataStruct> &get_frame_timing_hits(uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].timing_hits; }
    std::vector<AlcorFinedataStruct> &get_frame_tracking_hits(uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].tracking_hits; }
    std::vector<AlcorFinedataStruct> &get_frame_cherenkov_hits(uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].cherenkov_hits; }

    // -----------------------------------------------------------------------
    //  Utility methods
    // -----------------------------------------------------------------------

    /// @brief Resets the spill to an empty state, also clearing the deletion registry.
    void clear()
    {
        spilldata.clear();
        frame_reference_for_deletion.clear();
    }

    /// @brief Returns @c true if the given frame contains at least one trigger Hit.
    bool has_trigger(uint32_t index_of_frame) { return !spilldata.frame_and_lightdata[index_of_frame].trigger_hits.empty(); }

    /// @brief Appends a trigger event to the Hit list of the specified frame.
    void add_trigger_to_frame(uint32_t index_of_frame, TriggerEvent trg) { spilldata.frame_and_lightdata[index_of_frame].trigger_hits.push_back(trg); }

    /// @brief Marks a frame to be excluded from the next TTree fill.
    void do_not_write_frame(uint32_t index_of_frame) { frame_reference_for_deletion[index_of_frame] = true; }

    /**
     * @brief Returns, per device, the list of channels that are participating
     *        but **not** marked as dead.
     */
    std::map<uint32_t, std::vector<uint8_t>> get_not_dead_participants();

    /// @brief Returns @c true if this object contains any spill data.
    bool has_data() const noexcept { return !spilldata.participants_mask.empty() || !spilldata.participants_mask_list.empty(); }

    // -----------------------------------------------------------------------
    //  ROOT TTree I/O
    // -----------------------------------------------------------------------

    /**
     * @brief Binds the flat-vector members to branches of an existing input TTree.
     *
     * Call once before entering the event loop that calls @c TTree::GetEntry.
     * Uses the wrapper's @c _ptr_ slots so that ROOT can refill the vectors
     * in-place on each @c GetEntry call.
     */
    void link_to_tree(TTree *input_tree);

    /// @brief Creates branches on an output TTree and binds them to the flat vectors.
    void write_to_tree(TTree *output_tree);

    /**
     * @brief Converts the working maps into flat vectors ready for @c TTree::Fill.
     *
     * Steps performed:
     *  1. Clears the flat vectors (capacity preserved between spills).
     *  2. Transfers @c dead_mask and @c participants_mask into their list counterparts.
     *  3. Iterates over @c frame_and_lightdata, skipping frames registered via
     *     @ref do_not_write_frame, moving surviving frames into the flat vectors.
     *  4. Clears the now-consumed working maps.
     */
    void prepare_tree_fill();

    /// @brief Placeholder called after @c TTree::GetEntry — reserved for future use.
    void get_entry() {}

private:
    /// Re-anchor branch-address pointer slots at @c spilldata's vectors.
    /// Called from constructors; never called again because this wrapper is
    /// non-movable, so the addresses are stable for its lifetime.
    void sync_ptrs_() noexcept
    {
        dead_mask_list_ptr_ = &spilldata.dead_mask_list;
        participants_mask_list_ptr_ = &spilldata.participants_mask_list;
        frame_reference_ptr_ = &spilldata.frame_reference;
        lightdata_list_in_frame_ptr_ = &spilldata.lightdata_list_in_frame;
    }

    AlcorSpilldataStruct spilldata;                                  ///< Owned spill-data payload.
    std::unordered_map<uint32_t, bool> frame_reference_for_deletion; ///< Frame IDs suppressed from TTree output.

    // Branch-address pointer slots — live HERE (not in the POD struct).
    // Stable for the wrapper's lifetime since the class is non-movable.
    std::vector<DataMaskStruct> *dead_mask_list_ptr_ = nullptr;
    std::vector<DataMaskStruct> *participants_mask_list_ptr_ = nullptr;
    std::vector<uint32_t> *frame_reference_ptr_ = nullptr;
    std::vector<AlcorLightdataStruct> *lightdata_list_in_frame_ptr_ = nullptr;
};

// ============================================================================
//  Free-function merge utilities
// ============================================================================

/**
 * @brief Merges @p rhs into @p lhs by moving all Hit vectors (trigger, timing,
 *        tracking, Cherenkov) from @p rhs into @p lhs.
 */
void merge_lightdata(AlcorLightdataStruct &lhs, AlcorLightdataStruct &&rhs);

/**
 * @brief Merges @p rhs into @p lhs for all three working maps.
 *
 * - Dead and participant masks are OR-combined per device.
 * - Frame data is inserted if the key is new, or merged via @ref merge_lightdata
 *   if the key already exists.
 */
void merge(AlcorSpilldataStruct &lhs, AlcorSpilldataStruct &&rhs);
