#pragma once

/**
 * @file alcor_data_streamer.h
 * @brief Data structures and utilities for ALCOR hit-level data handling.
 *
 * This header defines:
 * * A base, retro compatible data struct (@ref alcor_data_struct) that reads data from ALCOR decoder
 * * Enumerations describing hit types and bit-level hit masks.
 * * A lightweight wrapper class (@ref alcor_data) that provides:
 *  - Safe accessors and mutators
 *  - Derived quantities (indices, global time, etc.)
 *  - Utility predicates (hit type checks)
 *  - ROOT I/O helpers
 *
 * The design intentionally separates *storage* (struct) from *logic*
 * (class) to keep ROOT I/O simple while allowing richer semantics
 * at the analysis level.
 */

#include <string>
#include "TFile.h"
#include "TTree.h"
#include "alcor_data.h"

/**
 * @brief Streamer for reading ePIC dRICH ALCOR beam-test data from a ROOT TTree.
 *
 * This class wraps a ROOT TFile and TTree, providing sequential access to
 * `alcor_data` objects stored in the file. It supports move semantics but
 * disables copy construction and assignment to ensure safe resource management.
 */
class alcor_data_streamer
{
public:
  explicit alcor_data_streamer(const std::string &fname);

  // move only
  alcor_data_streamer(alcor_data_streamer &&) noexcept;
  alcor_data_streamer &operator=(alcor_data_streamer &&) noexcept;

  alcor_data_streamer(const alcor_data_streamer &) = delete;
  alcor_data_streamer &operator=(const alcor_data_streamer &) = delete;

  ~alcor_data_streamer() noexcept;

  // status
  /** @name Status checks */
  ///@{
  /**
   * @brief Check if the streamer is valid (file and tree opened successfully).
   * @return True if valid, false otherwise.
   */
  bool is_valid() const noexcept;

  /**
   * @brief Check if the streamer has reached the end of the TTree.
   * @return True if all entries have been read.
   */
  bool eof() const noexcept;
  ///@}

  /** @name Accessors */
  ///@{
  /**
   * @brief Get the ROOT file name used by this streamer.
   * @return The filename string.
   */
  std::string get_filename() const noexcept;

  /**
   * @brief Get the current `alcor_data` object (read-only).
   * @return Constant reference to the current data entry.
   */
  const alcor_data &current() const noexcept;

  /**
   * @brief Get the current `alcor_data` object (modifiable).
   * @return Reference to the current data entry.
   */
  alcor_data &current() noexcept;

  /**
   * @brief Get the current entry index in the TTree.
   * @return Index of the current entry (0-based).
   */
  Long64_t entry() const noexcept;

  /**
   * @brief Get the total number of entries in the TTree.
   * @return Total number of entries.
   */
  Long64_t entries() const noexcept;
  ///@}

  /** @name Reading methods */
  ///@{
  /**
   * @brief Read the next entry from the TTree.
   *
   * Updates the internal cursor and current `alcor_data`.
   * @return True if an entry was read successfully, false if EOF or error.
   */
  bool read_next();

  /**
   * @brief Rewind the streamer to the first entry.
   *
   * Resets the cursor to zero without closing the file.
   */
  void rewind() noexcept;
  ///@}

private:
  //  Filename to read
  std::string filename;

  //  Internal pointers to TFile and TTree
  TFile *file = nullptr;
  TTree *tree = nullptr;

  //  Data container
  alcor_data data;

  //  Utilities
  Long64_t n_entries = 0;
  Long64_t cursor = 0;
  bool valid = false;
};