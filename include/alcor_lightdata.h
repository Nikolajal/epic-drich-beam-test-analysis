#pragma once

/**
 * @file AlcorLightdata.h
 * @brief Per-spill container of categorised ALCOR hits.
 *
 * Holds four parallel Hit vectors — Cherenkov, timing, tracking, and trigger
 * — corresponding to the categories assigned by the readout configuration
 * file.  Output of @c lightdata_writer; consumed by @c recodata_writer.
 */

#include <vector>
#include <cstdint>
#include <algorithm>
#include "alcor_finedata.h"
#include "triggers.h"
#include "utility/config_reader.h"

/**
 * @brief Structure holding all types of light detector hits for a spill.
 *
 * Stores trigger hits, timing hits, tracking hits, and Cherenkov hits.
 * Simple container with a @c clear() method to reset all vectors.
 */
struct AlcorLightdataStruct
{
    std::vector<TriggerEvent> trigger_hits;
    std::vector<AlcorFinedataStruct> timing_hits;
    std::vector<AlcorFinedataStruct> tracking_hits;
    std::vector<AlcorFinedataStruct> cherenkov_hits;

    /**
     * @brief Per-frame streaming-RANSAC ring geometry (first / second radiator).
     *
     * The streaming ring finder runs a completeness-corrected, sensor-fiducial
     * RANSAC + Taubin fit on the frame's Cherenkov hits — a robust estimate of
     * the (centre, radius) even for short far-off-centre arcs.  These scalars
     * carry that estimate downstream so `recodata_writer` can **seed** its
     * per-ring fit from it instead of re-finding the geometry from scratch (a
     * free re-fit on a sparse short arc is high-variance and can collapse the
     * far centre back toward the origin).  `radius == 0` ⇒ no ring tagged in
     * this slot (the unset sentinel).  Populated in `run_streaming_ransac_trigger`.
     */
    float ring1_cx = 0.f;
    float ring1_cy = 0.f;
    float ring1_radius = 0.f;
    float ring2_cx = 0.f;
    float ring2_cy = 0.f;
    float ring2_radius = 0.f;

    /**
     * @brief Clear all vectors and release memory.
     *
     * Empties all Hit vectors and calls @c shrink_to_fit() on each to return
     * heap storage to the allocator.  Also resets the per-frame ring scalars.
     */
    void clear();
};

/**
 * @brief Accessor class for light-detector Hit collections.
 *
 * Inherits from @ref AlcorFinedata and provides value-copy and reference
 * getters/setters for trigger, timing, tracking, and Cherenkov Hit vectors,
 * as well as a utility method to look up a trigger by index.
 */
class AlcorLightdata : public AlcorFinedata
{
private:
    AlcorLightdataStruct lightdata;

public:
    /// @brief Default constructor — all Hit vectors empty.
    AlcorLightdata() = default;

    /// @brief Construct from an existing lightdata struct (copied).
    AlcorLightdata(const AlcorLightdataStruct &data_struct) : lightdata(data_struct) {}

    /** @name Getters — by value */
    ///@{

    /**
     * @brief Return a copy of the internal lightdata struct.
     * @return Copy of @ref AlcorLightdataStruct.
     */
    AlcorLightdataStruct get_lightdata() const { return lightdata; }

    /// @brief Return a copy of the timing Hit vector.
    std::vector<AlcorFinedataStruct> get_timing_hits() const { return lightdata.timing_hits; }

    /// @brief Return a copy of the tracking Hit vector.
    std::vector<AlcorFinedataStruct> get_tracking_hits() const { return lightdata.tracking_hits; }

    /// @brief Return a copy of the Cherenkov Hit vector.
    std::vector<AlcorFinedataStruct> get_cherenkov_hits() const { return lightdata.cherenkov_hits; }

    /// @brief Return a copy of the trigger Hit vector.
    std::vector<TriggerEvent> get_triggers() const { return lightdata.trigger_hits; }

    ///@}

    /** @name Getters — by reference */
    ///@{

    /// @brief Return a mutable reference to the internal lightdata struct.
    AlcorLightdataStruct &get_lightdata_link() { return lightdata; }

    /// @brief Return a mutable reference to the timing Hit vector.
    std::vector<AlcorFinedataStruct> &get_timing_hits_link() { return lightdata.timing_hits; }

    /// @brief Return a mutable reference to the tracking Hit vector.
    std::vector<AlcorFinedataStruct> &get_tracking_hits_link() { return lightdata.tracking_hits; }

    /// @brief Return a mutable reference to the Cherenkov Hit vector.
    std::vector<AlcorFinedataStruct> &get_cherenkov_hits_link() { return lightdata.cherenkov_hits; }

    /// @brief Return a mutable reference to the trigger Hit vector.
    std::vector<TriggerEvent> &get_triggers_link() { return lightdata.trigger_hits; }

    ///@}

    /** @name Setters — by value */
    ///@{

    /**
     * @brief Replace the internal lightdata struct (copied).
     * @param v New @ref AlcorLightdataStruct to assign.
     */
    void set_lightdata(AlcorLightdataStruct v) { lightdata = v; }

    /// @brief Replace the timing Hit vector.
    void set_timing_hits(std::vector<AlcorFinedataStruct> v) { lightdata.timing_hits = v; }

    /// @brief Replace the tracking Hit vector.
    void set_tracking_hits(std::vector<AlcorFinedataStruct> v) { lightdata.tracking_hits = v; }

    /// @brief Replace the Cherenkov Hit vector.
    void set_cherenkov_hits(std::vector<AlcorFinedataStruct> v) { lightdata.cherenkov_hits = v; }

    /// @brief Replace the trigger Hit vector.
    void set_trigger(std::vector<TriggerEvent> v) { lightdata.trigger_hits = v; }

    ///@}

    /** @name Setters — by reference */
    ///@{

    /// @brief Replace the internal lightdata struct (assigned from reference).
    void set_lightdata_link(AlcorLightdataStruct &v) { lightdata = v; }

    /// @brief Replace the timing Hit vector (assigned from reference).
    void set_timing_hits_link(std::vector<AlcorFinedataStruct> &v) { lightdata.timing_hits = v; }

    /// @brief Replace the tracking Hit vector (assigned from reference).
    void set_tracking_hits_link(std::vector<AlcorFinedataStruct> &v) { lightdata.tracking_hits = v; }

    /// @brief Replace the Cherenkov Hit vector (assigned from reference).
    void set_cherenkov_hits_link(std::vector<AlcorFinedataStruct> &v) { lightdata.cherenkov_hits = v; }

    /// @brief Replace the trigger Hit vector (assigned from reference).
    void set_trigger_link(std::vector<TriggerEvent> &v) { lightdata.trigger_hits = v; }

    ///@}

    /** @name Utility */
    ///@{

    /**
     * @brief Return the fine time of the first trigger matching @p trigger_index.
     * @param trigger_index Hardware trigger index to search for.
     * @return Fine time [ns], or @c std::nullopt if no matching trigger is present.
     */
    std::optional<float> get_trigger_time(uint8_t trigger_index);

    ///@}
};
