#include "alcor_recotrackdata.h"
#include <iostream>

using namespace std;

alcor_recotrackdata::alcor_recotrackdata(alcor_recodata &v)
{
    //  Link recotrackdata to recodata
    set_recodata_ptr(v.get_recodata_ptr());
    set_triggers_ptr(v.get_triggers_ptr());
}

void alcor_recotrackdata::add_recotrackdata(const alcor_recotrackdata_struct &entry) { recotrackdata.push_back(entry); }
alcor_recotrackdata_struct &alcor_recotrackdata::recotrackdata_at(std::size_t idx)
{
    if (recotrackdata.size() <= idx)
        recotrackdata.resize(idx + 1); // default-construct new elements
    return recotrackdata.at(idx);
}
const alcor_recotrackdata_struct &alcor_recotrackdata::recotrackdata_at(std::size_t idx) const { return recotrackdata.at(idx); }
void alcor_recotrackdata::set_det_plane_x(std::size_t idx, float val) { recotrackdata_at(idx).det_plane_x = val; }
void alcor_recotrackdata::set_det_plane_y(std::size_t idx, float val) { recotrackdata_at(idx).det_plane_y = val; }
void alcor_recotrackdata::set_traj_angcoeff_x(std::size_t idx, float val) { recotrackdata_at(idx).traj_angcoeff_x = val; }
void alcor_recotrackdata::set_traj_angcoeff_y(std::size_t idx, float val) { recotrackdata_at(idx).traj_angcoeff_y = val; }
void alcor_recotrackdata::set_chi2ndof(std::size_t idx, float val) { recotrackdata_at(idx).chi2ndof = val; }
void alcor_recotrackdata::clear()
{
    recotrackdata.clear();
    recotrackdata.shrink_to_fit();
}

void alcor_recotrackdata::link_to_tree(TTree *input_tree)
{
    if (!input_tree)
    {
        cerr << "[ERROR] link_to_tree: input_tree is null." << endl;
        return;
    }
    input_tree->SetBranchAddress("recotrackdata", &recotrackdata_ptr);
    input_tree->SetBranchAddress("recodata", get_recodata_ptr());
    input_tree->SetBranchAddress("triggers", get_triggers_ptr());
}
void alcor_recotrackdata::write_to_tree(TTree *output_tree)
{
    if (!output_tree)
    {
        cerr << "[ERROR] write_to_tree: output_tree is null." << endl;
        return;
    }
    output_tree->Branch("recotrackdata", &recotrackdata);
    output_tree->Branch("recodata", get_recodata_ptr());
    output_tree->Branch("triggers", get_triggers_ptr());
}
void alcor_recotrackdata::import_event(std::vector<tracking_altai_struct> vec)
{
    auto i_trk = -1;
    for (auto &v : vec)
    {
        i_trk++;
        if (i_trk > 0)
        {
            logger::log_debug("TRIGGERED");
            logger::log_debug(Form("vec size: %d", vec.size()));
        }
        set_det_plane_x(i_trk, v.zero_plane_x); // TODO: protect access
        set_det_plane_y(i_trk, v.zero_plane_y);
        set_traj_angcoeff_x(i_trk, v.angcoeff_dx);
        set_traj_angcoeff_y(i_trk, v.angcoeff_dy);
        set_chi2ndof(i_trk, v.chi2ndof);
    }
}

// --- Inline functions from header ---
inline double calculate_angle(double detector_to_telescope_plane, double pixel_position) { return atan(pixel_position / detector_to_telescope_plane); }

inline double calculate_angle_resolution(double detector_to_telescope_plane, double detector_to_telescope_plane_error,
                                         double pixel_position, double pixel_position_error)
{
    // TODO: implement proper error propagation
    return -1.;
}
