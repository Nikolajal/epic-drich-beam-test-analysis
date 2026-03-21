#include "../lib_loader.h"
#include "TCutG.h"
#include "ringtrack_config.h"
#include <fstream>
#include <sstream>
#include <array>

int n_rejected = 0;

constexpr float z_drich = -4250;
constexpr float z_scint = -1150;

enum class CutPlane { NONE, DRICH, SCINT, BOTH };
enum class CutSide  { INSIDE, OUTSIDE };

// ---------------------------------------------------------------------------
//  TCutG factories
// ---------------------------------------------------------------------------
TCutG *make_cutg_scint_stretto()
{
    TCutG *cutg = new TCutG("cutg_scint_stretto", 8);
    cutg->SetPoint(0, -8.0798, -1.49754);
    cutg->SetPoint(1, -5.97317, 2.84483);
    cutg->SetPoint(2, -2.97012, 2.81117);
    cutg->SetPoint(3,  0.660441, 0.95977);
    cutg->SetPoint(4, -3.05976, -3.07964);
    cutg->SetPoint(5, -8.16944, -1.43021);
    cutg->SetPoint(6, -8.16944, -1.43021);
    cutg->SetPoint(7, -8.0798, -1.49754);
    cutg->SetVarX("x"); cutg->SetVarY("y");
    cutg->SetTitle("Scint stretto");
    return cutg;
}

TCutG *make_cutg_scint_largo()
{
    TCutG *cutg = new TCutG("cutg_scint_largo", 10);
    cutg->SetPoint(0, -8.34873, -1.49754);
    cutg->SetPoint(1, -4.9871,   5.50411);
    cutg->SetPoint(2,  0.525976,  5.53777);
    cutg->SetPoint(3, 11.7314,   0.95977);
    cutg->SetPoint(4, 11.5073,  -6.1092);
    cutg->SetPoint(5,  0.436333, -6.04187);
    cutg->SetPoint(6, -8.39355, -1.86782);
    cutg->SetPoint(7, -8.39355, -1.49754);
    cutg->SetPoint(8, -8.39355, -1.49754);
    cutg->SetPoint(9, -8.34873, -1.49754);
    cutg->SetVarX("x"); cutg->SetVarY("y");
    cutg->SetTitle("Scint largo");
    return cutg;
}

TCutG *make_cutg_drich_stretto()
{
    TCutG *cutg = new TCutG("cutg_drich_stretto", 7);
    cutg->SetPoint(0, -4.92388, -0.289291);
    cutg->SetPoint(1, -3.30728,  3.0507);
    cutg->SetPoint(2, -0.67378,  0.447086);
    cutg->SetPoint(3, -2.21216, -2.68252);
    cutg->SetPoint(4, -4.92388, -0.31559);
    cutg->SetPoint(5, -4.94996, -0.31559);
    cutg->SetPoint(6, -4.92388, -0.289291);
    cutg->SetVarX("x"); cutg->SetVarY("y");
    cutg->SetTitle("dRICH stretto");
    return cutg;
}

TCutG *make_cutg_drich_largo()
{
    TCutG *cutg = new TCutG("cutg_drich_largo", 9);
    cutg->SetPoint(0, -5.65396, -0.525984);
    cutg->SetPoint(1, -2.78579,  5.25984);
    cutg->SetPoint(2,  3.78492,  0.657479);
    cutg->SetPoint(3,  3.81099, -4.15527);
    cutg->SetPoint(4,  2.53335, -5.31243);
    cutg->SetPoint(5, -1.03882, -5.31243);
    cutg->SetPoint(6, -5.68004, -0.473385);
    cutg->SetPoint(7, -5.68004, -0.473385);
    cutg->SetPoint(8, -5.65396, -0.525984);
    cutg->SetVarX("x"); cutg->SetVarY("y");
    cutg->SetTitle("dRICH largo");
    return cutg;
}

TCutG *make_cutg_rect(float xmin, float xmax, float ymin, float ymax)
{
    TCutG *cutg = new TCutG("cutg_rect", 5);
    cutg->SetPoint(0, xmin, ymin);
    cutg->SetPoint(1, xmax, ymin);
    cutg->SetPoint(2, xmax, ymax);
    cutg->SetPoint(3, xmin, ymax);
    cutg->SetPoint(4, xmin, ymin);
    cutg->SetVarX("x"); cutg->SetVarY("y");
    cutg->SetTitle(Form("rect_%.0f_%.0f_%.0f_%.0f", xmin, xmax, ymin, ymax));
    return cutg;
}

