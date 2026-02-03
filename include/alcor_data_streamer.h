#pragma once

#include <string>
#include "TFile.h"
#include "TTree.h"
#include "alcor_data.h"

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
  bool is_valid() const noexcept;
  bool eof() const noexcept;

  // access
  std::string get_filename() const noexcept;
  const alcor_data &current() const noexcept;
  alcor_data &current() noexcept;
  Long64_t entry() const noexcept;
  Long64_t entries() const noexcept;

  // reading
  bool read_next();
  void rewind() noexcept;

private:
  std::string filename;

  TFile *file = nullptr;
  TTree *tree = nullptr;

  alcor_data data;

  Long64_t n_entries = 0;
  Long64_t cursor = 0;
  bool valid = false;
};