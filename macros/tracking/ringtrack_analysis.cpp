#include "../lib_loader.h"
#include "ringtrack_config.h"
#include "ringtrack_track_selection.h"
#include <fstream>
#include <sstream>
#include <array>
#include <unordered_set>

int n_rejected = 0;

// ===========================================================================
//  ringtrack_analysis  —  track-only, single loop
//  Ring fitting is handled separately by ringtrack_fit
// ===========================================================================
void ringtrack_analysis(std::string data_repository, std::string run_name,
                        std::string conf_path = "ringtrack.conf",
                        std::string output_dir = "")
{
    // -------------------------------------------------------------------------
    //  Load config
    // -------------------------------------------------------------------------
    RingtrackConfig cfg;
    cfg.load(conf_path);
    cfg.print();

    if (output_dir.empty())
        output_dir = data_repository + "/plots/" + run_name;

    const int   first_event = cfg.get_int("first_event", 0);
    const int   max_frames_ = cfg.get_int("max_frames",  1000000);

    // frame list opzionale
    const std::string frame_list_file = cfg.get_string("frame_list_file", "");
    std::unordered_set<int> allowed_frames;
    bool use_frame_list = false;
    if (!frame_list_file.empty())
    {
        std::ifstream fl(frame_list_file);
        if (fl.is_open())
        {
            std::string line;
            std::getline(fl, line); // skip header
            while (std::getline(fl, line))
            {
                if (line.empty()) continue;
                std::istringstream ss(line);
                int frame_id; ss >> frame_id;
                allowed_frames.insert(frame_id);
            }
            fl.close();
            use_frame_list = true;
            std::cout << "[INFO] Loaded " << allowed_frames.size()
                      << " frames from " << frame_list_file << std::endl;
        }
        else
            std::cerr << "[WARNING] Cannot open frame_list_file: " << frame_list_file << std::endl;
    }

    // -------------------------------------------------------------------------
    //  Track selection from conf
    // -------------------------------------------------------------------------
    TrackSelectionConfig tsel;
    tsel.load(cfg);

    // Stampa riepilogo selezione molteplicità
    if (tsel.require_exact_ntracks > 0)
        std::cout << "[INFO] Track multiplicity: exact = " << tsel.require_exact_ntracks << std::endl;
    else if (tsel.require_min_ntracks > 0 || tsel.require_max_ntracks > 0)
        std::cout << "[INFO] Track multiplicity: min = " << tsel.require_min_ntracks
                  << "  max = " << (tsel.require_max_ntracks > 0 ? std::to_string(tsel.require_max_ntracks) : "inf")
                  << std::endl;
    else if (tsel.require_single_track)
        std::cout << "[INFO] Track multiplicity: single track only" << std::endl;
    else if (tsel.require_multi_track)
        std::cout << "[INFO] Track multiplicity: multi track (>= 2)" << std::endl;

    const bool  apply_afterpulse_cut = cfg.get_bool("apply_afterpulse_cut", true);
    const bool  apply_theta_phi_cut  = cfg.get_bool("apply_theta_phi_cut",  false);
    const bool  apply_angle_xy_cut   = cfg.get_bool("apply_angle_xy_cut",   false);
    const bool  apply_window_selection  = cfg.get_bool("apply_window_selection",  false);
    const float window_sel_min          = cfg.get_float("window_sel_min",         -10.f);
    const float window_sel_max          = cfg.get_float("window_sel_max",          10.f);
    const int   window_sel_threshold    = cfg.get_int("window_sel_threshold",      5);
    const bool  apply_window_veto       = cfg.get_bool("apply_window_veto",        false);
    const float window_veto_min         = cfg.get_float("window_veto_min",        -65.f);
    const float window_veto_max         = cfg.get_float("window_veto_max",        -25.f);
    const int   window_veto_threshold   = cfg.get_int("window_veto_threshold",     3);
    const bool  apply_window_veto_2     = cfg.get_bool("apply_window_veto_2",      false);
    const float window_veto_2_min       = cfg.get_float("window_veto_2_min",       80.f);
    const float window_veto_2_max       = cfg.get_float("window_veto_2_max",      120.f);
    const int   window_veto_2_threshold = cfg.get_int("window_veto_2_threshold",   3);
    const bool  require_all_tracks_pass_geometric_cuts =
                    cfg.get_bool("require_all_tracks_pass_geometric_cuts", false);

    const float theta_min   = cfg.get_float("theta_min",    0.f);
    const float theta_max   = cfg.get_float("theta_max",    0.0001f);
    const float phi_min     = cfg.get_float("phi_min",     -3.f);
    const float phi_max     = cfg.get_float("phi_max",      3.f);
    const float angle_x_min = cfg.get_float("angle_x_min", -0.05f);
    const float angle_x_max = cfg.get_float("angle_x_max",  0.05f);
    const float angle_y_min = cfg.get_float("angle_y_min", -0.05f);
    const float angle_y_max = cfg.get_float("angle_y_max",  0.05f);

    const std::array<float,2> time_cut = {
        cfg.get_float("time_cut_min", -50.f),
        cfg.get_float("time_cut_max",  20.f)
    };

    const int         n_display_events       = cfg.get_int("n_display_events", 100000);
    const std::string display_nhits_cut_mode = cfg.get_string("display_nhits_cut_mode", "none");
    const int         display_nhits_min      = cfg.get_int("display_nhits_min", 0);
    const int         display_nhits_max      = cfg.get_int("display_nhits_max", 8);
    const bool        display_all_tracks     = tsel.require_multi_track;

    if (tsel.require_single_track && tsel.require_multi_track)
        std::cerr << "[WARNING] require_single_track e require_multi_track entrambi true\n";

    // -------------------------------------------------------------------------
    //  Open input
    // -------------------------------------------------------------------------
    std::string input_filename = data_repository + "/" + run_name + "/recotrackdata.root";
    TFile *input_file = new TFile(input_filename.c_str());
    if (!input_file || input_file->IsZombie())
    {
        std::cerr << "[ERROR] Could not open " << input_filename << std::endl;
        return;
    }
    TTree *tree = (TTree *)input_file->Get("recotrackdata");
    alcor_recotrackdata *recotrackdata = new alcor_recotrackdata();
    recotrackdata->link_to_tree(tree);

    const long long n_frames   = tree->GetEntries();
    const int       all_frames = (int)min((long long)first_event + max_frames_, n_frames);

    // =========================================================================
    //  Histograms
    // =========================================================================
    TH1F *h_t_distribution = new TH1F("h_t_distribution",
        "Hit time wrt trigger;t_{hit} - t_{timing} (ns);counts", 200, -312.5, 312.5);
    TH1F *h_t_distribution_track = new TH1F("h_t_distribution_track",
        "Hit time wrt trigger (selected events);t_{hit} - t_{timing} (ns);counts", 200, -312.5, 312.5);
    TH1F *h_t_distribution_selected = new TH1F("h_t_distribution_selected",
        "Hit time in time window (selected events);t_{hit} - t_{timing} (ns);counts", 200, -312.5, 312.5);
    TH1F *h_all_n_hits = new TH1F("h_all_n_hits",
        "N hits in time window (all events);n hits;counts", 100, 0, 100);
    TH1F *h_n_selected_hits = new TH1F("h_n_selected_hits",
        "N hits in time window (selected events);n hits;counts", 100, 0, 100);

    TH1F *h_tracking_theta   = new TH1F("h_tracking_theta",
        "Track polar angle;#theta (rad);counts", 1000, 0, 0.1);
    TH1F *h_tracking_phi     = new TH1F("h_tracking_phi",
        "Track azimuthal angle;#phi (rad);counts", 1000, -3.1415, +3.1415);
    TH1F *h_tracking_angle_x = new TH1F("h_tracking_angle_x",
        "Track angle X;#alpha_{x} (rad);counts", 1000, -0.1, 0.1);
    TH1F *h_tracking_angle_y = new TH1F("h_tracking_angle_y",
        "Track angle Y;#alpha_{y} (rad);counts", 1000, -0.1, 0.1);

    TH2F *h_intercept_drich = new TH2F("h_intercept_drich",
        "Track intercept at dRICH;x (mm);y (mm)", 500, -200., +200., 200, -200., +200.);
    TH2F *h_intercept_scint = new TH2F("h_intercept_scint",
        "Track intercept at scintillator;x (mm);y (mm)", 500, -200., +200., 200, -200., +200.);

    TH1F *h_event_multiplicity = new TH1F("h_event_multiplicity",
        "Track multiplicity per event;n tracks;n events", 10, -0.5, 9.5);
    TH1F *h_track_multiplicity = new TH1F("h_track_multiplicity",
        "Track multiplicity;n tracks;n events", 10, -0.5, 9.5);

    TH2F *h_n_selected_hits_vs_multiplicity = new TH2F("h_n_selected_hits_vs_multiplicity",
        "N hits vs track multiplicity;n tracks;n hits", 10, -0.5, 9.5, 100, -0.5, 99.5);
    TH2F *h_n_selected_hits_vs_theta = new TH2F("h_n_selected_hits_vs_theta",
        "N hits vs track #theta;#theta (rad);n hits", 2000, 0, 0.1, 100, -0.5, 99.5);
    TH2F *h_n_selected_hits_vs_ix_drich = new TH2F("h_n_selected_hits_vs_ix_drich",
        "N hits vs intercept X at dRICH;x_{dRICH} (mm);n hits", 120, -200., 200., 100, -0.5, 99.5);
    TH2F *h_n_selected_hits_vs_iy_drich = new TH2F("h_n_selected_hits_vs_iy_drich",
        "N hits vs intercept Y at dRICH;y_{dRICH} (mm);n hits", 120, -200., 200., 100, -0.5, 99.5);

    TH2F *h_hit_xy_map = new TH2F("h_hit_xy_map",
        "Hit map (selected events);x (mm);y (mm)", 396, -99, 99, 396, -99, 99);

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

    int n_frames_no_trigger   = 0;
    int n_frames_single_track = 0;
    int n_frames_multi_track  = 0;
    int n_frames_no_track     = 0;
    int n_frames_total        = 0;

    // output intercepts
    gSystem->mkdir(output_dir.c_str(), true);
    std::ofstream intercept_file(output_dir + "/intercepts.txt");
    intercept_file << "frame\tplane_x\tplane_y\tslope_x\tslope_y\tix_drich\tiy_drich\n";

    // =========================================================================
    //  Single loop
    // =========================================================================
    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 1000 == 0) bar.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;
        if (use_frame_list && allowed_frames.find(i_frame) == allowed_frames.end()) continue;

        auto trigger = recotrackdata->get_trigger_by_index(0);
        if (!trigger) { ++n_frames_no_trigger; continue; }

        ++n_frames_total;

        const int n_trk = recotrackdata->n_recotrackdata();
        if      (n_trk == 0) ++n_frames_no_track;
        else if (n_trk == 1) ++n_frames_single_track;
        else                 ++n_frames_multi_track;

        h_track_multiplicity->Fill(n_trk);

        for (int i = 0; i < n_trk; i++)
        {
            h_tracking_theta->Fill(recotrackdata->get_traj_angcoeff_theta(i));
            h_tracking_phi->Fill(recotrackdata->get_traj_angcoeff_phi(i));
            h_tracking_angle_x->Fill(atan(recotrackdata->get_traj_angcoeff_x(i)));
            h_tracking_angle_y->Fill(atan(recotrackdata->get_traj_angcoeff_y(i)));
        }
        h_event_multiplicity->Fill(n_trk);

        // t_distribution e n_hits per tutti gli eventi
        int n_hits_all = 0;
        for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
        {
            if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
            float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
            h_t_distribution->Fill(dt);
            if (dt >= time_cut[0] && dt <= time_cut[1]) ++n_hits_all;
        }
        h_all_n_hits->Fill(n_hits_all);

        // ---- window selection / veto ----
        if (apply_window_selection || apply_window_veto || apply_window_veto_2)
        {
            int n_sel = 0, n_veto = 0, n_veto2 = 0;
            for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
            {
                if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
                float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
                if (apply_window_selection && dt >= window_sel_min  && dt <= window_sel_max)  ++n_sel;
                if (apply_window_veto      && dt >= window_veto_min && dt <= window_veto_max) ++n_veto;
                if (apply_window_veto_2    && dt >= window_veto_2_min && dt <= window_veto_2_max) ++n_veto2;
            }
            if (apply_window_selection && n_sel   <  window_sel_threshold)    continue;
            if (apply_window_veto      && n_veto  >= window_veto_threshold)   continue;
            if (apply_window_veto_2    && n_veto2 >= window_veto_2_threshold) continue;
        }

        // ---- molteplicità ----
        if (!tsel.passes_multiplicity(n_trk)) continue;

        // ---- require_all_tracks_pass ----
        if (require_all_tracks_pass_geometric_cuts && n_trk > 1)
        {
            bool all_pass = true;
            for (int i = 0; i < n_trk; i++)
            {
                if (!is_track_selected(
                        recotrackdata->get_det_plane_x(i), recotrackdata->get_det_plane_y(i),
                        recotrackdata->get_traj_angcoeff_x(i), recotrackdata->get_traj_angcoeff_y(i),
                        tsel.cutg1, tsel.plane1, tsel.side1,
                        tsel.cutg2, tsel.plane2, tsel.side2,
                        tsel.z_drich, tsel.z_scint))
                { all_pass = false; break; }
            }
            if (!all_pass) { n_rejected++; continue; }
        }

        // ---- selezione traccia (best o loop) ----
        std::vector<int> tracks_to_process;
        if (tsel.use_best_track_only)
        {
            int best = 0;
            float best_chi2 = recotrackdata->get_chi2ndof(0);
            for (int i = 1; i < n_trk; i++)
            {
                float c = recotrackdata->get_chi2ndof(i);
                if (c < best_chi2) { best_chi2 = c; best = i; }
            }
            if (tsel.apply_chi2_cut && best_chi2 > tsel.chi2_max) { n_rejected++; continue; }
            if (!is_track_selected(
                    recotrackdata->get_det_plane_x(best), recotrackdata->get_det_plane_y(best),
                    recotrackdata->get_traj_angcoeff_x(best), recotrackdata->get_traj_angcoeff_y(best),
                    tsel.cutg1, tsel.plane1, tsel.side1,
                    tsel.cutg2, tsel.plane2, tsel.side2,
                    tsel.z_drich, tsel.z_scint))
            { n_rejected++; continue; }
            tracks_to_process.push_back(best);
        }
        else
        {
            for (int i = 0; i < n_trk; i++)
            {
                if (tsel.apply_chi2_cut && recotrackdata->get_chi2ndof(i) > tsel.chi2_max) continue;
                if (!is_track_selected(
                        recotrackdata->get_det_plane_x(i), recotrackdata->get_det_plane_y(i),
                        recotrackdata->get_traj_angcoeff_x(i), recotrackdata->get_traj_angcoeff_y(i),
                        tsel.cutg1, tsel.plane1, tsel.side1,
                        tsel.cutg2, tsel.plane2, tsel.side2,
                        tsel.z_drich, tsel.z_scint)) continue;
                tracks_to_process.push_back(i);
            }
            if (tracks_to_process.empty()) { n_rejected++; continue; }
        }

        // intercette di tutte le tracce per display multi
        std::vector<std::pair<float,float>> all_track_intercepts;
        if (display_all_tracks)
        {
            for (int i = 0; i < n_trk; i++)
            {
                float px = recotrackdata->get_det_plane_x(i);
                float py = recotrackdata->get_det_plane_y(i);
                float sx = recotrackdata->get_traj_angcoeff_x(i);
                float sy = recotrackdata->get_traj_angcoeff_y(i);
                all_track_intercepts.push_back({px + sx * tsel.z_drich, py + sy * tsel.z_drich});
            }
        }

        // ---- hit loop ----
        int n_hits = 0;
        for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
        {
            if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
            float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
            h_t_distribution_selected->Fill(dt);
            if (dt < time_cut[0] || dt > time_cut[1]) continue;
            h_hit_xy_map->Fill(recotrackdata->get_hit_x_rnd(i_hit), recotrackdata->get_hit_y_rnd(i_hit));
            ++n_hits;
        }
        h_n_selected_hits->Fill(n_hits);

        // ---- loop tracce ----
        bool first_track = true;
        for (int idx : tracks_to_process)
        {
            const float px      = recotrackdata->get_det_plane_x(idx);
            const float py      = recotrackdata->get_det_plane_y(idx);
            const float sx      = recotrackdata->get_traj_angcoeff_x(idx);
            const float sy      = recotrackdata->get_traj_angcoeff_y(idx);
            const float ix_drich = px + sx * tsel.z_drich;
            const float iy_drich = py + sy * tsel.z_drich;
            const float ix_scint = px + sx * tsel.z_scint;
            const float iy_scint = py + sy * tsel.z_scint;
            const float theta    = recotrackdata->get_traj_angcoeff_theta(idx);

            if (apply_theta_phi_cut)
            {
                float phi = recotrackdata->get_traj_angcoeff_phi(idx);
                if (theta < theta_min || theta > theta_max) continue;
                if (phi   < phi_min   || phi   > phi_max)   continue;
            }
            if (apply_angle_xy_cut)
            {
                float ax = atan(sx), ay = atan(sy);
                if (ax < angle_x_min || ax > angle_x_max) continue;
                if (ay < angle_y_min || ay > angle_y_max) continue;
            }

            h_intercept_drich->Fill(ix_drich, iy_drich);
            h_intercept_scint->Fill(ix_scint, iy_scint);
            h_n_selected_hits_vs_multiplicity->Fill(n_trk, n_hits);
            h_n_selected_hits_vs_theta->Fill(theta, n_hits);
            h_n_selected_hits_vs_ix_drich->Fill(ix_drich, n_hits);
            h_n_selected_hits_vs_iy_drich->Fill(iy_drich, n_hits);

            intercept_file << i_frame << "\t"
                           << px << "\t" << py << "\t"
                           << sx << "\t" << sy << "\t"
                           << ix_drich << "\t" << iy_drich << "\n";

            // hit time distribution per eventi con traccia selezionata
            if (first_track)
            {
                for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
                {
                    if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
                    h_t_distribution_track->Fill(
                        recotrackdata->get_hit_t(i_hit) - trigger->fine_time);
                }
            }

            // display
            if (first_track && n_display_filled < n_display_events)
            {
                bool pass_nhits = true;
                if (display_nhits_cut_mode == "greater")
                    pass_nhits = (n_hits > display_nhits_min);
                else if (display_nhits_cut_mode == "range")
                    pass_nhits = (n_hits >= display_nhits_min && n_hits <= display_nhits_max);

                if (n_trk == 1)
                    g_display_intercepts_single->AddPoint(ix_drich, iy_drich);
                else if (display_all_tracks)
                    for (auto &p : all_track_intercepts)
                        g_display_intercepts_multi->AddPoint(p.first, p.second);
                else
                    g_display_intercepts_multi->AddPoint(ix_drich, iy_drich);

                if (pass_nhits)
                {
                    for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
                    {
                        if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
                        float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
                        if (dt < time_cut[0] || dt > time_cut[1]) continue;
                        h_display_hits->Fill(recotrackdata->get_hit_x_rnd(i_hit),
                                             recotrackdata->get_hit_y_rnd(i_hit));
                    }
                }
                n_display_filled++;
            }

            first_track = false;
        }
    }
    bar.update(all_frames, all_frames);
    bar.finish();
    intercept_file.close();

    // =========================================================================
    //  Save
    // =========================================================================
    std::string output_root = output_dir + "/histograms.root";
    TFile *output_file = new TFile(output_root.c_str(), "RECREATE");

    for (auto &kv : cfg._data)
        TNamed(kv.first.c_str(), kv.second.c_str()).Write();
    TNamed("run_name", run_name.c_str()).Write();

    if (tsel.cutg2 && tsel.plane2 != CutPlane::NONE)
        tsel.cutg2->Write("cutg2_display");

    h_t_distribution->Write();
    h_t_distribution_track->Write();
    h_t_distribution_selected->Write();
    h_all_n_hits->Write();
    h_n_selected_hits->Write();
    h_tracking_theta->Write();
    h_tracking_phi->Write();
    h_tracking_angle_x->Write();
    h_tracking_angle_y->Write();
    h_intercept_drich->Write();
    h_intercept_scint->Write();
    h_event_multiplicity->Write();
    h_track_multiplicity->Write();
    h_n_selected_hits_vs_multiplicity->Write();
    h_n_selected_hits_vs_theta->Write();
    h_n_selected_hits_vs_ix_drich->Write();
    h_n_selected_hits_vs_iy_drich->Write();
    h_hit_xy_map->Write();
    h_display_hits->Write();
    g_display_intercepts_single->Write();
    g_display_intercepts_multi->Write();

    output_file->Close();

    std::cout << "Output: " << output_root << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Frame statistics:" << std::endl;
    std::cout << "  Total:         " << n_frames_total        << std::endl;
    std::cout << "  No trigger:    " << n_frames_no_trigger   << std::endl;
    std::cout << "  No track:      " << n_frames_no_track     << std::endl;
    std::cout << "  Single track:  " << n_frames_single_track << std::endl;
    std::cout << "  Multi track:   " << n_frames_multi_track  << std::endl;
    std::cout << "  Rejected:      " << n_rejected            << std::endl;
    std::cout << "========================================" << std::endl;
}