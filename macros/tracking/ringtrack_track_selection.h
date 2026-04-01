#pragma once
#include "TCutG.h"
#include "ringtrack_config.h"
#include <string>
#include <sstream>

// ===========================================================================
//  Shared track-selection helpers
//  Used by: ringtrack_analysis.cpp, ringtrack_mult_windows.cpp
// ===========================================================================

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
    TString name = TString::Format("cutg_rect_%.0f_%.0f_%.0f_%.0f", xmin, xmax, ymin, ymax);
    TCutG *cutg = new TCutG(name.Data(), 5);
    cutg->SetPoint(0, xmin, ymin);
    cutg->SetPoint(1, xmax, ymin);
    cutg->SetPoint(2, xmax, ymax);
    cutg->SetPoint(3, xmin, ymax);
    cutg->SetPoint(4, xmin, ymin);
    cutg->SetVarX("x"); cutg->SetVarY("y");
    cutg->SetTitle(TString::Format("rect_%.0f_%.0f_%.0f_%.0f", xmin, xmax, ymin, ymax).Data());
    return cutg;
}

// ---------------------------------------------------------------------------
//  Parsers
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
//  Track geometric selection
// ---------------------------------------------------------------------------
bool is_track_selected(float plane_x, float plane_y, float slope_x, float slope_y,
                       TCutG *cutg1, CutPlane plane1, CutSide side1,
                       TCutG *cutg2, CutPlane plane2, CutSide side2,
                       float z_drich, float z_scint)
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

// ---------------------------------------------------------------------------
//  Load geometric selection settings from config
// ---------------------------------------------------------------------------
struct TrackSelectionConfig
{
    CutPlane plane1 = CutPlane::NONE;
    CutSide  side1  = CutSide::INSIDE;
    TCutG   *cutg1  = nullptr;
    CutPlane plane2 = CutPlane::NONE;
    CutSide  side2  = CutSide::OUTSIDE;
    TCutG   *cutg2  = nullptr;
    float    z_drich = -4250.f;
    float    z_scint = -1150.f;

    // --- multiplicity ---
    // Legacy params require_single_track / require_multi_track are
    // kept for backward compatibility with existing conf files.
    // I nuovi parametri require_exact/min/max_ntracks hanno precedenza se > 0.
    bool     require_single_track  = false;
    bool     require_multi_track   = false;
    int      require_exact_ntracks = 0;   // 0 = disabilitato
    int      require_min_ntracks   = 0;   // 0 = nessun minimo aggiuntivo
    int      require_max_ntracks   = 0;   // 0 = nessun massimo

    bool     apply_chi2_cut        = false;
    float    chi2_max              = 5.f;
    bool     use_best_track_only   = true;

    void load(const RingtrackConfig &cfg)
    {
        plane1 = parse_plane(cfg.get_string("plane1", "NONE"));
        side1  = parse_side (cfg.get_string("side1",  "INSIDE"));
        cutg1  = parse_cutg (cfg.get_string("cutg1",  "scint_largo"));
        plane2 = parse_plane(cfg.get_string("plane2", "NONE"));
        side2  = parse_side (cfg.get_string("side2",  "OUTSIDE"));
        cutg2  = parse_cutg (cfg.get_string("cutg2",  "rect -45 25 -25 25"));
        z_drich               = cfg.get_float("z_drich",               -4250.f);
        z_scint               = cfg.get_float("z_scint",               -1150.f);
        require_single_track  = cfg.get_bool ("require_single_track",  false);
        require_multi_track   = cfg.get_bool ("require_multi_track",   false);
        require_exact_ntracks = cfg.get_int  ("require_exact_ntracks", 0);
        require_min_ntracks   = cfg.get_int  ("require_min_ntracks",   0);
        require_max_ntracks   = cfg.get_int  ("require_max_ntracks",   0);
        apply_chi2_cut        = cfg.get_bool ("apply_chi2_cut",        false);
        chi2_max              = cfg.get_float("chi2_max",              5.f);
        use_best_track_only   = cfg.get_bool ("use_best_track_only",   true);
    }

    // Check multiplicity constraints only (no geometric cuts).
    bool passes_multiplicity(int n_tracks) const
    {
        if (n_tracks == 0) return false;

        // Nuovi parametri — hanno precedenza se specificati
        if (require_exact_ntracks > 0 && n_tracks != require_exact_ntracks) return false;
        if (require_min_ntracks   > 0 && n_tracks <  require_min_ntracks)   return false;
        if (require_max_ntracks   > 0 && n_tracks >  require_max_ntracks)   return false;

        // Legacy
        if ( require_single_track && !require_multi_track && n_tracks != 1) return false;
        if (!require_single_track &&  require_multi_track && n_tracks <  2) return false;
        if (!require_single_track && !require_multi_track && n_tracks <  1) return false;
        if ( require_single_track &&  require_multi_track && n_tracks <  1) return false;

        return true;
    }

    // Returns true if the event passes multiplicity and at least one track
    // passes the geometric cuts. track_idx_out is the selected track index.
    bool select_event(alcor_recotrackdata *reco, int &track_idx_out) const
    {
        const int n_tracks = reco->n_recotrackdata();

        if (!passes_multiplicity(n_tracks)) return false;

        if (use_best_track_only)
        {
            int best = 0;
            float best_chi2 = reco->get_chi2ndof(0);
            for (int i = 1; i < n_tracks; i++)
            {
                float c = reco->get_chi2ndof(i);
                if (c < best_chi2) { best_chi2 = c; best = i; }
            }
            if (apply_chi2_cut && best_chi2 > chi2_max) return false;
            float px = reco->get_det_plane_x(best);
            float py = reco->get_det_plane_y(best);
            float sx = reco->get_traj_angcoeff_x(best);
            float sy = reco->get_traj_angcoeff_y(best);
            if (!is_track_selected(px, py, sx, sy, cutg1, plane1, side1, cutg2, plane2, side2, z_drich, z_scint))
                return false;
            track_idx_out = best;
            return true;
        }
        else
        {
            for (int i = 0; i < n_tracks; i++)
            {
                float c = reco->get_chi2ndof(i);
                if (apply_chi2_cut && c > chi2_max) continue;
                float px = reco->get_det_plane_x(i);
                float py = reco->get_det_plane_y(i);
                float sx = reco->get_traj_angcoeff_x(i);
                float sy = reco->get_traj_angcoeff_y(i);
                if (is_track_selected(px, py, sx, sy, cutg1, plane1, side1, cutg2, plane2, side2, z_drich, z_scint))
                {
                    track_idx_out = i;
                    return true;
                }
            }
            return false;
        }
    }
};