/**
 * @file macros/utilities/ransac_tune.cpp
 * @brief `ransac_tune` — fast standalone harness for tuning the streaming
 *        RANSAC ring-finder scan WITHOUT re-running the (slow) lightdata stage.
 *
 * Reads the Cherenkov hits already in a `lightdata.root`, rebuilds the per-frame
 * ring-candidate set exactly as `run_streaming_ransac_trigger` does (non-
 * afterpulse hits within `--time-window` of a ring-seeding trigger), and re-runs
 * `mist::ring_finding::find_rings_ransac` with CLI-tunable scan parameters.  It
 * reports the ring yield (fraction of frames with ≥1 and with 2 rings) and the
 * centre/radius distributions, and writes a small ROOT file with the centre-XY
 * and R hists for rendering.  Iterate on the parameters in seconds.
 *
 * Occupancy weights (1/DCR proxy) are derived here from the per-channel hit
 * rate across the processed subset — high-rate (noisy) channels are down-
 * weighted, mirroring the score stage's `weight_by_channel = 1/m_c`.
 *
 * Usage:
 *   ransac_tune <lightdata.root> [--max-frames N] [--time-window ns]
 *     [--iter N] [--min-sig F] [--min-inliers N] [--band F]
 *     [--rmin F] [--rmax F] [--visfrac F] [--sensor F] [--max-rings N]
 *     [--weights 0|1] [--out file.root]
 */

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "TFile.h"
#include "TH1F.h"
#include "TH2F.h"
#include "TTree.h"

#include <mist/ring_finding/ransac_ring_finder.h>

#include "alcor_data.h"
#include "alcor_finedata.h"
#include "alcor_lightdata.h"
#include "alcor_spilldata.h"
#include "triggers/events.h"
#include "utility/global_index.h"

namespace
{
//  Same predicate as run_streaming_ransac_trigger's is_ring_seed_trigger:
//  config hardware [0,99], built-in TIMING, and the streaming self-trigger.
bool is_ring_seed_trigger(int index)
{
    return index < static_cast<int>(TriggerFirstFrames) ||
           index == static_cast<int>(TriggerTiming) ||
           index == static_cast<int>(_TRIGGER_STREAMING_RING_FOUND_);
}

struct FrameHits
{
    std::vector<float> x, y, t;
    std::vector<int> ch;
};
} // namespace

