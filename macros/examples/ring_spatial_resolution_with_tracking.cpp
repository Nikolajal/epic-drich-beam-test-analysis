#include "../lib_loader.h"
#include "TCutG.h"

int n_rejected = 0;
/**
 * @file ring_spatial_resolution.cpp
 * @brief Calculate the spatial resolution of the ring.
 *
 * This exercise estimates the center and radius of a ring of hits and then
 * computes the spatial resolution using multiple methods.
 *
 * Additonally, this version of the macro exploits the tracking capabilities of the recotrackdata, to check the correlation between tracking angle and ring reconstruction quality.
 *
 * @details
 * **Workflow:**
 * 1. **Initial ring center and radius estimation**
 *    - Select points tagged as "ring-like" by a density scan algorithm
 *      (based on time coincidence and radial proximity).
 *    - Filter hits that are **not labeled as After-Pulses (APs)** and within a
 *      reasonable time window relative to the trigger.
 *    - Fit the selected points with a circle to determine the center `(x_0, y_0)`
 *      and radius `R`.
 *    - Fixing the center will aid the subsequent resolution calculation.
 *
 * 2. **Spatial resolution calculation** (three methods):
 *    - **Method 1: Variable resolution vs. participant hits**
 *      - Fit points assigned to the ring.
 *      - Plot resulting radius as a function of the number of participant hits (photo-electrons, p.e.).
 *
 *    - **Method 2: Single Photon Spatial Resolution (SPSR)**
 *      - Fit points assigned to the ring.
 *      - Remove one point at a time.
 *      - Compute the difference in radius between the fit result and the removed point.
 *
 *    - **Method 3: SPSR alternative**
 *      - Compute the difference in radius between the first-round radius result
 *        and each selected point.
 *
 * **Notes:**
 * - Method 1 provides variable resolution as a function of participants (p.e.).
 * - Methods 2 and 3 provide the spatial resolution for a single photon.
 *
 * @author Nicola Rubini
 */

//  --- --- --- !!!
//  This excercise is still a work in progress, stay tuned for updates!
//  --- --- --- !!!

std::array<float, 2> time_cut_boundaries = {-15., 15.};

// ---------------------------------------------------------------------------
//  Z positions of the two detector planes
// ---------------------------------------------------------------------------
constexpr float z_drich = 4325; // dRICH detector plane (mm)
constexpr float z_scint = 1150; // Scintillator plane (mm)

// ---------------------------------------------------------------------------
//  Track selection:
//    CutPlane -> on which plane(s) the TCutG is evaluated
//    CutSide  -> keep tracks INSIDE or OUTSIDE the polygon
//
//  Once a track survives the selection, its intercept is propagated to
//  BOTH planes and both maps are filled — regardless of where the cut was.
// ---------------------------------------------------------------------------
enum class CutPlane
{
    NONE,
    DRICH,
    SCINT,
    BOTH
};
enum class CutSide
{
    INSIDE,
    OUTSIDE
};

// ---------------------------------------------------------------------------
//  Single TCutG — same polygon, applied at the chosen plane(s)
// ---------------------------------------------------------------------------
TCutG *make_cutg_scint_stretto()
{
    TCutG *cutg = new TCutG("mycut", 8);
    cutg->SetPoint(0, -8.0798, -1.49754);
    cutg->SetPoint(1, -5.97317, 2.84483);
    cutg->SetPoint(2, -2.97012, 2.81117);
    cutg->SetPoint(3, 0.660441, 0.95977);
    cutg->SetPoint(4, -3.05976, -3.07964);
    cutg->SetPoint(5, -8.16944, -1.43021);
    cutg->SetPoint(6, -8.16944, -1.43021);
    cutg->SetPoint(7, -8.0798, -1.49754);
    cutg->SetVarX("x");
    cutg->SetVarY("y");
    cutg->SetTitle("Scint stretto");
    return cutg;
}

TCutG *make_cutg_scint_largo()
{
    TCutG *cutg = new TCutG("mycut", 10);
    cutg->SetPoint(0, -8.34873, -1.49754);
    cutg->SetPoint(1, -4.9871, 5.50411);
    cutg->SetPoint(2, 0.525976, 5.53777);
    cutg->SetPoint(3, 11.7314, 0.95977);
    cutg->SetPoint(4, 11.5073, -6.1092);
    cutg->SetPoint(5, 0.436333, -6.04187);
    cutg->SetPoint(6, -8.39355, -1.86782);
    cutg->SetPoint(7, -8.39355, -1.49754);
    cutg->SetPoint(8, -8.39355, -1.49754);
    cutg->SetPoint(9, -8.34873, -1.49754);
    cutg->SetVarX("x");
    cutg->SetVarY("y");
    cutg->SetTitle("Scint largo");
    return cutg;
}

