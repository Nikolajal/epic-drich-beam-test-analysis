#pragma once

#include <string>
#include "TFile.h"
#include "TTree.h"
#include "alcor_data.h"

class alcor_data_streamer
{
public:
  // --- Constructor
  explicit alcor_data_streamer(const std::string &fname)
      : filename(fname)
  {
    file = TFile::Open(filename.c_str(), "READ");
    if (!file || file->IsZombie())
      return;

    tree = dynamic_cast<TTree *>(file->Get("alcor"));
    if (!tree)
      return;

    data.link_to_tree(tree); // bind branches ONCE
    n_entries = tree->GetEntries();

    valid = true;
  }

  // --- Move only (IMPORTANT)
  alcor_data_streamer(alcor_data_streamer &&other) noexcept { *this = std::move(other); }

  alcor_data_streamer &operator=(alcor_data_streamer &&other) noexcept
  {
    if (this != &other)
    {
      filename = std::move(other.filename);
      file = other.file;
      tree = other.tree;
      data = std::move(other.data);
      n_entries = other.n_entries;
      cursor = other.cursor;
      valid = other.valid;

      other.file = nullptr;
      other.tree = nullptr;
      other.valid = false;
    }
    return *this;
  }

  // --- Disable copy to avoid two objects pointing to the same memory spot
  alcor_data_streamer(const alcor_data_streamer &) = delete;
  alcor_data_streamer &operator=(const alcor_data_streamer &) = delete;

  // --- Destructor
  ~alcor_data_streamer() noexcept
  {
    if (tree)
      tree->ResetBranchAddresses();

    if (file)
    {
      file->Close();
      delete file;
    }
  }

  // --- Status (file opened and streamer successfully initialized)
  bool is_valid() const noexcept { return valid; }
  bool eof() const noexcept { return cursor >= n_entries; }

  // --- Data access
  std::string get_filename() const noexcept { return filename; }
  const alcor_data &current() const noexcept { return data; }
  alcor_data &current() noexcept { return data; }
  Long64_t entry() const noexcept { return cursor; }
  Long64_t entries() const noexcept { return n_entries; }

  // --- Reading
  bool read_next()
  {
    if (!valid || eof())
      return false;

    tree->GetEntry(cursor++);
    return true;
  }

  // --- Control
  void rewind() noexcept { cursor = 0; }

private:
  std::string filename;

  TFile *file = nullptr;
  TTree *tree = nullptr;

  alcor_data data;

  Long64_t n_entries = 0;
  Long64_t cursor = 0;

  bool valid = false;
};
