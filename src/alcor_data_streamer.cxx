#include "TGraph.h"
#include "alcor_data_streamer.h"
#include <mist/logger/logger.h>
#include "TString.h"

// --- construction & destruction ------------------------------------------

AlcorDataStreamer::AlcorDataStreamer(const std::string &fname)
    : filename(fname)
{
    file.reset(TFile::Open(filename.c_str(), "READ"));
    if (!file || file->IsZombie())
        return;

    tree = dynamic_cast<TTree *>(file->Get("alcor"));
    if (!tree)
        return;

    data.link_to_tree(tree);
    n_entries = tree->GetEntries();

    //  Recover the RDO id from the path (e.g. ".../rdo-193/decoded/alcdaq.fifo_07.root").
    //  Upstream writes 0 into the alcor tree's device branch, so we override it per-Hit
    //  in read_next() using this value.
    auto rdo_pos = filename.find("rdo-");
    if (rdo_pos != std::string::npos)
    {
        try
        {
            device_id = std::stoi(filename.substr(rdo_pos + 4));
        }
        catch (...)
        {
            device_id = -1;
        }
    }
    //  Recover the FIFO id from the filename (e.g. ".../alcdaq.fifo_07.root").
    //  Each input file contains data from a single FIFO, so we override the fifo
    //  branch per-Hit in read_next() whenever the in-data fifo disagrees with
    //  the filename's value.
    auto fifo_pos = filename.find("fifo_");
    if (fifo_pos != std::string::npos)
    {
        try
        {
            fifo_id = std::stoi(filename.substr(fifo_pos + 5));
        }
        catch (...)
        {
            fifo_id = -1;
        }
    }
    if (n_entries > 0)
    {
        tree->GetEntry(0);
        if (device_id > 0 && data.get_data().device <= 0)
            mist::logger::warning(TString::Format(
                                      "(AlcorDataStreamer) Overriding bogus device branch with %d (parsed from %s)",
                                      device_id, filename.c_str())
                                      .Data());
        if (fifo_id >= 0 && data.get_data().fifo != fifo_id)
            mist::logger::warning(TString::Format(
                                      "(AlcorDataStreamer) Overriding bogus fifo branch with %d (parsed from %s)",
                                      fifo_id, filename.c_str())
                                      .Data());
    }
    //  Read gRollover — one point per spill, y = rollover count during that spill.
    if (auto *rollover_graph = dynamic_cast<TGraph *>(file->Get("gRollover")))
    {
        const int n_rollover_points = rollover_graph->GetN();
        rollover_count_per_spill.reserve(n_rollover_points);
        for (int i_spill = 0; i_spill < n_rollover_points; ++i_spill)
            rollover_count_per_spill.push_back(rollover_graph->GetPointY(i_spill));
    }

    valid = true;
}

AlcorDataStreamer &AlcorDataStreamer::operator=(AlcorDataStreamer &&other) noexcept
{
    if (this == &other)
        return *this;

    // Reset branch addresses on *this* tree before the old file is closed.
    // TFilePtr's move-assignment closes + deletes the current file implicitly.
    if (tree)
        tree->ResetBranchAddresses();

    filename = std::move(other.filename);
    file = std::move(other.file); // closes old file, takes ownership of other's
    tree = other.tree;
    data = std::move(other.data);
    n_entries = other.n_entries;
    cursor = other.cursor;
    valid = other.valid;
    device_id = other.device_id;
    fifo_id = other.fifo_id;
    rollover_count_per_spill = std::move(other.rollover_count_per_spill);

    other.tree = nullptr;
    other.valid = false;

    // Same rationale as the move-ctor: re-link branches to *this's `data`
    // fields, otherwise tree->GetEntry() writes to the source's freed
    // memory.  SetBranchAddress is idempotent.
    if (tree)
        data.link_to_tree(tree);

    return *this;
}

AlcorDataStreamer::~AlcorDataStreamer() noexcept
{
    // Reset branch addresses before TFilePtr closes the file —
    // members destruct in reverse declaration order, so this body
    // runs before file's destructor (which calls Close() + delete).
    if (tree)
        tree->ResetBranchAddresses();
}