#include "alcor_recotrackdata.h"
#include <iostream>

// --- construction --------------------------------------------------------

AlcorRecotrackdata::AlcorRecotrackdata(AlcorRecodata &v)
{
    set_recodata_ptr(v.get_recodata_ptr());
    set_triggers_ptr(v.get_triggers_ptr());
}

// --- element access ------------------------------------------------------

AlcorRecotrackdataStruct &AlcorRecotrackdata::recotrackdata_at(std::size_t idx)
{
    if (recotrackdata.size() <= idx)
        recotrackdata.resize(idx + 1);
    return recotrackdata.at(idx);
}

// --- I/O -----------------------------------------------------------------

void AlcorRecotrackdata::clear()
{
    AlcorRecodata::clear();
    recotrackdata.clear();
    recotrackdata.shrink_to_fit();
}

void AlcorRecotrackdata::link_to_tree(TTree *input_tree)
{
    if (!input_tree)
    {
        std::cerr << "[ERROR] link_to_tree: input_tree is null.\n";
        return;
    }
    AlcorRecodata::link_to_tree(input_tree);
    input_tree->SetBranchAddress("recotrackdata", &recotrackdata_ptr);
}

void AlcorRecotrackdata::write_to_tree(TTree *output_tree)
{
    if (!output_tree)
    {
        std::cerr << "[ERROR] write_to_tree: output_tree is null.\n";
        return;
    }
    output_tree->Branch("recotrackdata", &recotrackdata);
    output_tree->Branch("recodata", get_recodata_ptr());
    output_tree->Branch("triggers", get_triggers_ptr());
}

// --- import from tracking ------------------------------------------------

void AlcorRecotrackdata::import_event(std::vector<TrackingAltaiStruct> vec)
{
    int i_trk = -1;
    for (auto &v : vec)
    {
        i_trk++;
        set_det_plane_x(i_trk, v.zero_plane_x);
        set_det_plane_y(i_trk, v.zero_plane_y);
        set_traj_angcoeff_x(i_trk, v.angcoeff_dx);
        set_traj_angcoeff_y(i_trk, v.angcoeff_dy);
        set_chi2ndof(i_trk, v.chi2ndof);
    }
}