// ---------------------------------------------------------------------------
//  Config helpers
// ---------------------------------------------------------------------------
CutPlane parse_plane(const std::string &s)
{
    if (s == "DRICH") return CutPlane::DRICH;
    if (s == "SCINT") return CutPlane::SCINT;
    if (s == "BOTH")  return CutPlane::BOTH;
    return CutPlane::NONE;
}

CutSide parse_side(const std::string &s)
{
    return (s == "OUTSIDE") ? CutSide::OUTSIDE : CutSide::INSIDE;
}

TCutG *parse_cutg(const std::string &s)
{
    if (s == "scint_largo")   return make_cutg_scint_largo();
    if (s == "scint_stretto") return make_cutg_scint_stretto();
    if (s == "drich_largo")   return make_cutg_drich_largo();
    if (s == "drich_stretto") return make_cutg_drich_stretto();
    if (s.size() >= 4 && s.substr(0, 4) == "rect")
    {
        std::istringstream ss(s.substr(4));
        float xmin, xmax, ymin, ymax;
        ss >> xmin >> xmax >> ymin >> ymax;
        return make_cutg_rect(xmin, xmax, ymin, ymax);
    }
    return nullptr;
}

std::string cut_plane_to_string(CutPlane p)
{
    switch (p) {
        case CutPlane::DRICH: return "DRICH";
        case CutPlane::SCINT: return "SCINT";
        case CutPlane::BOTH:  return "BOTH";
        default:              return "NONE";
    }
}

// ---------------------------------------------------------------------------
//  is_track_selected
// ---------------------------------------------------------------------------
bool is_track_selected(float plane_x, float plane_y, float slope_x, float slope_y,
                       TCutG *cutg1, CutPlane plane1, CutSide side1,
                       TCutG *cutg2, CutPlane plane2, CutSide side2)
{
    float ix_drich = plane_x + slope_x * z_drich;
    float iy_drich = plane_y + slope_y * z_drich;
    float ix_scint = plane_x + slope_x * z_scint;
    float iy_scint = plane_y + slope_y * z_scint;

    auto eval = [&](TCutG *cutg, CutPlane plane, CutSide side) -> bool
    {
        if (plane == CutPlane::NONE || cutg == nullptr) return true;
        auto pass = [&](bool inside) {
            return (side == CutSide::INSIDE) ? inside : !inside;
        };
        switch (plane) {
            case CutPlane::DRICH: return pass(cutg->IsInside(ix_drich, iy_drich));
            case CutPlane::SCINT: return pass(cutg->IsInside(ix_scint, iy_scint));
            case CutPlane::BOTH:  return pass(cutg->IsInside(ix_drich, iy_drich)) &&
                                         pass(cutg->IsInside(ix_scint, iy_scint));
            default: return true;
        }
    };
    return eval(cutg1, plane1, side1) && eval(cutg2, plane2, side2);
}

