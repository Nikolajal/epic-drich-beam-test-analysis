#include "alcor_data_streamer.h"

//  Constructors
alcor_data_streamer::alcor_data_streamer(const std::string &fname)
    : filename(fname)
{
  file = TFile::Open(filename.c_str(), "READ");
  if (!file || file->IsZombie())
    return;

  tree = dynamic_cast<TTree *>(file->Get("alcor"));
  if (!tree)
    return;

  data.link_to_tree(tree);
  n_entries = tree->GetEntries();
  valid = true;
}
alcor_data_streamer::alcor_data_streamer(alcor_data_streamer &&other) noexcept
{
  *this = std::move(other);
}
//  Copy Constructor
alcor_data_streamer &alcor_data_streamer::operator=(alcor_data_streamer &&other) noexcept
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
//  Destructor
alcor_data_streamer::~alcor_data_streamer() noexcept
{
  if (tree)
    tree->ResetBranchAddresses();

  if (file)
  {
    file->Close();
    delete file;
  }
}

//  General methods
std::string alcor_data_streamer::get_filename() const noexcept { return filename; }
bool alcor_data_streamer::is_valid() const noexcept { return valid; }
bool alcor_data_streamer::eof() const noexcept { return cursor >= n_entries; }
const alcor_data &alcor_data_streamer::current() const noexcept { return data; }
alcor_data &alcor_data_streamer::current() noexcept { return data; }
Long64_t alcor_data_streamer::entry() const noexcept { return cursor; }
Long64_t alcor_data_streamer::entries() const noexcept { return n_entries; }
bool alcor_data_streamer::read_next()
{
  if (!valid || eof())
    return false;

  tree->GetEntry(cursor++);
  return true;
}
void alcor_data_streamer::rewind() noexcept { cursor = 0; }
