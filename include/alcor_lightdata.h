#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include "alcor_finedata.h"
#include "triggers.h"
#include "config_reader.h"

/**
 * @brief Structure holding all types of light detector hits for a spill.
 *
 * This struct stores trigger hits, timing hits, tracking hits, and Cherenkov hits.
 * It is a simple container with a clear() method to reset all vectors.
 */
struct alcor_lightdata_struct
{
    std::vector<trigger_struct> trigger_hits;
    std::vector<alcor_finedata_struct> timing_hits;
    std::vector<alcor_finedata_struct> tracking_hits;
    std::vector<alcor_finedata_struct> cherenkov_hits;

    /**
     * @brief Clear all vectors and release memory.
     *
     * This sets all vectors empty and calls shrink_to_fit() to free memory.
     */
    void clear();
};

/**
 * @brief Class wrapping lightdata hits with convenient accessors and utilities.
 *
 * Inherits from alcor_finedata and provides getters, setters, and utilities
 * to work with trigger, timing, tracking, and Cherenkov hits.
 */
class alcor_lightdata : public alcor_finedata
{
private:
    alcor_lightdata_struct lightdata;

public:
    alcor_lightdata() = default;
    alcor_lightdata(const alcor_lightdata_struct &data_struct);

    /** @name Getters */
    ///@{
    /**
     * @brief Get a copy of the internal lightdata struct.
     * @return Copy of alcor_lightdata_struct
     */
    alcor_lightdata_struct get_lightdata() const;

    /**
     * @brief Get a copy of the timing hits.
     * @return Vector of timing hit structs
     */
    std::vector<alcor_finedata_struct> get_timing_hits() const;

    /**
     * @brief Get a copy of the tracking hits.
     * @return Vector of tracking hit structs
     */
    std::vector<alcor_finedata_struct> get_tracking_hits() const;

    /**
     * @brief Get a copy of the Cherenkov hits.
     * @return Vector of Cherenkov hit structs
     */
    std::vector<alcor_finedata_struct> get_cherenkov_hits() const;

    /**
     * @brief Get a copy of trigger hits.
     * @return Vector of trigger_struct
     */
    std::vector<trigger_struct> get_triggers() const;

    /**
     * @brief Get a reference to the internal lightdata struct.
     * @return Reference to alcor_lightdata_struct
     */
    alcor_lightdata_struct &get_lightdata_link();

    /**
     * @brief Get a reference to the internal timing hits.
     * @return Reference to vector of timing hit structs
     */
    std::vector<alcor_finedata_struct> &get_timing_hits_link();

    /**
     * @brief Get a reference to the internal tracking hits.
     * @return Reference to vector of tracking hit structs
     */
    std::vector<alcor_finedata_struct> &get_tracking_hits_link();

    /**
     * @brief Get a reference to the internal Cherenkov hits.
     * @return Reference to vector of Cherenkov hit structs
     */
    std::vector<alcor_finedata_struct> &get_cherenkov_hits_link();

    /**
     * @brief Get a reference to the internal trigger hits.
     * @return Reference to vector of trigger_struct
     */
    std::vector<trigger_struct> &get_triggers_link();
    ///@}

    /** @name Setters */
    ///@{
    /**
     * @brief Set the internal lightdata struct.
     * @param v Copy of lightdata struct
     */
    void set_lightdata(alcor_lightdata_struct v);

    void set_timing_hits(std::vector<alcor_finedata_struct> v);
    void set_tracking_hits(std::vector<alcor_finedata_struct> v);
    void set_cherenkov_hits(std::vector<alcor_finedata_struct> v);
    void set_trigger(std::vector<trigger_struct> v);

    void set_lightdata_link(alcor_lightdata_struct &v);
    void set_timing_hits_link(std::vector<alcor_finedata_struct> &v);
    void set_tracking_hits_link(std::vector<alcor_finedata_struct> &v);
    void set_cherenkov_hits_link(std::vector<alcor_finedata_struct> &v);
    void set_trigger_link(std::vector<trigger_struct> &v);
    ///@}

    /** @name Utility Methods */
    ///@{
    /**
     * @brief Get the trigger time for a given trigger index.
     * @param trigger_index Index of the trigger in the vector
     * @return Time value of the trigger in ns
     */
    std::optional<float>  get_trigger_time(uint8_t trigger_index);
    ///@}
};
