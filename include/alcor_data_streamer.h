#pragma once

/**
 * @file alcor_data_streamer.h
 * @brief Sequential cursor-based reader for ALCOR hit-level data in a ROOT TTree.
 *
 * Wraps a ROOT TFile/TTree pair and exposes a minimal streaming API:
 * call read_next() in a loop until eof(), inspecting current() each iteration.
 * Move-only: copying is disabled to prevent double-free of ROOT resources.
 */

#include <string>
#include "TFile.h"
#include "TTree.h"
#include "alcor_data.h"

class alcor_data_streamer
{
public:
    // -------------------------------------------------------------------------
    /** @name Construction & destruction */
    /// @{

    /**
     * @brief Open @p fname and bind to the @c "alcor" TTree.
     * @param fname Path to the ROOT file to read.
     */
    explicit alcor_data_streamer(const std::string &fname);

    /**
     * @brief Transfer ownership of file/tree resources from @p other.
     * @param other Source streamer; left in an invalid state after the move.
     */
    alcor_data_streamer(alcor_data_streamer &&other) noexcept;

    /**
     * @brief Move-assign; source is left invalid after the operation.
     * @param other Source streamer to move from.
     * @return Reference to @c *this.
     */
    alcor_data_streamer &operator=(alcor_data_streamer &&other) noexcept;

    alcor_data_streamer(const alcor_data_streamer &) = delete;
    alcor_data_streamer &operator=(const alcor_data_streamer &) = delete;

    /// Reset branch addresses, close and delete the underlying TFile.
    ~alcor_data_streamer() noexcept;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Status checks */
    /// @{

    /// @c true if the file and TTree were opened successfully.
    bool is_valid() const noexcept;

    /// @c true once the cursor has passed the last entry.
    bool eof() const noexcept;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Accessors */
    /// @{

    /// Path of the underlying ROOT file.
    std::string get_filename() const noexcept;

    /// Read-only view of the entry last loaded by read_next().
    const alcor_data &current() const noexcept;

    /// Mutable view of the entry last loaded by read_next().
    alcor_data &current() noexcept;

    /// Zero-based index of the entry that will be read next.
    Long64_t entry() const noexcept;

    /// Total number of entries in the TTree.
    Long64_t entries() const noexcept;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Reading */
    /// @{

    /// Load the next entry into current(); returns @c false at EOF or on error.
    bool read_next() noexcept;

    /// Reset the cursor to 0 without re-opening the file.
    void rewind() noexcept;

    /// @}

private:
    // -------------------------------------------------------------------------
    /** @name Internal state */
    /// @{

    std::string filename;   ///< Path of the ROOT file being read.
    TFile *file = nullptr;  ///< Owning pointer to the open TFile.
    TTree *tree = nullptr;  ///< Non-owning pointer into @c file.
    alcor_data data;        ///< Buffer filled by each read_next() call.
    Long64_t n_entries = 0; ///< Total entries cached from the TTree.
    Long64_t cursor = 0;    ///< Index of the next entry to be read.
    bool valid = false;     ///< Set @c true only after successful open.

    /// @}
};