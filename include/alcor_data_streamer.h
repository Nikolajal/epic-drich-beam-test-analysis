#pragma once

/**
 * @file AlcorDataStreamer.h
 * @brief Sequential cursor-based reader for ALCOR Hit-level data in a ROOT TTree.
 *
 * Wraps a ROOT TFile/TTree pair and exposes a minimal streaming API:
 * call read_next() in a loop until eof(), inspecting current() each iteration.
 * Move-only: copying is disabled to prevent double-free of ROOT resources.
 */

#include <string>
#include <utility>
#include <vector>
#include "TTree.h"
#include "alcor_data.h"
#include "utility/root_io.h"

class AlcorDataStreamer
{
public:
    // -------------------------------------------------------------------------
    /** @name Construction & destruction */
    /// @{

    /**
     * @brief Open @p fname and bind to the @c "alcor" TTree.
     * @param fname Path to the ROOT file to read.
     */
    explicit AlcorDataStreamer(const std::string &fname);

    /**
     * @brief Transfer ownership of file/tree resources from @p other.
     * @param other Source streamer; left in an invalid state after the move.
     *
     * Implemented via member-initialiser list so the object is constructed
     * directly from @p other's state without an intermediate default-construction
     * step.  @c std::exchange nulls out the source's owning pointers atomically.
     */
    AlcorDataStreamer(AlcorDataStreamer &&other) noexcept
        : filename(std::move(other.filename)),
          file(std::move(other.file)),
          tree(std::exchange(other.tree, nullptr)),
          device_id(other.device_id),
          fifo_id(other.fifo_id),
          data(std::move(other.data)),
          n_entries(other.n_entries),
          cursor(other.cursor),
          valid(std::exchange(other.valid, false)),
          rollover_count_per_spill(std::move(other.rollover_count_per_spill))
    {
        // CRITICAL: ROOT branch addresses on `tree` still point at the
        // source's `data` fields (set by `data.link_to_tree(tree)` in the
        // source's ctor).  After this move-ctor returns the source goes
        // out of scope; its destructor frees `data`, leaving the branch
        // addresses dangling.  Re-link so the branches target *this*'s
        // `data` fields — SetBranchAddress is idempotent so the last call
        // wins.  Without this, the next `tree->GetEntry()` writes into
        // freed memory (manifesting as "pure virtual function called"
        // crashes inside TTree::GetEntry → TLeaf::ReadValue once the
        // memory gets recycled).
        if (tree)
            data.link_to_tree(tree);
    }

    /**
     * @brief Move-assign; source is left invalid after the operation.
     * @param other Source streamer to move from.
     * @return Reference to @c *this.
     */
    AlcorDataStreamer &operator=(AlcorDataStreamer &&other) noexcept;

    AlcorDataStreamer(const AlcorDataStreamer &) = delete;
    AlcorDataStreamer &operator=(const AlcorDataStreamer &) = delete;

    /// Reset branch addresses, close and delete the underlying TFile.
    ~AlcorDataStreamer() noexcept;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Status checks */
    /// @{

    /// @c true if the file and TTree were opened successfully.
    bool is_valid() const noexcept { return valid; }

    /// @c true once the cursor has passed the last entry.
    bool eof() const noexcept { return cursor >= n_entries; }

    /// @}

    // -------------------------------------------------------------------------
    /** @name Accessors */
    /// @{

    /// Path of the underlying ROOT file.
    std::string get_filename() const noexcept { return filename; }

    /// Read-only view of the entry last loaded by read_next().
    const AlcorData &current() const noexcept { return data; }

    /// Mutable view of the entry last loaded by read_next().
    AlcorData &current() noexcept { return data; }

    /// Zero-based index of the entry that will be read next.
    Long64_t entry() const noexcept { return cursor; }

    /// Total number of entries in the TTree.
    Long64_t entries() const noexcept { return n_entries; }

    /// Rollover count read from gRollover at file open, one entry per spill.
    /// Empty if gRollover was absent from the file.
    const std::vector<double> &get_rollover_count_per_spill() const noexcept { return rollover_count_per_spill; }

    /// @}

    // -------------------------------------------------------------------------
    /** @name Reading */
    /// @{

    /// Load the next entry into current(); returns @c false at EOF or on error.
    bool read_next() noexcept
    {
        if (!valid || eof())
            return false;
        tree->GetEntry(cursor++);
        if (data.get_data().device <= 0 && device_id > 0)
            data.set_device(device_id);
        if (fifo_id >= 0 && data.get_data().fifo != fifo_id)
        {
            //  The upstream decoder labels FIFO N's data with the wrong lane
            //  (raw fifo != filename fifo) AND encodes pixel/column for that
            //  wrong lane's slot within its chip. Remap channel-within-chip to
            //  the target lane's slot so that pixel + 4*column matches the
            //  filename-declared fifo.
            const auto &d = data.get_data();
            const int raw_ch_in_chip = d.pixel + 4 * d.column;
            const int ch_in_fifo = raw_ch_in_chip % 8;
            const int new_ch_in_chip = 8 * (fifo_id % 4) + ch_in_fifo;
            data.set_fifo(fifo_id);
            data.set_pixel(new_ch_in_chip % 4);
            data.set_column(new_ch_in_chip / 4);
        }
        return true;
    }

    /// Reset the cursor to 0 without re-opening the file.
    void rewind() noexcept { cursor = 0; }

    /// @}

private:
    // -------------------------------------------------------------------------
    /** @name Internal state */
    /// @{

    std::string filename;                         ///< Path of the ROOT file being read.
    TFilePtr file;                                ///< Owning handle to the open TFile (closes on destruction).
    TTree *tree = nullptr;                        ///< Non-owning pointer into @c file.
    int device_id = -1;                           ///< RDO id parsed from "rdo-NNN" in the path; -1 if not found. Used to overwrite the bogus upstream device branch.
    int fifo_id = -1;                             ///< FIFO id parsed from "fifo_NN" in the filename; -1 if not found. Used to overwrite the fifo branch per-Hit.
    AlcorData data;                               ///< Buffer filled by each read_next() call.
    Long64_t n_entries = 0;                       ///< Total entries cached from the TTree.
    Long64_t cursor = 0;                          ///< Index of the next entry to be read.
    bool valid = false;                           ///< Set @c true only after successful open.
    std::vector<double> rollover_count_per_spill; ///< gRollover y-values, one per spill.

    /// @}
};