TCutG *make_cutg_drich_stretto()
{
    TCutG *cutg = new TCutG("mycut", 7);
    cutg->SetPoint(0, -4.92388, -0.289291);
    cutg->SetPoint(1, -3.30728, 3.0507);
    cutg->SetPoint(2, -0.67378, 0.447086);
    cutg->SetPoint(3, -2.21216, -2.68252);
    cutg->SetPoint(4, -4.92388, -0.31559);
    cutg->SetPoint(5, -4.94996, -0.31559);
    cutg->SetPoint(6, -4.92388, -0.289291);
    cutg->SetVarX("x");
    cutg->SetVarY("y");
    cutg->SetTitle("dRICH stretto");
    return cutg;
}

TCutG *make_cutg_drich_largo()
{
    TCutG *cutg = new TCutG("mycut", 9);
    cutg->SetPoint(0, -5.65396, -0.525984);
    cutg->SetPoint(1, -2.78579, 5.25984);
    cutg->SetPoint(2, 3.78492, 0.657479);
    cutg->SetPoint(3, 3.81099, -4.15527);
    cutg->SetPoint(4, 2.53335, -5.31243);
    cutg->SetPoint(5, -1.03882, -5.31243);
    cutg->SetPoint(6, -5.68004, -0.473385);
    cutg->SetPoint(7, -5.68004, -0.473385);
    cutg->SetPoint(8, -5.65396, -0.525984);
    cutg->SetVarX("x");
    cutg->SetVarY("y");
    cutg->SetTitle("dRICH largo");
    return cutg;
}

TCutG *make_cutg_rect(float xmin, float xmax, float ymin, float ymax)
{
    TCutG *cutg = new TCutG("mycut", 5);
    cutg->SetPoint(0, xmin, ymin);
    cutg->SetPoint(1, xmax, ymin);
    cutg->SetPoint(2, xmax, ymax);
    cutg->SetPoint(3, xmin, ymax);
    cutg->SetPoint(4, xmin, ymin);
    cutg->SetVarX("x");
    cutg->SetVarY("y");
    cutg->SetTitle(Form("rect_%.0f_%.0f_%.0f_%.0f", xmin, xmax, ymin, ymax));
    return cutg;
}

/**
 * @brief Returns true if the track passes the selection.
 *
 * CutPlane::NONE  -> always pass (side is ignored)
 * CutPlane::DRICH -> evaluate cut at z_drich only
 * CutPlane::SCINT -> evaluate cut at z_scint only
 * CutPlane::BOTH  -> evaluate cut at both planes (AND)
 * CutSide::INSIDE  -> keep tracks whose intercept is inside the polygon
 * CutSide::OUTSIDE -> keep tracks whose intercept is outside the polygon
 */
bool is_track_selected(float plane_x, float plane_y, float slope_x, float slope_y,
                       TCutG *cutg, CutPlane plane, CutSide side)
{
    if (plane == CutPlane::NONE)
        return true;

    float ix_drich = plane_x - slope_x * z_drich;
    float iy_drich = plane_y - slope_y * z_drich;
    float ix_scint = plane_x - slope_x * z_scint;
    float iy_scint = plane_y - slope_y * z_scint;

    auto pass = [&](bool is_inside)
    {
        return (side == CutSide::INSIDE) ? is_inside : !is_inside;
    };

    switch (plane)
    {
    case CutPlane::DRICH:
        return pass(cutg->IsInside(ix_drich, iy_drich));
    case CutPlane::SCINT:
        return pass(cutg->IsInside(ix_scint, iy_scint));
    case CutPlane::BOTH:
        return pass(cutg->IsInside(ix_drich, iy_drich)) && pass(cutg->IsInside(ix_scint, iy_scint));
    default:
        return true;
    }
}

std::string cut_plane_to_string(CutPlane p)
{
    switch (p)
    {
    case CutPlane::NONE:  return "NONE";
    case CutPlane::DRICH: return "DRICH";
    case CutPlane::SCINT: return "SCINT";
    case CutPlane::BOTH:  return "BOTH";
    default:              return "UNKNOWN";
    }
}