// ===========================================================================
//  MAIN FUNCTION
// ===========================================================================
void ringtrack_analysis(std::string data_repository, std::string run_name,
                        std::string conf_path = "ringtrack.conf")
{
    // -------------------------------------------------------------------------
    //  Load config
    // -------------------------------------------------------------------------
    RingtrackConfig cfg;
    cfg.load(conf_path);
    cfg.print();

    const int   first_event  = cfg.get_int("first_event", 0);
    const int   max_frames_  = cfg.get_int("max_frames",  1000000);
    const int   event_shift  = cfg.get_int("event_shift", 0);

    const bool require_single_track   = cfg.get_bool("require_single_track",   false);
    const bool require_multi_track    = cfg.get_bool("require_multi_track",    false);
    const bool apply_theta_phi_cut    = cfg.get_bool("apply_theta_phi_cut",    false);
    const bool apply_angle_xy_cut     = cfg.get_bool("apply_angle_xy_cut",     false);
    const bool apply_radial_cut       = cfg.get_bool("apply_radial_cut",       false);
    const bool apply_multiplicity_cut = cfg.get_bool("apply_multiplicity_cut", false);

    const float theta_min   = cfg.get_float("theta_min",    0.f);
    const float theta_max   = cfg.get_float("theta_max",    0.0001f);
    const float phi_min     = cfg.get_float("phi_min",     -3.f);
    const float phi_max     = cfg.get_float("phi_max",      3.f);
    const float angle_x_min = cfg.get_float("angle_x_min", -0.05f);
    const float angle_x_max = cfg.get_float("angle_x_max",  0.05f);
    const float angle_y_min = cfg.get_float("angle_y_min", -0.05f);
    const float angle_y_max = cfg.get_float("angle_y_max",  0.05f);

    const std::array<float,2> time_cut_boundaries = {
        cfg.get_float("time_cut_min", -50.f),
        cfg.get_float("time_cut_max",  20.f)
    };

    const CutPlane plane1 = parse_plane(cfg.get_string("plane1", "NONE"));
    const CutSide  side1  = parse_side (cfg.get_string("side1",  "INSIDE"));
    TCutG         *cutg1  = parse_cutg (cfg.get_string("cutg1",  "scint_largo"));

    const CutPlane plane2 = parse_plane(cfg.get_string("plane2", "NONE"));
    const CutSide  side2  = parse_side (cfg.get_string("side2",  "OUTSIDE"));
    TCutG         *cutg2  = parse_cutg (cfg.get_string("cutg2",  "rect -45 25 -25 25"));

    const int         n_display_events       = cfg.get_int("n_display_events", 100000);
    const std::string display_nhits_cut_mode = cfg.get_string("display_nhits_cut_mode", "none");
    const int         display_nhits_min      = cfg.get_int("display_nhits_min", 0);
    const int         display_nhits_max      = cfg.get_int("display_nhits_max", 8);

    // display_all_tracks: attivo se require_multi_track è true
    const bool display_all_tracks = require_multi_track;

    if (require_single_track && require_multi_track)
        std::cerr << "[WARNING] require_single_track e require_multi_track entrambi true — tutti gli eventi con >= 1 traccia passano\n";

    // -------------------------------------------------------------------------
    //  Open input file
    // -------------------------------------------------------------------------
    std::string input_filename = data_repository + "/" + run_name + "/recotrackdata.root";
    TFile *input_file = new TFile(input_filename.c_str());
    if (!input_file || input_file->IsZombie())
    {
        std::cerr << "[ERROR] Could not open " << input_filename << std::endl;
        return;
    }

    TTree *recotrackdata_tree = (TTree *)input_file->Get("recotrackdata");
    alcor_recotrackdata *recotrackdata = new alcor_recotrackdata();
    recotrackdata->link_to_tree(recotrackdata_tree);

    const long long n_frames   = recotrackdata_tree->GetEntries();
    const int       all_frames = (int)min((long long)first_event + max_frames_, n_frames);

    // =========================================================================
    //  HISTOGRAMS — first round
    // =========================================================================
    TH1F *h_t_distribution = new TH1F("h_t_distribution",
        "Hit time wrt trigger;t_{hit} - t_{timing} (ns);counts", 200, -312.5, 312.5);
    TH1F *h_t_distribution_track = new TH1F("h_t_distribution_track",
        "Hit time wrt trigger (with track);t_{hit} - t_{timing} (ns);counts", 200, -312.5, 312.5);
    TH1F *h_first_round_X = new TH1F("h_first_round_X",
        "Ring center X (1st round);x_{0} (mm);counts", 120, -30, 30);
    TH1F *h_first_round_Y = new TH1F("h_first_round_Y",
        "Ring center Y (1st round);y_{0} (mm);counts", 120, -30, 30);
    TH1F *h_first_round_R = new TH1F("h_first_round_R",
        "Ring radius (1st round);R (mm);counts", 200, 30, 130);
    TH1F *h_tracking_theta = new TH1F("h_tracking_theta",
        "Track polar angle;#theta (rad);counts", 1000, 0, 0.1);
    TH1F *h_tracking_phi = new TH1F("h_tracking_phi",
        "Track azimuthal angle;#phi (rad);counts", 1000, -3.1415, +3.1415);
    TH1F *h_tracking_angle_x = new TH1F("h_tracking_angle_x",
        "Track angle X;#alpha_{x} (rad);counts", 1000, -0.1, 0.1);
    TH1F *h_tracking_angle_y = new TH1F("h_tracking_angle_y",
        "Track angle Y;#alpha_{y} (rad);counts", 1000, -0.1, 0.1);
    TH2F *h_intercept_drich = new TH2F("h_intercept_drich",
        "Track intercept at dRICH plane;x (mm);y (mm)", 500, -200., +200., 200, -200., +200.);
    TH2F *h_intercept_scint = new TH2F("h_intercept_scint",
        "Track intercept at scintillator plane;x (mm);y (mm)", 500, -200., +200., 200, -200., +200.);
    TH1F *h_event_multiplicity = new TH1F("h_event_multiplicity",
        "Track multiplicity per event;n tracks;n events", 10, -0.5, 9.5);

    // =========================================================================
    //  FIRST LOOP
    // =========================================================================
    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        recotrackdata_tree->GetEntry(i_frame);
        std::vector<std::array<float, 2>> selected_points;
        float avg_radius = 0.;

        if (i_frame % 10000 == 0) bar.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;

        for (auto i = 0; i < recotrackdata->n_recotrackdata(); i++)
        {
            h_tracking_theta->Fill(recotrackdata->get_traj_angcoeff_theta(i));
            h_tracking_phi->Fill(recotrackdata->get_traj_angcoeff_phi(i));
            h_tracking_angle_x->Fill(atan(recotrackdata->get_traj_angcoeff_x(i)));
            h_tracking_angle_y->Fill(atan(recotrackdata->get_traj_angcoeff_y(i)));
        }
        h_event_multiplicity->Fill(recotrackdata->n_recotrackdata());

        auto default_hardware_trigger = recotrackdata->get_trigger_by_index(0);
        if (default_hardware_trigger)
        {
            for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
            {
                if (recotrackdata->is_afterpulse(current_hit)) continue;
                auto time_delta = recotrackdata->get_hit_t(current_hit) - default_hardware_trigger->fine_time;
                h_t_distribution->Fill(time_delta);
                if (time_delta < time_cut_boundaries[0] || time_delta > time_cut_boundaries[1]) continue;
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

    float found_ring_center_x    = h_first_round_X->GetMean();
    float found_ring_center_y    = h_first_round_Y->GetMean();
    float found_ring_radius      = h_first_round_R->GetMean();
    h_first_round_R->Fit("gaus", "Q");
    auto  gaus_f                 = h_first_round_R->GetFunction("gaus");
    float found_ring_radius_stddev = gaus_f->GetParameter(2);

    // =========================================================================
    //  HISTOGRAMS — second round
    // =========================================================================
    std::vector<double> ix_drich_range = {-60, -40, -20, -10, -6, -2, 0, 2, 6, 10, 20, 40, 60};

    std::ofstream intercept_file(data_repository + "/" + run_name + "/intercepts.txt");
    intercept_file << "track_frame\thit_frame\tplane_x\tplane_y\tix_drich\tiy_drich\n";

    TH2F *h_second_round_xy_map = new TH2F("h_second_round_xy_map",
        "Selected hits on detector plane;x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH2F *h_second_round_xy_map_rejected = new TH2F("h_second_round_xy_map_rejected",
        "Rejected hits on detector plane;x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH1F *h_second_round_R = new TH1F("h_second_round_R",
        "Ring radius residual (2nd round);#DeltaR (mm);counts", 200, -20, 20);
    TH1F *h_n_selected_hits = new TH1F("h_n_selected_hits",
        "Selected hits per event;n hits;counts", 100, 0, 100);
    TH2F *h_n_selected_hits_vs_multiplicity = new TH2F("h_n_selected_hits_vs_multiplicity",
        "Selected hits vs track multiplicity;n tracks;n hits", 100, -0.5, 9.5, 100, -0.5, 99.5);
    TH2F *h_n_selected_hits_vs_theta = new TH2F("h_n_selected_hits_vs_theta",
        "Selected hits vs track polar angle;#theta (rad);n hits", 2000, 0, 0.1, 100, -0.5, 99.5);
    TH2F *h_n_selected_hits_vs_ix_drich = new TH2F("h_n_selected_hits_vs_ix_drich",
        "Selected hits vs track intercept X at dRICH;x_{dRICH} (mm);n hits",
        120, -200., 200., 100, -0.5, 99.5);
    TH2F *h_n_selected_hits_vs_iy_drich = new TH2F("h_n_selected_hits_vs_iy_drich",
        "Selected hits vs track intercept Y at dRICH;y_{dRICH} (mm);n hits",
        120, -200., 200., 100, -0.5, 99.5);
    TH2F *h_deltaR_vs_theta = new TH2F("h_deltaR_vs_theta",
        "#DeltaR vs track #theta;#theta (rad);#DeltaR (mm)", 50, 0, 0.1, 200, -20, 20);
    TH2F *h_deltaR_vs_phi = new TH2F("h_deltaR_vs_phi",
        "#DeltaR vs track #phi;#phi (rad);#DeltaR (mm)", 50, -3.1415, 3.1415, 200, -20, 20);
    TH2F *h_deltaR_vs_ix_drich = new TH2F("h_deltaR_vs_ix_drich",
        "#DeltaR vs track intercept X at dRICH;x_{dRICH} (mm);#DeltaR (mm)",
        50, -60., 60., 200, -20., 20.);
    TH2F *h_deltaR_vs_iy_drich = new TH2F("h_deltaR_vs_iy_drich",
        "#DeltaR vs track intercept Y at dRICH;y_{dRICH} (mm);#DeltaR (mm)",
        50, -60., 60., 200, -20., 20.);
    TH1F *h_d_intercept = new TH1F("h_d_intercept",
        "Distance intercept-ring center at dRICH;d_{intercept} (mm);counts", 100, 0., 50.);
    TH2F *h_deltaR_vs_d_intercept = new TH2F("h_deltaR_vs_d_intercept",
        "#DeltaR vs distance intercept-ring center;d_{intercept} (mm);#DeltaR (mm)",
        100, 0., 50., 500, -20., 20.);
    TH2F *h_ring_R_vs_d_intercept = new TH2F("h_ring_R_vs_d_intercept",
        "Ring radius vs distance intercept-ring center;d_{intercept} (mm);R (mm)",
        100, 0., 50., 200, 30, 130);
    TH2F *h_ring_x0_vs_ix_drich = new TH2F("h_ring_x0_vs_ix_drich",
        "Ring center X vs intercept X at dRICH;x_{dRICH} (mm);x_{0} (mm)",
        ix_drich_range.size()-1, ix_drich_range.data(), 120, -30, 30);
    TH2F *h_ring_y0_vs_ix_drich = new TH2F("h_ring_y0_vs_ix_drich",
        "Ring center Y vs intercept X at dRICH;x_{dRICH} (mm);y_{0} (mm)",
        ix_drich_range.size()-1, ix_drich_range.data(), 120, -30, 30);
    TH2F *h_ring_R_vs_ix_drich = new TH2F("h_ring_R_vs_ix_drich",
        "Ring radius vs intercept X at dRICH;x_{dRICH} (mm);R (mm)",
        ix_drich_range.size()-1, ix_drich_range.data(), 200, 30, 130);
    TH2F *h_ring_x0_vs_iy_drich = new TH2F("h_ring_x0_vs_iy_drich",
        "Ring center X vs intercept Y at dRICH;y_{dRICH} (mm);x_{0} (mm)",
        ix_drich_range.size()-1, ix_drich_range.data(), 120, -30, 30);
    TH2F *h_ring_y0_vs_iy_drich = new TH2F("h_ring_y0_vs_iy_drich",
        "Ring center Y vs intercept Y at dRICH;y_{dRICH} (mm);y_{0} (mm)",
        ix_drich_range.size()-1, ix_drich_range.data(), 120, -30, 30);
    TH2F *h_ring_R_vs_iy_drich = new TH2F("h_ring_R_vs_iy_drich",
        "Ring radius vs intercept Y at dRICH;y_{dRICH} (mm);R (mm)",
        ix_drich_range.size()-1, ix_drich_range.data(), 200, 30, 130);
    TH2F *h_ring_x0_vs_theta = new TH2F("h_ring_x0_vs_theta",
        "Ring center X vs track #theta;#theta (rad);x_{0} (mm)", 50, 0, 0.1, 120, -30, 30);
    TH2F *h_ring_y0_vs_theta = new TH2F("h_ring_y0_vs_theta",
        "Ring center Y vs track #theta;#theta (rad);y_{0} (mm)", 50, 0, 0.1, 120, -30, 30);
    TH2F *h_ring_R_vs_theta = new TH2F("h_ring_R_vs_theta",
        "Ring radius vs track #theta;#theta (rad);R (mm)", 50, 0, 0.1, 200, 30, 130);
    TH2F *h_ring_x0_vs_phi = new TH2F("h_ring_x0_vs_phi",
        "Ring center X vs track #phi;#phi (rad);x_{0} (mm)", 50, -3.1415, 3.1415, 120, -30, 30);
    TH2F *h_ring_y0_vs_phi = new TH2F("h_ring_y0_vs_phi",
        "Ring center Y vs track #phi;#phi (rad);y_{0} (mm)", 50, -3.1415, 3.1415, 120, -30, 30);
    TH2F *h_ring_R_vs_phi = new TH2F("h_ring_R_vs_phi",
        "Ring radius vs track #phi;#phi (rad);R (mm)", 50, -3.1415, 3.1415, 200, 30, 130);

    TH2F *h_display_hits = new TH2F("h_display_hits",
        Form("Hits (first %d selected events);x (mm);y (mm)", n_display_events),
        396, -200, 200, 396, -200, 200);

    TGraph *g_display_intercepts_single = new TGraph();
    g_display_intercepts_single->SetName("g_display_intercepts_single");
    g_display_intercepts_single->SetTitle(Form("Intercepts - single track (first %d events);x (mm);y (mm)", n_display_events));

    TGraph *g_display_intercepts_multi = new TGraph();
    g_display_intercepts_multi->SetName("g_display_intercepts_multi");
    g_display_intercepts_multi->SetTitle(Form("Intercepts - multi track (first %d events);x (mm);y (mm)", n_display_events));

    int n_display_filled = 0;

    TH2F *h_intercept_drich_zero_hits = new TH2F("h_intercept_drich_zero_hits",
        "Track intercept at dRICH (0 hit events);x (mm);y (mm)", 120, -60., 60., 120, -60., 60.);

    // =========================================================================
    //  SECOND LOOP
    // =========================================================================
    mist::logger::progress_bar bar_second(mist::logger::bar_style::BLOCK);
    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        recotrackdata_tree->GetEntry(i_frame);
        std::vector<std::array<float, 2>> selected_points;

        if (i_frame % 1000 == 0) bar_second.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;

        auto default_hardware_trigger = recotrackdata->get_trigger_by_index(0);
        if (!default_hardware_trigger) continue;

        const int n_tracks = recotrackdata->n_recotrackdata();

        // molteplicità
        if ( require_single_track && !require_multi_track && n_tracks != 1) continue;
        if (!require_single_track &&  require_multi_track && n_tracks <  2) continue;
        if (!require_single_track && !require_multi_track && n_tracks <  1) continue;
        if ( require_single_track &&  require_multi_track && n_tracks <  1) continue;

        // best track
        int best_track = 0;
        float best_chi2 = recotrackdata->get_chi2ndof(0);
        for (int i = 1; i < n_tracks; i++)
        {
            float chi2 = recotrackdata->get_chi2ndof(i);
            if (chi2 < best_chi2) { best_chi2 = chi2; best_track = i; }
        }

        const float plane_x     = recotrackdata->get_det_plane_x(best_track);
        const float plane_y     = recotrackdata->get_det_plane_y(best_track);
        const float slope_x     = recotrackdata->get_traj_angcoeff_x(best_track);
        const float slope_y     = recotrackdata->get_traj_angcoeff_y(best_track);
        const float ix_drich    = plane_x + slope_x * z_drich;
        const float iy_drich    = plane_y + slope_y * z_drich;
        const float d_intercept = sqrt(pow(ix_drich - found_ring_center_x, 2) +
                                       pow(iy_drich - found_ring_center_y, 2));

        if (!is_track_selected(plane_x, plane_y, slope_x, slope_y,
                               cutg1, plane1, side1, cutg2, plane2, side2))
        {
            n_rejected++;
            continue;
        }

        const float track_theta   = recotrackdata->get_traj_angcoeff_theta(best_track);
        const float track_phi     = recotrackdata->get_traj_angcoeff_phi(best_track);
        const float track_angle_x = atan(slope_x);
        const float track_angle_y = atan(slope_y);

        if (apply_theta_phi_cut)
        {
            if (track_theta < theta_min || track_theta > theta_max) continue;
            if (track_phi   < phi_min   || track_phi   > phi_max)   continue;
        }
        if (apply_angle_xy_cut)
        {
            if (track_angle_x < angle_x_min || track_angle_x > angle_x_max) continue;
            if (track_angle_y < angle_y_min || track_angle_y > angle_y_max) continue;
        }

        const float saved_ix_scint = plane_x + slope_x * z_scint;
        const float saved_iy_scint = plane_y + slope_y * z_scint;

        // intercette di tutte le tracce per display multi
        std::vector<std::pair<float,float>> all_track_intercepts;
        if (display_all_tracks)
        {
            for (int i = 0; i < n_tracks; i++)
            {
                float px = recotrackdata->get_det_plane_x(i);
                float py = recotrackdata->get_det_plane_y(i);
                float sx = recotrackdata->get_traj_angcoeff_x(i);
                float sy = recotrackdata->get_traj_angcoeff_y(i);
                all_track_intercepts.push_back({px + sx * z_drich, py + sy * z_drich});
            }
        }

        // shift
        int hit_frame = i_frame + event_shift;
        if (hit_frame < first_event || hit_frame >= all_frames) continue;

        recotrackdata_tree->GetEntry(hit_frame);
        auto hit_frame_trigger = recotrackdata->get_trigger_by_index(0);
        if (!hit_frame_trigger) continue;

        // loop hit
        selected_points.clear();
        for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
        {
            if (recotrackdata->is_afterpulse(current_hit)) continue;
            auto time_delta = recotrackdata->get_hit_t(current_hit) - hit_frame_trigger->fine_time;
            if (time_delta < time_cut_boundaries[0] || time_delta > time_cut_boundaries[1]) continue;
            if (apply_radial_cut &&
                fabs(recotrackdata->get_hit_r(current_hit, {found_ring_center_x, found_ring_center_y}) - found_ring_radius) > 5 * found_ring_radius_stddev)
                continue;
            selected_points.push_back({recotrackdata->get_hit_x(current_hit), recotrackdata->get_hit_y(current_hit)});
            h_second_round_xy_map->Fill(recotrackdata->get_hit_x_rnd(current_hit), recotrackdata->get_hit_y_rnd(current_hit));
        }

        const int n_hits = (int)selected_points.size();

        h_n_selected_hits->Fill(n_hits);
        h_n_selected_hits_vs_multiplicity->Fill(n_tracks, n_hits);
        h_n_selected_hits_vs_theta->Fill(track_theta, n_hits);
        h_n_selected_hits_vs_ix_drich->Fill(ix_drich, n_hits);
        h_n_selected_hits_vs_iy_drich->Fill(iy_drich, n_hits);

        intercept_file << i_frame << "\t" << hit_frame << "\t"
                       << plane_x << "\t" << plane_y << "\t"
                       << ix_drich << "\t" << iy_drich << "\n";

        if (n_hits == 0) h_intercept_drich_zero_hits->Fill(ix_drich, iy_drich);

        // display nhits cut
        bool pass_display_nhits = true;
        if (display_nhits_cut_mode == "greater")
            pass_display_nhits = (n_hits > display_nhits_min);
        else if (display_nhits_cut_mode == "range")
            pass_display_nhits = (n_hits >= display_nhits_min && n_hits <= display_nhits_max);

        if (n_display_filled < n_display_events && pass_display_nhits)
        {
            if (n_tracks == 1)
                g_display_intercepts_single->AddPoint(ix_drich, iy_drich);
            else if (display_all_tracks)
                for (auto &p : all_track_intercepts)
                    g_display_intercepts_multi->AddPoint(p.first, p.second);
            else
                g_display_intercepts_multi->AddPoint(ix_drich, iy_drich);

            for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
            {
                if (recotrackdata->is_afterpulse(current_hit)) continue;
                auto time_delta = recotrackdata->get_hit_t(current_hit) - hit_frame_trigger->fine_time;
                if (time_delta < time_cut_boundaries[0] || time_delta > time_cut_boundaries[1]) continue;
                h_display_hits->Fill(recotrackdata->get_hit_x_rnd(current_hit),
                                     recotrackdata->get_hit_y_rnd(current_hit));
            }
        }
        if (n_display_filled < n_display_events) n_display_filled++;

        // fit
        if (n_hits > 4 && (!apply_multiplicity_cut || fabs(n_hits - 15) < 2))
        {
            h_intercept_drich->Fill(ix_drich, iy_drich);
            h_intercept_scint->Fill(saved_ix_scint, saved_iy_scint);
            h_d_intercept->Fill(d_intercept);

            for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
            {
                if (recotrackdata->is_afterpulse(current_hit)) continue;
                auto time_delta = recotrackdata->get_hit_t(current_hit) - hit_frame_trigger->fine_time;
                h_t_distribution_track->Fill(time_delta);
            }

            auto fit_result = fit_circle(selected_points,
                {found_ring_center_x, found_ring_center_y, found_ring_radius}, true, {{}});
            float deltaR = fit_result[2][0] - found_ring_radius;

            h_second_round_R->Fill(deltaR);
            h_deltaR_vs_theta->Fill(track_theta, deltaR);
            h_deltaR_vs_phi->Fill(track_phi, deltaR);
            h_deltaR_vs_ix_drich->Fill(ix_drich, deltaR);
            h_deltaR_vs_iy_drich->Fill(iy_drich, deltaR);
            h_deltaR_vs_d_intercept->Fill(d_intercept, deltaR);
            h_ring_x0_vs_ix_drich->Fill(ix_drich, fit_result[0][0]);
            h_ring_y0_vs_ix_drich->Fill(ix_drich, fit_result[1][0]);
            h_ring_R_vs_ix_drich->Fill(ix_drich,  fit_result[2][0]);
            h_ring_x0_vs_iy_drich->Fill(iy_drich, fit_result[0][0]);
            h_ring_y0_vs_iy_drich->Fill(iy_drich, fit_result[1][0]);
            h_ring_R_vs_iy_drich->Fill(iy_drich,  fit_result[2][0]);
            h_ring_x0_vs_theta->Fill(track_theta, fit_result[0][0]);
            h_ring_y0_vs_theta->Fill(track_theta, fit_result[1][0]);
            h_ring_R_vs_theta->Fill(track_theta,  fit_result[2][0]);
            h_ring_x0_vs_phi->Fill(track_phi, fit_result[0][0]);
            h_ring_y0_vs_phi->Fill(track_phi, fit_result[1][0]);
            h_ring_R_vs_phi->Fill(track_phi,  fit_result[2][0]);
            h_ring_R_vs_d_intercept->Fill(d_intercept, fit_result[2][0]);
        }
    }
    bar_second.update(all_frames, all_frames);
    bar_second.finish();
    intercept_file.close();

    // =========================================================================
    //  THIRD LOOP — no track
    // =========================================================================
    TH2F *h_notrack_xy_map = new TH2F("h_notrack_xy_map",
        "Selected hits (no track);x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH1F *h_notrack_R = new TH1F("h_notrack_R",
        "Ring radius residual (no track);#DeltaR (mm);counts", 200, -20, 20);
    TH1F *h_notrack_n_hits = new TH1F("h_notrack_n_hits",
        "Selected hits per event (no track);n hits;counts", 100, 0, 100);

    mist::logger::progress_bar bar_notrack(mist::logger::bar_style::BLOCK);
    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        recotrackdata_tree->GetEntry(i_frame);
        if (i_frame % 10000 == 0) bar_notrack.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;
        if (recotrackdata->n_recotrackdata() != 0) continue;

        auto default_hardware_trigger = recotrackdata->get_trigger_by_index(0);
        if (!default_hardware_trigger) continue;

        std::vector<std::array<float, 2>> selected_points;
        for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
        {
            if (recotrackdata->is_afterpulse(current_hit)) continue;
            auto time_delta = recotrackdata->get_hit_t(current_hit) - default_hardware_trigger->fine_time;
            if (time_delta < time_cut_boundaries[0] || time_delta > time_cut_boundaries[1]) continue;
            if (apply_radial_cut &&
                fabs(recotrackdata->get_hit_r(current_hit, {found_ring_center_x, found_ring_center_y}) - found_ring_radius) > 5 * found_ring_radius_stddev)
                continue;
            selected_points.push_back({recotrackdata->get_hit_x(current_hit), recotrackdata->get_hit_y(current_hit)});
            h_notrack_xy_map->Fill(recotrackdata->get_hit_x_rnd(current_hit), recotrackdata->get_hit_y_rnd(current_hit));
        }
        h_notrack_n_hits->Fill(selected_points.size());
        if (selected_points.size() > 4)
        {
            auto fit_result = fit_circle(selected_points,
                {found_ring_center_x, found_ring_center_y, found_ring_radius}, true, {{}});
            h_notrack_R->Fill(fit_result[2][0] - found_ring_radius);
        }
    }
    bar_notrack.update(all_frames, all_frames);
    bar_notrack.finish();

    // =========================================================================
    //  SAVE
    // =========================================================================
    std::string output_dir  = data_repository + "/" + run_name + "/plots";
    std::string output_root = output_dir + "/histograms.root";
    gSystem->mkdir(output_dir.c_str(), true);

    TFile *output_file = new TFile(output_root.c_str(), "RECREATE");

    // salva tutti i settings dal conf come TNamed
    for (auto &kv : cfg._data)
        TNamed(kv.first.c_str(), kv.second.c_str()).Write();

    TNamed("found_ring_center_x", Form("%.6f", found_ring_center_x)).Write();
    TNamed("found_ring_center_y", Form("%.6f", found_ring_center_y)).Write();
    TNamed("found_ring_radius",   Form("%.6f", found_ring_radius)).Write();
    TNamed("run_name", run_name.c_str()).Write();

    if (cutg2 && plane2 != CutPlane::NONE)
        cutg2->Write("cutg2_display");

    h_t_distribution->Write();
    h_t_distribution_track->Write();
    h_first_round_X->Write();
    h_first_round_Y->Write();
    h_first_round_R->Write();
    h_tracking_theta->Write();
    h_tracking_phi->Write();
    h_tracking_angle_x->Write();
    h_tracking_angle_y->Write();
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
    h_deltaR_vs_iy_drich->Write();
    h_d_intercept->Write();
    h_deltaR_vs_d_intercept->Write();
    h_ring_R_vs_d_intercept->Write();
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
    h_notrack_xy_map->Write();
    h_notrack_R->Write();
    h_notrack_n_hits->Write();
    h_display_hits->Write();
    g_display_intercepts_single->Write();
    g_display_intercepts_multi->Write();
    h_intercept_drich_zero_hits->Write();
    h_event_multiplicity->Write();

    output_file->Close();

    cout << "Output: " << output_root << endl;
    cout << "Rejected tracks: " << n_rejected << endl;

    int n_eventi_multi = 0, n_tracce_totali_multi = 0, n_tracce_extra_multi = 0;
    for (int b = 3; b <= h_event_multiplicity->GetNbinsX(); b++)
    {
        int mult = b - 1;
        int n_ev = (int)h_event_multiplicity->GetBinContent(b);
        n_eventi_multi        += n_ev;
        n_tracce_totali_multi += n_ev * mult;
        n_tracce_extra_multi  += n_ev * (mult - 1);
    }
    cout << "eventi multitraccia:          " << n_eventi_multi << endl;
    cout << "tracce totali (anche prima):  " << n_tracce_totali_multi << endl;
    cout << "tracce extra (dalla seconda): " << n_tracce_extra_multi << endl;
}