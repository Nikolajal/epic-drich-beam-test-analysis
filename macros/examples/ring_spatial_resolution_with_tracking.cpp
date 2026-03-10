#include "../lib_loader.h"
#include "TCutG.h"

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
constexpr float z_drich = 432.5; // dRICH detector plane (mm)
constexpr float z_scint = 115.0; // Scintillator plane (mm)

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
TCutG *make_cutg()
{
    TCutG *cutg = new TCutG("mycut", 10);

    cutg->SetPoint(0, -9.42544, -1.48856);
    cutg->SetPoint(1, -6.13596, 4.94749);
    cutg->SetPoint(2, 0.16886, 5.26401);
    cutg->SetPoint(3, 13.6009, 1.5712);
    cutg->SetPoint(4, 13.6009, -6.34198);
    cutg->SetPoint(5, 1.8136, -6.34198);
    cutg->SetPoint(6, -9.5625, -1.9106);
    cutg->SetPoint(7, -10.3849, -1.48856);
    cutg->SetPoint(8, -9.42544, -1.69958);
    cutg->SetPoint(9, -9.42544, -1.48856);

    cutg->SetVarX("x");
    cutg->SetVarY("y");
    cutg->SetTitle("Track selection cut");

    return cutg;
}

/**
 * @brief Returns true if the track passes the selection.
 *
 * The cut is evaluated on the plane(s) specified by `plane`, keeping tracks
 * on the side specified by `side`. Tracks that survive are then propagated
 * to both detector planes for map filling — that happens outside this function.
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
// ---------------------------------------------------------------------------

void ring_spatial_resolution_with_tracking(std::string data_repository, std::string run_name, int max_frames = 100000)
{
    //  Input files
    std::string input_filename_recotrackdata = data_repository + "/" + run_name + "/recotrackdata.root";

    //  Load recotrackdata, return if not available
    TFile *input_file_recotrackdata = new TFile(input_filename_recotrackdata.c_str());
    if (!input_file_recotrackdata || input_file_recotrackdata->IsZombie())
    {
        std::cerr << "[WARNING] Could not find recotrackdata, making it" << std::endl;
        return;
    }

    //  Link recotrackdata tree locally
    TTree *recotrackdata_tree = (TTree *)input_file_recotrackdata->Get("recotrackdata");
    alcor_recotrackdata *recotrackdata = new alcor_recotrackdata();
    recotrackdata->link_to_tree(recotrackdata_tree);

    //  Get number of frames, limited to maximum requested frames
    auto n_frames = recotrackdata_tree->GetEntries();
    auto all_frames = min((int)n_frames, (int)max_frames);

    //  -------------------------------------------------------------------------
    //  >>> SELECTION SETTINGS — change these two lines <<<
    CutPlane cut_plane = CutPlane::NONE; // NONE / DRICH / SCINT / BOTH
    CutSide cut_side = CutSide::INSIDE;  // INSIDE / OUTSIDE
    //  -------------------------------------------------------------------------
    TCutG *cutg = make_cutg();

    //  Time distribution
    TH1F *h_t_distribution = new TH1F("h_t_distribution", "Hit time wrt trigger;t_{hit} - t_{timing} (ns);counts", 200, -312.5, 312.5);
    //  First round X, Y, R
    TH1F *h_first_round_X = new TH1F("h_first_round_X", "Ring center X (1st round);x_{0} (mm);counts", 120, -30, 30);
    TH1F *h_first_round_Y = new TH1F("h_first_round_Y", "Ring center Y (1st round);y_{0} (mm);counts", 120, -30, 30);
    TH1F *h_first_round_R = new TH1F("h_first_round_R", "Ring radius (1st round);R (mm);counts", 200, 30, 130);
    //  Tracking QA
    TH1F *h_tracking_theta = new TH1F("h_tracking_theta", "Track polar angle;#theta (rad);counts", 1000, 0, 0.1);
    TH1F *h_tracking_phi = new TH1F("h_tracking_phi", "Track azimuthal angle;#phi (rad);counts", 1000, -3.1415, +3.1415);
    //  Intercept maps — filled for ALL selected tracks, propagated to each plane
    TH2F *h_intercept_drich = new TH2F("h_intercept_drich", "Track intercept at dRICH plane (z=432.5 mm);x (mm);y (mm)", 200, -50., +50., 200, -50., +50.);
    TH2F *h_intercept_scint = new TH2F("h_intercept_scint", "Track intercept at scintillator plane (z=115.0 mm);x (mm);y (mm)", 200, -50., +50., 200, -50., +50.);

    //  First loop over frames
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

        //  QA — fill tracking distributions and intercept maps
        for (auto i = 0; i < recotrackdata->n_recotrackdata(); i++)
        {
            float plane_x = recotrackdata->get_det_plane_x(i);
            float plane_y = recotrackdata->get_det_plane_y(i);
            float slope_x = recotrackdata->get_traj_angcoeff_x(i);
            float slope_y = recotrackdata->get_traj_angcoeff_y(i);

            h_tracking_theta->Fill(recotrackdata->get_traj_angcoeff_theta(i));
            h_tracking_phi->Fill(recotrackdata->get_traj_angcoeff_phi(i));

            //  Fill both maps only for tracks that pass the selection —
            //  each map shows the surviving tracks propagated to its own plane
            if (is_track_selected(plane_x, plane_y, slope_x, slope_y, cutg, cut_plane, cut_side))
            {
                h_intercept_drich->Fill(plane_x - slope_x * z_drich, plane_y - slope_y * z_drich);
                h_intercept_scint->Fill(plane_x - slope_x * z_scint, plane_y - slope_y * z_scint);
            }
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

    float found_ring_center_x = h_first_round_X->GetMean();
    float found_ring_center_y = h_first_round_Y->GetMean();
    float found_ring_radius = h_first_round_R->GetMean();
    h_first_round_R->Fit("gaus", "Q");
    auto gaus_f = h_first_round_R->GetFunction("gaus");
    float found_ring_radius_stddev = gaus_f->GetParameter(2);

    //  Second round histograms
    TH2F *h_second_round_xy_map = new TH2F("h_second_round_xy_map", "Selected hits on detector plane;x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH1F *h_second_round_R = new TH1F("h_second_round_R", "Ring radius residual (2nd round);#DeltaR (mm);counts", 200, -20, 20);
    TH1F *h_n_selected_hits = new TH1F("h_n_selected_hits", "Selected hits per event;n hits;counts", 100, 0, 100);
    TH2F *h_n_selected_hits_vs_theta = new TH2F("h_n_selected_hits_vs_theta", "Selected hits vs track polar angle;#theta (rad);n hits", 10, 0, 0.01, 100, -0.5, 99.5);
    TH2F *h_second_round_R_vs_theta = new TH2F("h_second_round_R_vs_theta", "Ring radius residual vs track azimuthal angle;#phi (rad);#DeltaR (mm)", 10, -3.1415, 3.1415, 200, -20, 20);

    /// Ricava il range direttamente da h_intercept_drich già riempito
    TH1D *h_proj_x = h_intercept_drich->ProjectionX("proj_x");
    float ix_drich_min = h_proj_x->GetXaxis()->GetBinLowEdge(h_proj_x->FindFirstBinAbove(0));
    float ix_drich_max = h_proj_x->GetXaxis()->GetBinUpEdge(h_proj_x->FindLastBinAbove(0));
    float ix_margin = 0.05 * (ix_drich_max - ix_drich_min);
    ix_drich_min -= ix_margin;
    ix_drich_max += ix_margin;

    TH2F *h_n_selected_hits_vs_ix_drich = new TH2F("h_n_selected_hits_vs_ix_drich",
                                                   "Selected hits vs track intercept X at dRICH;x_{dRICH} (mm);n hits",
                                                   20, ix_drich_min, ix_drich_max, 100, -0.5, 99.5);

    TH2F *h_second_round_R_vs_ix_drich = new TH2F("h_second_round_R_vs_ix_drich",
                                                  "Ring radius residual vs track intercept X at dRICH;x_{dRICH} (mm);#DeltaR (mm)",
                                                  20, ix_drich_min, ix_drich_max, 200, -20., 20.);

    TH2F *h_ring_x0_vs_ix_drich = new TH2F("h_ring_x0_vs_ix_drich",
                                           "Ring center X vs intercept X at dRICH;x_{dRICH} (mm);x_{0} (mm)",
                                           20, ix_drich_min, ix_drich_max, 120, -30, 30);

    TH2F *h_ring_y0_vs_ix_drich = new TH2F("h_ring_y0_vs_ix_drich",
                                           "Ring center Y vs intercept X at dRICH;x_{dRICH} (mm);y_{0} (mm)",
                                           20, ix_drich_min, ix_drich_max, 120, -30, 30);

    TH2F *h_ring_R_vs_ix_drich = new TH2F("h_ring_R_vs_ix_drich",
                                          "Ring radius vs intercept X at dRICH;x_{dRICH} (mm);R (mm)",
                                          20, ix_drich_min, ix_drich_max, 200, 30, 130);

    //  Second loop over frames
    mist::logger::progress_bar bar_second(mist::logger::bar_style::BLOCK);
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        recotrackdata_tree->GetEntry(i_frame);

        std::vector<std::array<float, 2>> selected_points;

        if (i_frame % 10000 == 0)
            bar_second.update(i_frame, all_frames);

        if (recotrackdata->is_start_of_spill())
            continue;

        auto default_hardware_trigger = recotrackdata->get_trigger_by_index(0);
        auto streaming_trigger = recotrackdata->get_trigger_by_index(104);
        if (streaming_trigger && default_hardware_trigger)
        {
            // n_tracks
            // if (recotrackdata->n_recotrackdata() != 1)
            //     continue;

            float plane_x = recotrackdata->get_det_plane_x(0);
            float plane_y = recotrackdata->get_det_plane_y(0);
            float slope_x = recotrackdata->get_traj_angcoeff_x(0);
            float slope_y = recotrackdata->get_traj_angcoeff_y(0);

            float ix_drich = plane_x - slope_x * z_drich;
            float iy_drich = plane_y - slope_y * z_drich;

            if (!is_track_selected(plane_x, plane_y, slope_x, slope_y, cutg, cut_plane, cut_side))
                continue;

            for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
            {
                if (recotrackdata->is_afterpulse(current_hit))
                    continue;

                auto time_delta_wrt_ref = recotrackdata->get_hit_t(current_hit) - streaming_trigger->fine_time;
                if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
                    continue;

                float hit_x = recotrackdata->get_hit_x(current_hit);
                float hit_y = recotrackdata->get_hit_y(current_hit);
                if (fabs(recotrackdata->get_hit_r(current_hit, {found_ring_center_x, found_ring_center_y}) - found_ring_radius) > 5 * found_ring_radius_stddev)
                    continue;

                selected_points.push_back({hit_x, hit_y});
                h_second_round_xy_map->Fill(recotrackdata->get_hit_x_rnd(current_hit), recotrackdata->get_hit_y_rnd(current_hit));
            }

            h_n_selected_hits->Fill(selected_points.size());
            h_n_selected_hits_vs_theta->Fill(recotrackdata->get_traj_angcoeff_theta(0), selected_points.size());
            h_n_selected_hits_vs_ix_drich->Fill(ix_drich, selected_points.size());

            if (fabs((int)selected_points.size() - 15) < 2)
            {
                auto fit_result = fit_circle(selected_points, {found_ring_center_x, found_ring_center_y, found_ring_radius}, true, {{}});
                h_second_round_R->Fill(fit_result[2][0] - found_ring_radius);

                auto slope_phi = recotrackdata->get_traj_angcoeff_phi(0);
                h_second_round_R_vs_theta->Fill(slope_phi, fit_result[2][0] - found_ring_radius);
                h_second_round_R_vs_ix_drich->Fill(ix_drich, fit_result[2][0] - found_ring_radius);
                h_ring_x0_vs_ix_drich->Fill(ix_drich, fit_result[0][0]);
                h_ring_y0_vs_ix_drich->Fill(ix_drich, fit_result[1][0]);
                h_ring_R_vs_ix_drich->Fill(ix_drich, fit_result[2][0]);
            }
        }
    }
    bar_second.update(all_frames, all_frames);
    bar_second.finish();

    //  --- Drawing ---

    TCanvas *c_time_delta = new TCanvas("c_time_delta", "Hit time wrt trigger", 800, 800);
    gPad->SetLogy();
    h_t_distribution->SetLineColor(kBlack);
    h_t_distribution->SetLineWidth(2);
    h_t_distribution->Draw();

    TCanvas *c_first_round = new TCanvas("c_first_round", "Ring fit results - 1st round", 1200, 400);
    c_first_round->Divide(3, 1);
    c_first_round->cd(1);
    h_first_round_X->Draw();
    c_first_round->cd(2);
    h_first_round_Y->Draw();
    c_first_round->cd(3);
    h_first_round_R->Draw();

    TCanvas *c_second_round_tracking = new TCanvas("c_second_round_tracking", "Tracking angle distributions", 800, 400);
    c_second_round_tracking->Divide(2, 1);
    c_second_round_tracking->cd(1);
    h_tracking_theta->Draw();
    c_second_round_tracking->cd(2);
    h_tracking_phi->Draw();

    TCanvas *c_intercepts = new TCanvas("c_intercepts", "Track intercept maps (selected tracks)", 1600, 800);
    c_intercepts->Divide(2, 1);
    c_intercepts->cd(1);
    h_intercept_drich->Draw("COLZ");
    c_intercepts->cd(2);
    h_intercept_scint->Draw("COLZ");

    TCanvas *c_second_round_R = new TCanvas("c_second_round_R", "Ring radius residuals - 2nd round", 1200, 600);
    c_second_round_R->Divide(2, 1);
    c_second_round_R->cd(1);
    h_second_round_R->Draw();
    c_second_round_R->cd(2);
    h_second_round_R_vs_theta->Draw("COLZ");

    TCanvas *c_n_selected_hits = new TCanvas("c_n_selected_hits", "Selected hits per event", 1200, 600);
    c_n_selected_hits->Divide(2, 1);
    c_n_selected_hits->cd(1);
    h_n_selected_hits->Draw();
    c_n_selected_hits->cd(2);
    h_n_selected_hits_vs_theta->Draw("COLZ");

    TCanvas *c_sigma = new TCanvas("c_sigma", "Spatial resolution vs #phi", 800, 800);
    TGraphErrors *g_sigma = new TGraphErrors();
    g_sigma->SetTitle("Spatial resolution vs track #phi;#phi (rad);#sigma_{R} (mm)");
    for (auto x_bin = 1; x_bin <= h_second_round_R_vs_theta->GetNbinsX(); x_bin++)
    {
        auto current_point = g_sigma->GetN();
        auto current_slice = h_second_round_R_vs_theta->ProjectionY(Form("%i", x_bin), x_bin, x_bin);
        if (current_slice->GetEntries() < 50)
            continue;
        current_slice->Fit("gaus", "Q");
        auto gaus_f = current_slice->GetFunction("gaus");
        g_sigma->SetPoint(current_point, h_second_round_R_vs_theta->GetXaxis()->GetBinCenter(x_bin), gaus_f->GetParameter(2));
        g_sigma->SetPointError(current_point, 0.5 * h_second_round_R_vs_theta->GetXaxis()->GetBinWidth(x_bin), gaus_f->GetParError(2));
    }
    g_sigma->Draw("ALP");

    TCanvas *c_photons = new TCanvas("c_photons", "Photon yield vs #theta", 800, 800);
    TF1 *fitfunc = new TF1("fitfunc", "[1]*TMath::PoissonI(x,[0])", 0, 100);
    TGraphErrors *g_photons = new TGraphErrors();
    g_photons->SetTitle("Mean photon yield vs track #theta;#theta (rad);#mu_{p.e.}");
    for (auto x_bin = 1; x_bin <= h_n_selected_hits_vs_theta->GetNbinsX(); x_bin++)
    {
        auto current_point = g_photons->GetN();
        auto current_slice = h_n_selected_hits_vs_theta->ProjectionY(Form("%i", x_bin), x_bin, x_bin);
        if (current_slice->GetEntries() < 50)
            continue;
        fitfunc->SetParameter(0, 16.);
        fitfunc->SetParameter(1, 1.);
        current_slice->Fit("fitfunc", "Q");
        auto gaus_f = current_slice->GetFunction("fitfunc");
        g_photons->SetPoint(current_point, h_n_selected_hits_vs_theta->GetXaxis()->GetBinCenter(x_bin), gaus_f->GetParameter(0));
        g_photons->SetPointError(current_point, 0.5 * h_n_selected_hits_vs_theta->GetXaxis()->GetBinWidth(x_bin), gaus_f->GetParError(0));
    }
    g_photons->Draw("ALP");

    TCanvas *c_photons_ix = new TCanvas("c_photons_ix", "Photon yield vs x_{dRICH}", 800, 800);
    TGraphErrors *g_photons_ix = new TGraphErrors();
    g_photons_ix->SetTitle("Mean photon yield vs intercept X at dRICH;x_{dRICH} (mm);#mu_{p.e.}");
    for (auto x_bin = 1; x_bin <= h_n_selected_hits_vs_ix_drich->GetNbinsX(); x_bin++)
    {
        auto current_point = g_photons_ix->GetN();
        auto current_slice = h_n_selected_hits_vs_ix_drich->ProjectionY(Form("ix_%i", x_bin), x_bin, x_bin);
        if (current_slice->GetEntries() < 50)
            continue;
        fitfunc->SetParameter(0, 16.);
        fitfunc->SetParameter(1, 1.);
        current_slice->Fit("fitfunc", "Q");
        auto f = current_slice->GetFunction("fitfunc");
        g_photons_ix->SetPoint(current_point,
                               h_n_selected_hits_vs_ix_drich->GetXaxis()->GetBinCenter(x_bin),
                               f->GetParameter(0));
        g_photons_ix->SetPointError(current_point,
                                    0.5 * h_n_selected_hits_vs_ix_drich->GetXaxis()->GetBinWidth(x_bin),
                                    f->GetParError(0));
    }
    g_photons_ix->Draw("ALP");

    TCanvas *c_second_round_map = new TCanvas("c_second_round_map", "Selected hits on detector plane - 2nd round QA", 800, 800);
    h_second_round_xy_map->Draw("COLZ");

    TCanvas *c_ring_params_ix = new TCanvas("c_ring_params_ix", "Ring fit params vs x_{dRICH}", 1800, 600);
    c_ring_params_ix->Divide(3, 1);

    auto make_profile_graph = [&](TH2F *h2, const char *title) -> TGraphErrors *
    {
        TGraphErrors *g = new TGraphErrors();
        g->SetTitle(title);
        for (int xb = 1; xb <= h2->GetNbinsX(); xb++)
        {
            auto sl = h2->ProjectionY(Form("sl_%s_%i", h2->GetName(), xb), xb, xb);
            if (sl->GetEntries() < 50)
                continue;
            sl->Fit("gaus", "Q");
            auto f = sl->GetFunction("gaus");
            int n = g->GetN();
            g->SetPoint(n, h2->GetXaxis()->GetBinCenter(xb), f->GetParameter(1));
            g->SetPointError(n, 0.5 * h2->GetXaxis()->GetBinWidth(xb), f->GetParameter(2));
        }
        return g;
    };

    c_ring_params_ix->cd(1);
    make_profile_graph(h_ring_x0_vs_ix_drich, "Ring center x_{0} vs x_{dRICH};x_{dRICH} (mm);x_{0} (mm)")->Draw("ALP");
    c_ring_params_ix->cd(2);
    make_profile_graph(h_ring_y0_vs_ix_drich, "Ring center y_{0} vs x_{dRICH};x_{dRICH} (mm);y_{0} (mm)")->Draw("ALP");
    c_ring_params_ix->cd(3);
    make_profile_graph(h_ring_R_vs_ix_drich, "Ring radius R vs x_{dRICH};x_{dRICH} (mm);R (mm)")->Draw("ALP");
}