std::string cut_side_to_string(CutSide s)
{
    switch (s)
    {
    case CutSide::INSIDE:  return "INSIDE";
    case CutSide::OUTSIDE: return "OUTSIDE";
    default:               return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------

void ring_spatial_resolution_with_tracking(std::string data_repository, std::string run_name, int max_frames = 100000)
{
    //  Input files
    std::string input_filename_recotrackdata = data_repository + "/" + run_name + "/recotrackdata.root";

    TFile *input_file_recotrackdata = new TFile(input_filename_recotrackdata.c_str());
    if (!input_file_recotrackdata || input_file_recotrackdata->IsZombie())
    {
        std::cerr << "[WARNING] Could not find recotrackdata, making it" << std::endl;
        return;
    }

    TTree *recotrackdata_tree = (TTree *)input_file_recotrackdata->Get("recotrackdata");
    alcor_recotrackdata *recotrackdata = new alcor_recotrackdata();
    recotrackdata->link_to_tree(recotrackdata_tree);

    auto n_frames  = recotrackdata_tree->GetEntries();
    auto all_frames = min((int)n_frames, (int)max_frames);

    // -------------------------------------------------------------------------
    //  >>> SELECTION SETTINGS — change these lines <<<
    // -------------------------------------------------------------------------
    CutPlane cut_plane = CutPlane::NONE; // NONE / DRICH / SCINT / BOTH
    CutSide cut_side   = CutSide::INSIDE;

    bool apply_multiplicity_cut = false;
    bool apply_radial_cut       = false;
    bool require_single_track   = false;
    bool require_multi_track    = true;

    bool apply_theta_phi_cut = false;
    bool apply_slope_xy_cut  = false;

    float theta_min   = 0.0;
    float theta_max   = 1.0;
    float phi_min     = -2.2;
    float phi_max     = -1.5;
    float slope_x_min = 0.02;
    float slope_x_max = 1.0;
    float slope_y_min = 0.02;
    float slope_y_max = 1.0;

    TCutG *cutg = make_cutg_rect(-100, 100, -100, 100);
    // -------------------------------------------------------------------------

    // =========================================================================
    //  HISTOGRAMS
    // =========================================================================

    // --- Time distribution ---
    TH1F *h_t_distribution = new TH1F("h_t_distribution",
        "Hit time wrt trigger;t_{hit} - t_{timing} (ns);counts",
        200, -312.5, 312.5);

    // --- First round ring fit ---
    TH1F *h_first_round_X = new TH1F("h_first_round_X",
        "Ring center X (1st round);x_{0} (mm);counts", 120, -30, 30);
    TH1F *h_first_round_Y = new TH1F("h_first_round_Y",
        "Ring center Y (1st round);y_{0} (mm);counts", 120, -30, 30);
    TH1F *h_first_round_R = new TH1F("h_first_round_R",
        "Ring radius (1st round);R (mm);counts", 200, 30, 130);

    // --- Tracking QA ---
    TH1F *h_tracking_theta = new TH1F("h_tracking_theta",
        "Track polar angle;#theta (rad);counts", 1000, 0, 0.1);
    TH1F *h_tracking_phi = new TH1F("h_tracking_phi",
        "Track azimuthal angle;#phi (rad);counts", 1000, -3.1415, +3.1415);

    // --- Intercept maps ---
    TH2F *h_intercept_drich = new TH2F("h_intercept_drich",
        "Track intercept at dRICH plane (z=4325 mm);x (mm);y (mm)",
        500, -50., +50., 200, -50., +50.);
    TH2F *h_intercept_scint = new TH2F("h_intercept_scint",
        "Track intercept at scintillator plane (z=1150 mm);x (mm);y (mm)",
        500, -50., +50., 200, -50., +50.);

    // =========================================================================
    //  FIRST LOOP
    // =========================================================================
    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        recotrackdata_tree->GetEntry(i_frame);

        std::vector<std::array<float, 2>> selected_points;
        float avg_radius = 0.;

        if (i_frame % 10000 == 0)
            bar.update(i_frame, all_frames);

        if (recotrackdata->is_start_of_spill())
            continue;

        for (auto i = 0; i < recotrackdata->n_recotrackdata(); i++)
        {
            h_tracking_theta->Fill(recotrackdata->get_traj_angcoeff_theta(i));
            h_tracking_phi->Fill(recotrackdata->get_traj_angcoeff_phi(i));
        }

        auto streaming_trigger = recotrackdata->get_trigger_by_index(104);
        if (streaming_trigger)
        {
            for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
            {
                if (recotrackdata->is_afterpulse(current_hit))
                    continue;

                auto time_delta_wrt_ref = recotrackdata->get_hit_t(current_hit) - streaming_trigger->fine_time;
                h_t_distribution->Fill(time_delta_wrt_ref);

                if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
                    continue;

                selected_points.push_back({recotrackdata->get_hit_x(current_hit), recotrackdata->get_hit_y(current_hit)});
                avg_radius += recotrackdata->get_hit_r(current_hit);
            }

            if (selected_points.size() > 4)
            {
                auto fit_result = fit_circle(selected_points, {0., 0., avg_radius / selected_points.size()}, false, {{}});
                h_first_round_X->Fill(fit_result[0][0]);
                h_first_round_Y->Fill(fit_result[1][0]);
                h_first_round_R->Fill(fit_result[2][0]);
            }
        }
    }
    bar.update(all_frames, all_frames);
    bar.finish();

    float found_ring_center_x      = h_first_round_X->GetMean();
    float found_ring_center_y      = h_first_round_Y->GetMean();
    float found_ring_radius        = h_first_round_R->GetMean();
    h_first_round_R->Fit("gaus", "Q");
    auto gaus_f = h_first_round_R->GetFunction("gaus");
    float found_ring_radius_stddev = gaus_f->GetParameter(2);

    // =========================================================================
    //  SECOND ROUND HISTOGRAMS
    // =========================================================================

    // --- Variable binning for intercept axis ---
    std::vector<double> ix_drich_range = {-60, -40, -20, -10, -6, -2, 0, 2, 6, 10, 20, 40, 60};

    // --- Hit maps ---
    TH2F *h_second_round_xy_map = new TH2F("h_second_round_xy_map",
        "Selected hits on detector plane;x (mm);y (mm)",
        396, -99, 99, 396, -99, 99);
    TH2F *h_second_round_xy_map_rejected = new TH2F("h_second_round_xy_map_rejected",
        "Rejected hits on detector plane;x (mm);y (mm)",
        396, -99, 99, 396, -99, 99);

    // --- Basic ring residual and hit yield ---
    TH1F *h_second_round_R = new TH1F("h_second_round_R",
        "Ring radius residual (2nd round);#DeltaR (mm);counts", 200, -20, 20);
    TH1F *h_n_selected_hits = new TH1F("h_n_selected_hits",
        "Selected hits per event;n hits;counts", 100, 0, 100);

    // --- Multiplicity ---
    TH2F *h_n_selected_hits_vs_multiplicity = new TH2F("h_n_selected_hits_vs_multiplicity",
        "Selected hits vs track multiplicity;n tracks;n hits",
        10, -0.5, 9.5, 100, -0.5, 99.5);

    // --- Photon yield vs tracking variables ---
    TH2F *h_n_selected_hits_vs_theta = new TH2F("h_n_selected_hits_vs_theta",
        "Selected hits vs track polar angle;#theta (rad);n hits",
        10, 0, 0.1, 100, -0.5, 99.5);
    TH2F *h_n_selected_hits_vs_ix_drich = new TH2F("h_n_selected_hits_vs_ix_drich",
        "Selected hits vs track intercept X at dRICH;x_{dRICH} (mm);n hits",
        ix_drich_range.size() - 1, ix_drich_range.data(), 100, -0.5, 99.5);
    TH2F *h_n_selected_hits_vs_iy_drich = new TH2F("h_n_selected_hits_vs_iy_drich",
        "Selected hits vs track intercept Y at dRICH;y_{dRICH} (mm);n hits",
        ix_drich_range.size() - 1, ix_drich_range.data(), 100, -0.5, 99.5);

    // --- DeltaR vs tracking variables (for sigma plots) ---
    TH2F *h_deltaR_vs_theta = new TH2F("h_deltaR_vs_theta",
        "#DeltaR vs track #theta;#theta (rad);#DeltaR (mm)",
        50, 0, 0.1, 200, -20, 20);
    TH2F *h_deltaR_vs_phi = new TH2F("h_deltaR_vs_phi",
        "#DeltaR vs track #phi;#phi (rad);#DeltaR (mm)",
        50, -3.1415, 3.1415, 200, -20, 20);
    TH2F *h_deltaR_vs_ix_drich = new TH2F("h_deltaR_vs_ix_drich",
        "#DeltaR vs track intercept X at dRICH;x_{dRICH} (mm);#DeltaR (mm)",
        ix_drich_range.size() - 1, ix_drich_range.data(), 200, -20., 20.);

    // =========================================================================
    //  RING FIT PARAMS vs ix_drich  (x0, y0, R)
    // =========================================================================
    TH2F *h_ring_x0_vs_ix_drich = new TH2F("h_ring_x0_vs_ix_drich",
        "Ring center X vs intercept X at dRICH;x_{dRICH} (mm);x_{0} (mm)",
        ix_drich_range.size() - 1, ix_drich_range.data(), 120, -30, 30);
    TH2F *h_ring_y0_vs_ix_drich = new TH2F("h_ring_y0_vs_ix_drich",
        "Ring center Y vs intercept X at dRICH;x_{dRICH} (mm);y_{0} (mm)",
        ix_drich_range.size() - 1, ix_drich_range.data(), 120, -30, 30);
    TH2F *h_ring_R_vs_ix_drich = new TH2F("h_ring_R_vs_ix_drich",
        "Ring radius vs intercept X at dRICH;x_{dRICH} (mm);R (mm)",
        ix_drich_range.size() - 1, ix_drich_range.data(), 200, 30, 130);

    // =========================================================================
    //  RING FIT PARAMS vs iy_drich  (x0, y0, R)
    // =========================================================================
    TH2F *h_ring_x0_vs_iy_drich = new TH2F("h_ring_x0_vs_iy_drich",
        "Ring center X vs intercept Y at dRICH;y_{dRICH} (mm);x_{0} (mm)",
        ix_drich_range.size() - 1, ix_drich_range.data(), 120, -30, 30);
    TH2F *h_ring_y0_vs_iy_drich = new TH2F("h_ring_y0_vs_iy_drich",
        "Ring center Y vs intercept Y at dRICH;y_{dRICH} (mm);y_{0} (mm)",
        ix_drich_range.size() - 1, ix_drich_range.data(), 120, -30, 30);
    TH2F *h_ring_R_vs_iy_drich = new TH2F("h_ring_R_vs_iy_drich",
        "Ring radius vs intercept Y at dRICH;y_{dRICH} (mm);R (mm)",
        ix_drich_range.size() - 1, ix_drich_range.data(), 200, 30, 130);

    // =========================================================================
    //  RING FIT PARAMS vs theta  (x0, y0, R)
    // =========================================================================
    TH2F *h_ring_x0_vs_theta = new TH2F("h_ring_x0_vs_theta",
        "Ring center X vs track #theta;#theta (rad);x_{0} (mm)",
        50, 0, 0.1, 120, -30, 30);
    TH2F *h_ring_y0_vs_theta = new TH2F("h_ring_y0_vs_theta",
        "Ring center Y vs track #theta;#theta (rad);y_{0} (mm)",
        50, 0, 0.1, 120, -30, 30);
    TH2F *h_ring_R_vs_theta = new TH2F("h_ring_R_vs_theta",
        "Ring radius vs track #theta;#theta (rad);R (mm)",
        50, 0, 0.1, 200, 30, 130);

    // =========================================================================
    //  RING FIT PARAMS vs phi  (x0, y0, R)
    // =========================================================================
    TH2F *h_ring_x0_vs_phi = new TH2F("h_ring_x0_vs_phi",
        "Ring center X vs track #phi;#phi (rad);x_{0} (mm)",
        50, -3.1415, 3.1415, 120, -30, 30);
    TH2F *h_ring_y0_vs_phi = new TH2F("h_ring_y0_vs_phi",
        "Ring center Y vs track #phi;#phi (rad);y_{0} (mm)",
        50, -3.1415, 3.1415, 120, -30, 30);
    TH2F *h_ring_R_vs_phi = new TH2F("h_ring_R_vs_phi",
        "Ring radius vs track #phi;#phi (rad);R (mm)",
        50, -3.1415, 3.1415, 200, 30, 130);

    // =========================================================================
    //  SECOND LOOP
    // =========================================================================
    mist::logger::progress_bar bar_second(mist::logger::bar_style::BLOCK);
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        recotrackdata_tree->GetEntry(i_frame);

        std::vector<std::array<float, 2>> selected_points;

        if (i_frame % 1000 == 0)
            bar_second.update(i_frame, all_frames);

        if (recotrackdata->is_start_of_spill())
            continue;

        auto default_hardware_trigger = recotrackdata->get_trigger_by_index(0);
        auto streaming_trigger        = recotrackdata->get_trigger_by_index(104);
        if (streaming_trigger && default_hardware_trigger)
        {
            if (require_single_track && recotrackdata->n_recotrackdata() != 1) continue;
            if (!require_single_track && recotrackdata->n_recotrackdata() < 1) continue;
            if (require_multi_track && recotrackdata->n_recotrackdata() > 2) continue;

            float plane_x = recotrackdata->get_det_plane_x(0);
            float plane_y = recotrackdata->get_det_plane_y(0);
            float slope_x = recotrackdata->get_traj_angcoeff_x(0);
            float slope_y = recotrackdata->get_traj_angcoeff_y(0);

            float ix_drich = plane_x - slope_x * z_drich;
            float iy_drich = plane_y - slope_y * z_drich;

            if (!is_track_selected(plane_x, plane_y, slope_x, slope_y, cutg, cut_plane, cut_side))
            {
                n_rejected++;
                continue;
            }

            float track_theta = recotrackdata->get_traj_angcoeff_theta(0);
            float track_phi   = recotrackdata->get_traj_angcoeff_phi(0);

            if (apply_theta_phi_cut)
            {
                if (track_theta < theta_min || track_theta > theta_max) continue;
                if (track_phi   < phi_min   || track_phi   > phi_max)   continue;
            }

            if (apply_slope_xy_cut)
            {
                if (fabs(slope_x) < slope_x_min || fabs(slope_x) > slope_x_max) continue;
                if (fabs(slope_y) < slope_y_min || fabs(slope_y) > slope_y_max) continue;
            }

            // Intercept maps
            h_intercept_drich->Fill(ix_drich, iy_drich);
            h_intercept_scint->Fill(plane_x - slope_x * z_scint, plane_y - slope_y * z_scint);

            // Hit selection
            for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
            {
                if (recotrackdata->is_afterpulse(current_hit)) continue;

                auto time_delta_wrt_ref = recotrackdata->get_hit_t(current_hit) - streaming_trigger->fine_time;
                if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
                    continue;

                if (apply_radial_cut &&
                    fabs(recotrackdata->get_hit_r(current_hit, {found_ring_center_x, found_ring_center_y}) - found_ring_radius) > 5 * found_ring_radius_stddev)
                    continue;

                selected_points.push_back({recotrackdata->get_hit_x(current_hit), recotrackdata->get_hit_y(current_hit)});
                h_second_round_xy_map->Fill(recotrackdata->get_hit_x_rnd(current_hit), recotrackdata->get_hit_y_rnd(current_hit));
            }

            // Hit yield
            h_n_selected_hits->Fill(selected_points.size());
            h_n_selected_hits_vs_multiplicity->Fill(recotrackdata->n_recotrackdata(), selected_points.size());
            h_n_selected_hits_vs_theta->Fill(track_theta, selected_points.size());
            h_n_selected_hits_vs_ix_drich->Fill(ix_drich, selected_points.size());
            h_n_selected_hits_vs_iy_drich->Fill(iy_drich, selected_points.size());

            // Ring fit
            if (!apply_multiplicity_cut || fabs((int)selected_points.size() - 15) < 2)
            {
                auto fit_result = fit_circle(selected_points, {found_ring_center_x, found_ring_center_y, found_ring_radius}, true, {{}});
                float deltaR = fit_result[2][0] - found_ring_radius;

                h_second_round_R->Fill(deltaR);

                // DeltaR vs tracking variables
                h_deltaR_vs_theta->Fill(track_theta, deltaR);
                h_deltaR_vs_phi->Fill(track_phi, deltaR);
                h_deltaR_vs_ix_drich->Fill(ix_drich, deltaR);

                // Ring params vs ix_drich
                h_ring_x0_vs_ix_drich->Fill(ix_drich, fit_result[0][0]);
                h_ring_y0_vs_ix_drich->Fill(ix_drich, fit_result[1][0]);
                h_ring_R_vs_ix_drich->Fill(ix_drich, fit_result[2][0]);

                // Ring params vs iy_drich
                h_ring_x0_vs_iy_drich->Fill(iy_drich, fit_result[0][0]);
                h_ring_y0_vs_iy_drich->Fill(iy_drich, fit_result[1][0]);
                h_ring_R_vs_iy_drich->Fill(iy_drich, fit_result[2][0]);

                // Ring params vs theta
                h_ring_x0_vs_theta->Fill(track_theta, fit_result[0][0]);
                h_ring_y0_vs_theta->Fill(track_theta, fit_result[1][0]);
                h_ring_R_vs_theta->Fill(track_theta, fit_result[2][0]);

                // Ring params vs phi
                h_ring_x0_vs_phi->Fill(track_phi, fit_result[0][0]);
                h_ring_y0_vs_phi->Fill(track_phi, fit_result[1][0]);
                h_ring_R_vs_phi->Fill(track_phi, fit_result[2][0]);
            }
        }
    }
    bar_second.update(all_frames, all_frames);
    bar_second.finish();

    // =========================================================================
    //  DRAWING
    // =========================================================================
    gROOT->SetBatch(true);

    // --- Lambda: sigma graph (gaus slice fit, returns sigma per bin) ---
    auto make_sigma_graph = [&](TH2F *h2, const char *title) -> TGraphErrors *
    {
        TGraphErrors *g = new TGraphErrors();
        g->SetTitle(title);
        for (int xb = 1; xb <= h2->GetNbinsX(); xb++)
        {
            auto sl = h2->ProjectionY(Form("sg_%s_%i", h2->GetName(), xb), xb, xb);
            if (sl->GetEntries() < 50) continue;
            sl->Fit("gaus", "Q");
            auto f = sl->GetFunction("gaus");
            int n = g->GetN();
            g->SetPoint(n, h2->GetXaxis()->GetBinCenter(xb), f->GetParameter(2));
            g->SetPointError(n, 0.5 * h2->GetXaxis()->GetBinWidth(xb), f->GetParError(2));
        }
        return g;
    };

    // --- Time distribution ---
    TCanvas *c_time_delta = new TCanvas("c_time_delta", "Hit time wrt trigger", 800, 800);
    gPad->SetLogy();
    h_t_distribution->SetLineColor(kBlack);
    h_t_distribution->SetLineWidth(2);
    h_t_distribution->Draw();

    // --- First round ring fit ---
    TCanvas *c_first_round = new TCanvas("c_first_round", "Ring fit results - 1st round", 1200, 400);
    c_first_round->Divide(3, 1);
    c_first_round->cd(1); h_first_round_X->Draw();
    c_first_round->cd(2); h_first_round_Y->Draw();
    c_first_round->cd(3); h_first_round_R->Draw();

    // --- Tracking angle distributions ---
    TCanvas *c_tracking_angles = new TCanvas("c_tracking_angles", "Tracking angle distributions", 1600, 800);
    c_tracking_angles->Divide(2, 1);
    c_tracking_angles->cd(1); h_tracking_theta->Draw();
    c_tracking_angles->cd(2); h_tracking_phi->Draw();

    // --- Intercept maps ---
    TCanvas *c_intercepts = new TCanvas("c_intercepts", "Track intercept maps (selected tracks)", 1600, 800);
    c_intercepts->Divide(2, 1);
    c_intercepts->cd(1); h_intercept_drich->Draw("COLZ");
    c_intercepts->cd(2); h_intercept_scint->Draw("COLZ");

    // --- Hit maps ---
    TCanvas *c_second_round_map = new TCanvas("c_second_round_map",
        "Selected hits on detector plane - 2nd round", 800, 800);
    h_second_round_xy_map->Draw("COLZ");

    TCanvas *c_second_round_map_rejected = new TCanvas("c_second_round_map_rejected",
        "Rejected hits on detector plane - 2nd round", 800, 800);
    h_second_round_xy_map_rejected->Draw("COLZ");

    // --- Basic ring residual ---
    TCanvas *c_second_round_R = new TCanvas("c_second_round_R",
        "Ring radius residual - 2nd round", 800, 600);
    h_second_round_R->Draw();

    // --- Hit yield distributions ---
    TCanvas *c_n_selected_hits = new TCanvas("c_n_selected_hits",
        "Selected hits per event", 800, 600);
    h_n_selected_hits->Draw();

    TCanvas *c_hits_vs_multiplicity = new TCanvas("c_hits_vs_multiplicity",
        "Selected hits vs track multiplicity", 800, 800);
    h_n_selected_hits_vs_multiplicity->Draw("SCAT");

    // --- Photon yield vs tracking variables (scatter + Poisson profile) ---
    TF1 *fitfunc = new TF1("fitfunc", "[1]*TMath::PoissonI(x,[0])", 0, 100);

    auto draw_photon_yield = [&](TCanvas *c, TH2F *h2, TGraphErrors *g)
    {
        c->cd();
        h2->Draw("SCAT");
        for (auto x_bin = 1; x_bin <= h2->GetNbinsX(); x_bin++)
        {
            auto current_point = g->GetN();
            auto sl = h2->ProjectionY(Form("py_%s_%i", h2->GetName(), x_bin), x_bin, x_bin);
            if (sl->GetEntries() < 50) continue;
            fitfunc->SetParameter(0, 16.); fitfunc->SetParameter(1, 1.);
            sl->Fit("fitfunc", "Q");
            auto f = sl->GetFunction("fitfunc");
            g->SetPoint(current_point, h2->GetXaxis()->GetBinCenter(x_bin), f->GetParameter(0));
            g->SetPointError(current_point, 0.5 * h2->GetXaxis()->GetBinWidth(x_bin), f->GetParError(0));
        }
        g->Draw("ALP SAME");
    };

    TCanvas *c_photons_theta = new TCanvas("c_photons_theta", "Photon yield vs #theta", 800, 800);
    TGraphErrors *g_photons_theta = new TGraphErrors();
    g_photons_theta->SetTitle("Mean photon yield vs #theta;#theta (rad);#mu_{p.e.}");
    draw_photon_yield(c_photons_theta, h_n_selected_hits_vs_theta, g_photons_theta);

    TCanvas *c_photons_ix = new TCanvas("c_photons_ix", "Photon yield vs x_{dRICH}", 800, 800);
    TGraphErrors *g_photons_ix = new TGraphErrors();
    g_photons_ix->SetTitle("Mean photon yield vs x_{dRICH};x_{dRICH} (mm);#mu_{p.e.}");
    draw_photon_yield(c_photons_ix, h_n_selected_hits_vs_ix_drich, g_photons_ix);

    TCanvas *c_photons_iy = new TCanvas("c_photons_iy", "Photon yield vs y_{dRICH}", 800, 800);
    TGraphErrors *g_photons_iy = new TGraphErrors();
    g_photons_iy->SetTitle("Mean photon yield vs y_{dRICH};y_{dRICH} (mm);#mu_{p.e.}");
    draw_photon_yield(c_photons_iy, h_n_selected_hits_vs_iy_drich, g_photons_iy);

    // --- Ring fit params vs ix_drich (x0, y0, R — scatter) ---
    TCanvas *c_ring_params_ix = new TCanvas("c_ring_params_ix",
        "Ring fit params vs x_{dRICH}", 1800, 600);
    c_ring_params_ix->Divide(3, 1);
    c_ring_params_ix->cd(1); h_ring_x0_vs_ix_drich->Draw("SCAT");
    c_ring_params_ix->cd(2); h_ring_y0_vs_ix_drich->Draw("SCAT");
    c_ring_params_ix->cd(3); h_ring_R_vs_ix_drich->Draw("SCAT");

    // --- Ring fit params vs iy_drich (x0, y0, R — scatter) ---
    TCanvas *c_ring_params_iy = new TCanvas("c_ring_params_iy",
        "Ring fit params vs y_{dRICH}", 1800, 600);
    c_ring_params_iy->Divide(3, 1);
    c_ring_params_iy->cd(1); h_ring_x0_vs_iy_drich->Draw("SCAT");
    c_ring_params_iy->cd(2); h_ring_y0_vs_iy_drich->Draw("SCAT");
    c_ring_params_iy->cd(3); h_ring_R_vs_iy_drich->Draw("SCAT");

    // --- Ring fit params vs theta (x0, y0, R — scatter) ---
    TCanvas *c_ring_params_theta = new TCanvas("c_ring_params_theta",
        "Ring fit params vs #theta", 1800, 600);
    c_ring_params_theta->Divide(3, 1);
    c_ring_params_theta->cd(1); h_ring_x0_vs_theta->Draw("SCAT");
    c_ring_params_theta->cd(2); h_ring_y0_vs_theta->Draw("SCAT");
    c_ring_params_theta->cd(3); h_ring_R_vs_theta->Draw("SCAT");

    // --- Ring fit params vs phi (x0, y0, R — scatter) ---
    TCanvas *c_ring_params_phi = new TCanvas("c_ring_params_phi",
        "Ring fit params vs #phi", 1800, 600);
    c_ring_params_phi->Divide(3, 1);
    c_ring_params_phi->cd(1); h_ring_x0_vs_phi->Draw("SCAT");
    c_ring_params_phi->cd(2); h_ring_y0_vs_phi->Draw("SCAT");
    c_ring_params_phi->cd(3); h_ring_R_vs_phi->Draw("SCAT");

    // --- Sigma (spatial resolution) vs theta ---
    TCanvas *c_sigma_theta = new TCanvas("c_sigma_theta", "Spatial resolution vs #theta", 800, 800);
    make_sigma_graph(h_deltaR_vs_theta,
        "Spatial resolution vs track #theta;#theta (rad);#sigma_{R} (mm)")->Draw("ALP");

    // --- Sigma (spatial resolution) vs phi ---
    TCanvas *c_sigma_phi = new TCanvas("c_sigma_phi", "Spatial resolution vs #phi", 800, 800);
    make_sigma_graph(h_deltaR_vs_phi,
        "Spatial resolution vs track #phi;#phi (rad);#sigma_{R} (mm)")->Draw("ALP");

    // --- Settings ---
    TCanvas *c_settings = new TCanvas("c_settings", "Analysis settings", 600, 600);
    TPaveText *pt = new TPaveText(0.05, 0.05, 0.95, 0.95, "NDC");
    pt->SetTextAlign(12);
    pt->SetTextFont(42);
    pt->SetFillColor(0);
    pt->SetBorderSize(1);
    pt->AddText(Form("cut_plane:             %s", cut_plane_to_string(cut_plane).c_str()));
    pt->AddText(Form("cut_side:              %s", cut_side_to_string(cut_side).c_str()));
    pt->AddText(Form("cutg:                  %s", cutg->GetTitle()));
    pt->AddText(Form("require_single_track:  %i", require_single_track));
    pt->AddText(Form("require_multi_track:   %i", require_multi_track));
    pt->AddText(Form("apply_multiplicity_cut:%i", apply_multiplicity_cut));
    pt->AddText(Form("apply_radial_cut:      %i", apply_radial_cut));
    pt->AddText(Form("apply_theta_phi_cut:   %i", apply_theta_phi_cut));
    pt->AddText(Form("apply_slope_xy_cut:    %i", apply_slope_xy_cut));
    pt->AddText(Form("theta_min:             %.4f", theta_min));
    pt->AddText(Form("theta_max:             %.4f", theta_max));
    pt->AddText(Form("slope_x_min:           %.4f", slope_x_min));
    pt->AddText(Form("slope_x_max:           %.4f", slope_x_max));
    pt->AddText(Form("slope_y_min:           %.4f", slope_y_min));
    pt->AddText(Form("slope_y_max:           %.4f", slope_y_max));
    pt->AddText(Form("run:                   %s", run_name.c_str()));
    pt->Draw();

    // =========================================================================
    //  SAVE
    // =========================================================================
    std::string output_root = data_repository + "/" + run_name + "/plots/histograms.root";
    std::string output_dir  = data_repository + "/" + run_name + "/plots";
    gSystem->mkdir(output_dir.c_str(), true);

    TFile *output_file = new TFile(output_root.c_str(), "RECREATE");

    h_t_distribution->Write();
    h_first_round_X->Write();
    h_first_round_Y->Write();
    h_first_round_R->Write();
    h_tracking_theta->Write();
    h_tracking_phi->Write();
    h_intercept_drich->Write();
    h_intercept_scint->Write();
    h_second_round_xy_map->Write();
    h_second_round_xy_map_rejected->Write();
    h_second_round_R->Write();
    h_n_selected_hits->Write();
    h_n_selected_hits_vs_multiplicity->Write();
    h_n_selected_hits_vs_theta->Write();
    h_n_selected_hits_vs_ix_drich->Write();
    h_n_selected_hits_vs_iy_drich->Write();
    h_deltaR_vs_theta->Write();
    h_deltaR_vs_phi->Write();
    h_deltaR_vs_ix_drich->Write();
    h_ring_x0_vs_ix_drich->Write();
    h_ring_y0_vs_ix_drich->Write();
    h_ring_R_vs_ix_drich->Write();
    h_ring_x0_vs_iy_drich->Write();
    h_ring_y0_vs_iy_drich->Write();
    h_ring_R_vs_iy_drich->Write();
    h_ring_x0_vs_theta->Write();
    h_ring_y0_vs_theta->Write();
    h_ring_R_vs_theta->Write();
    h_ring_x0_vs_phi->Write();
    h_ring_y0_vs_phi->Write();
    h_ring_R_vs_phi->Write();

    for (auto obj : *gROOT->GetListOfCanvases())
    {
        TCanvas *c = (TCanvas *)obj;
        c->SaveAs(Form("%s/%s.png", output_dir.c_str(), c->GetName()));
    }

    output_file->cd();
    c_settings->Write();
    output_file->Close();

    cout << "rejected tracks: " << n_rejected << endl;
}