int main(int argc, char **argv)
{
    CLI::App app{"ransac_tune — tune the streaming RANSAC ring scan on lightdata"};

    std::string file_path, out_path;
    long max_frames = -1;       // -1 → all
    double time_window = 100.0; // ns; mirror the score-stage time_window_ns
    int iterations = 4000;
    double min_sig = 3.0;
    int min_inliers = 6;
    double band = 6.0;
    double r_min = 50.0, r_max = 1000.0;
    double vis_frac = 0.10;
    double sensor = 99.0;
    int max_rings = 2;
    bool use_weights = true;
    bool ignore_triggers = false; // run on ALL frame cherenkov hits (no seed)
    bool auto_window = false;     // isolate the densest time_window per frame
    bool dt_scan = false;         // diagnostic: histogram t_hit - nearest seed time

    app.add_option("file", file_path, "Path to lightdata.root")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--out", out_path, "Output ROOT file for hists (optional)");
    app.add_option("--max-frames", max_frames, "Frames to process (-1 = all)");
    app.add_option("--time-window", time_window, "±time pre-cut [ns] around seed");
    app.add_option("--iter", iterations, "RANSAC iterations");
    app.add_option("--min-sig", min_sig, "min_significance");
    app.add_option("--min-inliers", min_inliers, "min_inliers");
    app.add_option("--band", band, "inlier_band [mm]");
    app.add_option("--rmin", r_min, "r_min [mm]");
    app.add_option("--rmax", r_max, "r_max [mm]");
    app.add_option("--visfrac", vis_frac, "min_visible_arc_frac");
    app.add_option("--sensor", sensor, "sensor half-extent [mm] (fiducial)");
    app.add_option("--max-rings", max_rings, "max rings per frame");
    app.add_option("--weights", use_weights, "use 1/rate occupancy weights (0/1)");
    app.add_flag("--ignore-triggers", ignore_triggers,
                 "run the finder on the frame's cherenkov hits directly, ignoring "
                 "the ring-seed trigger (frame = event); use when the score/trigger "
                 "seeding produced nothing");
    app.add_flag("--dt-scan", dt_scan,
                 "diagnostic: histogram (t_hit - nearest ring-seed trigger time) "
                 "over a wide range and report the peak, to find the trigger->"
                 "Cherenkov time offset; skips the finder");
    app.add_flag("--auto-window", auto_window,
                 "with --ignore-triggers: isolate the densest ±time-window time "
                 "cluster per frame (a trigger-free stand-in for the score-stage "
                 "time reference) instead of using every hit — essential on noisy "
                 "runs where the ring is buried in DCR");

    CLI11_PARSE(app, argc, argv);

    std::unique_ptr<TFile> f(TFile::Open(file_path.c_str(), "READ"));
    if (!f || f->IsZombie())
    {
        std::fprintf(stderr, "cannot open %s\n", file_path.c_str());
        return 1;
    }
    auto *tree = f->Get<TTree>("lightdata");
    if (!tree)
    {
        std::fprintf(stderr, "no 'lightdata' tree in %s\n", file_path.c_str());
        return 1;
    }
    AlcorSpilldata spilldata;
    spilldata.link_to_tree(tree);
    const long n_spills = tree->GetEntries();

    //  ── Collect per-frame candidate hit sets + per-channel occupancy ──────────
    std::vector<FrameHits> frames;
    std::unordered_map<int, long> ch_count;
    TH1F h_dt("h_dt", "t_hit - nearest seed trigger;#Deltat [ns];hits", 800, -2000, 2000);
    std::unordered_map<int, long> g_trg_count; // dt-scan: census of trigger indices
    std::unordered_map<int, long> g_unk_dev;   // dt-scan: device of UNKNOWN(255) triggers
    long n_frames_seen = 0;
    bool reached_cap = false;
    //  --max-frames caps the number of CANDIDATE frames collected (the ones the
    //  finder actually runs on), so the harness stays fast on huge runs.
    for (long is = 0; is < n_spills && !reached_cap; ++is)
    {
        tree->GetEntry(is);
        spilldata.get_entry();
        for (auto &fs : spilldata.get_frame_list_link())
        {
            if (reached_cap)
                break;
            AlcorLightdata ld(fs);

            //  Ring-seeding trigger times in this frame (skipped in
            //  --ignore-triggers mode, where the frame itself is the event).
            std::vector<float> seed_times;
            if (!ignore_triggers)
            {
                for (const auto &trg : ld.get_triggers())
                {
                    if (dt_scan)
                    {
                        ++g_trg_count[trg.index]; // census of ALL trigger indices
                        if (trg.index == 255)     // _TRIGGER_UNKNOWN_: coarse=device
                            ++g_unk_dev[trg.coarse];
                    }
                    if (is_ring_seed_trigger(trg.index))
                        seed_times.push_back(trg.fine_time);
                }
                if (seed_times.empty() && !dt_scan)
                    continue;
            }

            FrameHits fh;
            for (const auto &hs : ld.get_cherenkov_hits_link())
            {
                AlcorFinedata h(hs);
                if (h.is_afterpulse())
                    continue;
                if (dt_scan)
                {
                    //  Diagnostic only: signed Δt to the nearest seed trigger.
                    if (!seed_times.empty())
                    {
                        float best = 1e30f;
                        for (const float ts : seed_times)
                        {
                            const float d = h.get_time_ns() - ts;
                            if (std::fabs(d) < std::fabs(best))
                                best = d;
                        }
                        h_dt.Fill(best);
                    }
                    continue; // never collect/find in dt-scan mode
                }
                //  Within ±time_window of any seed trigger (unless ignoring
                //  triggers, in which case every non-afterpulse hit qualifies).
                if (!ignore_triggers)
                {
                    bool in_win = false;
                    for (const float ts : seed_times)
                        if (std::fabs(h.get_time_ns() - ts) < time_window)
                        {
                            in_win = true;
                            break;
                        }
                    if (!in_win)
                        continue;
                }
                const int ch = ::GlobalIndex(h.get_global_index()).channel_ordinal();
                fh.x.push_back(h.get_hit_x());
                fh.y.push_back(h.get_hit_y());
                fh.t.push_back(h.get_time_ns());
                fh.ch.push_back(ch);
            }
            ++n_frames_seen;

            //  Densest-window isolation: a trigger-free stand-in for the score
            //  stage's time reference.  Slide a window of width 2·time_window
            //  over the frame's hits (sorted by time) and keep only the hits in
            //  the densest position — the ring photons arrive in a tight time
            //  cluster while DCR is spread across the whole frame.
            if (ignore_triggers && auto_window &&
                static_cast<int>(fh.x.size()) > min_inliers)
            {
                std::vector<int> order(fh.t.size());
                for (size_t i = 0; i < order.size(); ++i)
                    order[i] = static_cast<int>(i);
                std::sort(order.begin(), order.end(),
                          [&fh](int a, int b)
                          { return fh.t[a] < fh.t[b]; });
                const double w = 2.0 * time_window;
                int best_lo = 0, best_cnt = 0, lo = 0;
                for (int hi = 0; hi < static_cast<int>(order.size()); ++hi)
                {
                    while (fh.t[order[hi]] - fh.t[order[lo]] > w)
                        ++lo;
                    if (hi - lo + 1 > best_cnt)
                    {
                        best_cnt = hi - lo + 1;
                        best_lo = lo;
                    }
                }
                FrameHits win;
                const float t0 = fh.t[order[best_lo]];
                for (int k : order)
                    if (fh.t[k] - t0 <= w && fh.t[k] >= t0)
                    {
                        win.x.push_back(fh.x[k]);
                        win.y.push_back(fh.y[k]);
                        win.t.push_back(fh.t[k]);
                        win.ch.push_back(fh.ch[k]);
                    }
                fh = std::move(win);
            }

            if (static_cast<int>(fh.x.size()) >= min_inliers)
            {
                for (int ch : fh.ch)
                    ++ch_count[ch];
                frames.push_back(std::move(fh));
                if (max_frames >= 0 && static_cast<long>(frames.size()) >= max_frames)
                    reached_cap = true;
            }
        }
    }

    if (dt_scan)
    {
        const int pk = h_dt.GetMaximumBin();
        std::printf("\n=== dt-scan %s ===\n", file_path.c_str());
        std::printf("frames with seed=%ld, hits filled=%.0f\n", n_frames_seen,
                    h_dt.GetEntries());
        std::printf("Δt(hit - nearest seed): peak bin=%.0f ns, mean=%.0f ns, "
                    "rms=%.0f ns\n",
                    h_dt.GetXaxis()->GetBinCenter(pk), h_dt.GetMean(), h_dt.GetRMS());
        //  Fraction within ±20 ns of the seed (what the current window captures).
        const int b0 = h_dt.FindBin(-20.0), b1 = h_dt.FindBin(20.0);
        const double in20 = h_dt.Integral(b0, b1);
        std::printf("within ±20 ns: %.0f (%.1f%% of hits)\n", in20,
                    h_dt.GetEntries() > 0 ? 100.0 * in20 / h_dt.GetEntries() : 0.0);
        std::printf("trigger-index census in per-frame trigger_hits:\n");
        for (const auto &[idx, c] : g_trg_count)
            std::printf("  index %d : %ld  (ring-seed=%s)\n", idx, c,
                        is_ring_seed_trigger(idx) ? "yes" : "NO");
        if (!g_unk_dev.empty())
        {
            std::printf("UNKNOWN(255) trigger words by device:\n");
            for (const auto &[dev, c] : g_unk_dev)
                std::printf("  device %d : %ld\n", dev, c);
        }
        if (!out_path.empty())
        {
            std::unique_ptr<TFile> of(TFile::Open(out_path.c_str(), "RECREATE"));
            h_dt.Write();
        }
        return 0;
    }

    //  Per-channel weight = 1/rate, normalised to mean 1 over present channels.
    std::unordered_map<int, float> ch_weight;
    if (use_weights)
    {
        double sum = 0.0;
        for (const auto &[ch, c] : ch_count)
        {
            const float w = 1.f / static_cast<float>(c);
            ch_weight[ch] = w;
            sum += w;
        }
        if (!ch_weight.empty() && sum > 0.0)
        {
            const float inv_mean = static_cast<float>(ch_weight.size() / sum);
            for (auto &[ch, w] : ch_weight)
                w *= inv_mean;
        }
    }

    //  ── Run the finder per frame with the requested scan ──────────────────────
    mist::ring_finding::RansacOptions opt;
    opt.max_rings = max_rings;
    opt.iterations = iterations;
    opt.inlier_band = band;
    opt.min_inliers = min_inliers;
    opt.min_significance = min_sig;
    opt.r_min = r_min;
    opt.r_max = r_max;
    opt.min_visible_arc_frac = vis_frac;
    opt.fiducial_xmin = -sensor;
    opt.fiducial_xmax = sensor;
    opt.fiducial_ymin = -sensor;
    opt.fiducial_ymax = sensor;

    TH2F h_cxy1("h_cxy1", "ring1 centre XY;X [mm];Y [mm]", 200, -600, 400, 200, -500, 500);
    TH2F h_cxy2("h_cxy2", "ring2 centre XY;X [mm];Y [mm]", 200, -600, 400, 200, -500, 500);
    TH1F h_R1("h_R1", "ring1 R;R [mm];rings", 120, 0, 700);
    TH1F h_R2("h_R2", "ring2 R;R [mm];rings", 120, 0, 700);

    long n_with1 = 0, n_with2 = 0, n_rings = 0;
    double sx1 = 0, sx1sq = 0, sR1 = 0, sR1sq = 0;
    long nr1 = 0;
    for (const auto &fh : frames)
    {
        std::vector<mist::ring_finding::Hit> hits;
        std::vector<float> w;
        hits.reserve(fh.x.size());
        w.reserve(fh.x.size());
        for (size_t i = 0; i < fh.x.size(); ++i)
        {
            hits.push_back({fh.x[i], fh.y[i], fh.t[i], 0});
            if (use_weights)
            {
                auto it = ch_weight.find(fh.ch[i]);
                w.push_back(it != ch_weight.end() ? it->second : 0.f);
            }
        }
        const auto rings =
            mist::ring_finding::find_rings_ransac(hits, opt, use_weights ? w : std::vector<float>{});
        if (!rings.empty())
            ++n_with1;
        if (rings.size() >= 2)
            ++n_with2;
        n_rings += static_cast<long>(rings.size());
        if (rings.size() >= 1)
        {
            h_cxy1.Fill(rings[0].cx, rings[0].cy);
            h_R1.Fill(rings[0].radius);
            sx1 += rings[0].cx;
            sx1sq += double(rings[0].cx) * rings[0].cx;
            sR1 += rings[0].radius;
            sR1sq += double(rings[0].radius) * rings[0].radius;
            ++nr1;
        }
        if (rings.size() >= 2)
        {
            h_cxy2.Fill(rings[1].cx, rings[1].cy);
            h_R2.Fill(rings[1].radius);
        }
    }

    const long nf = static_cast<long>(frames.size());
    auto frac = [nf](long n)
    { return nf ? 100.0 * n / nf : 0.0; };
    const double mx1 = nr1 ? sx1 / nr1 : 0.0;
    const double rmsx1 = nr1 ? std::sqrt(std::max(0.0, sx1sq / nr1 - mx1 * mx1)) : 0.0;
    const double mR1 = nr1 ? sR1 / nr1 : 0.0;
    const double rmsR1 = nr1 ? std::sqrt(std::max(0.0, sR1sq / nr1 - mR1 * mR1)) : 0.0;

    std::printf("\n=== ransac_tune %s ===\n", file_path.c_str());
    std::printf("scan: iter=%d min_sig=%.2f min_inliers=%d band=%.1f r=[%.0f,%.0f] "
                "visfrac=%.2f sensor=%.0f max_rings=%d weights=%d twin=%.0f\n",
                iterations, min_sig, min_inliers, band, r_min, r_max, vis_frac,
                sensor, max_rings, (int)use_weights, time_window);
    std::printf("frames seen=%ld, candidate frames (>=%d hits)=%ld\n",
                n_frames_seen, min_inliers, nf);
    std::printf("rings: total=%ld | >=1 ring: %ld (%.1f%%) | 2 rings: %ld (%.1f%%)\n",
                n_rings, n_with1, frac(n_with1), n_with2, frac(n_with2));
    std::printf("ring1 centre X: mean=%.1f rms=%.1f | R: mean=%.1f rms=%.1f\n",
                mx1, rmsx1, mR1, rmsR1);

    if (!out_path.empty())
    {
        std::unique_ptr<TFile> of(TFile::Open(out_path.c_str(), "RECREATE"));
        h_cxy1.Write();
        h_cxy2.Write();
        h_R1.Write();
        h_R2.Write();
        std::printf("wrote hists → %s\n", out_path.c_str());
    }
    return 0;
}
