#include "utility.h"
#include "TH1.h"
#include "TH2.h"
#include "TTree.h"
#include "TFile.h"
#include "mapping.h"
#include "math.h"
#include "TProfile.h"
#include "parallel_streaming_framer.h"
#include "alcor_recodata.h"
#include "alcor_spilldata.h"
#include "writers/lightdata.h"
#include "writers/recodata.h"

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill,
    bool force_recodata_rebuild,
    bool force_lightdata_rebuild,
    std::string mapping_conf,
    std::string trigger_conf,
    std::string framer_conf)
{
    //  Framer configuration
    auto framer_cfg = FramerConfReader(framer_conf);

    //  Input file — open lightdata.root, auto-rebuild via lightdata_writer
    //  if missing/corrupt or if force_lightdata_rebuild is set (CODE_REVIEW §D-06).
    //  TFilePtr is owning: closes + deletes on every exit path.
    std::string input_filename = data_repository + "/" + run_name + "/lightdata.root";
    TFilePtr input_file(TFile::Open(input_filename.c_str(), "READ"));
    if (!input_file || input_file->IsZombie() || force_lightdata_rebuild)
    {
        mist::logger::warning("(recodata_writer) " + input_filename +
                              " missing, corrupt, or rebuild forced — running lightdata_writer");
        // Default trailing args (calibration / framer config) preserve the
        // previous helper's hardcoded behaviour.  Callers that need to
        // forward custom config paths should call lightdata_writer directly
        // before recodata_writer rather than relying on this fallback.
        lightdata_writer(data_repository, run_name, max_spill,
                         force_lightdata_rebuild, /*requested_n_threads=*/-1);
        input_file.reset(TFile::Open(input_filename.c_str(), "READ"));
        if (!input_file || input_file->IsZombie())
        {
            mist::logger::error("(recodata_writer) Could not open " + input_filename +
                                " even after rebuild — aborting");
            return;
        }
    }

    //  Link lightdata tree locally — use TFile::Get<TTree> which returns the
    //  correctly typed pointer (cleaner than a C-style cast that can mask a
    //  type mismatch).  Bail out if the branch is missing.
    auto *lightdata_tree = input_file->Get<TTree>("lightdata");
    if (!lightdata_tree)
    {
        mist::logger::error(TString::Format("(recodata_writer) 'lightdata' tree missing in %s",
                                 input_filename.c_str()).Data());
        return;
    }
    AlcorSpilldata *spilldata = new AlcorSpilldata();
    spilldata->link_to_tree(lightdata_tree);

    AlcorFinedata::read_calib_from_file(data_repository + "/" + run_name + "/fine_calib.txt");

    auto fine_time_calib_th2f = input_file->Get<TH2F>("h_fine_calib");
    if (!fine_time_calib_th2f)
    {
        mist::logger::error(TString::Format("(recodata_writer) 'h_fine_calib' histogram missing in %s",
                                 input_filename.c_str()).Data());
        return;
    }
    AlcorFinedata::generate_calibration(fine_time_calib_th2f, true);
    //  Calibration table is now built; signal immutability so per-Hit
    //  AlcorFinedata::get_phase() readers skip the shared_mutex
    //  (CODE_REVIEW §3.1).  No worker threads have spawned yet.
    AlcorFinedata::freeze_calibration();

    //  Progress tracking — multi-bar with one subtask (per-frame post-processing).
    //  Main bar shows overall spill progress; the subtask clock is restarted
    //  at the head of each spill via progress_bars.restart() so it reflects
    //  THIS spill's reconstruction time, not the cumulative total.
    mist::logger::MultiProgressBar progress_bars(mist::logger::BarStyle::Block);
    progress_bars.set_unit("spills");
    auto &post_processing = progress_bars.add_subtask("post-processing");

    //  Generate Mapping
    Mapping current_mapping(mapping_conf);

    //  Build trigger registry from config
    //  The registry maps raw uint8 trigger values to a dense ordered list of
    //  (value, name) pairs — config-defined triggers first, built-in defaults
    //  after. This gives contiguous histogram bins with meaningful labels,
    //  avoiding the 252 empty bins you'd get from a raw 0–255 axis.
    auto trigger_configs = trigger_conf_reader(trigger_conf);
    TriggerRegistry registry(trigger_configs);
    const int n_triggers = registry.size();

    //  Get number of spills, limited to maximum requested spills
    auto n_spills = lightdata_tree->GetEntries();
    auto all_spills = std::min((int)n_spills, (int)max_spill);

    //  Prepare output file
    std::string outname = data_repository + "/" + run_name + "/recodata.root";
    if (std::filesystem::exists(outname) && !force_recodata_rebuild)
    {
        mist::logger::info(TString::Format("Output file already exists, skipping: %s", outname.c_str()).Data());
        return;
    }

    TFilePtr output_file(TFile::Open(outname.c_str(), "RECREATE"));
    if (!output_file || output_file->IsZombie())
    {
        mist::logger::error(TString::Format("(recodata_writer) Failed to create output file %s", outname.c_str()).Data());
        return;
    }
    TTree *recodata_tree = new TTree("recodata", "Recodata tree");
    AlcorRecodata recodata;
    recodata.write_to_tree(recodata_tree);

    //  Cache channel positions from Mapping

    // Phase 5: iterate (device, chip, channel) directly via the
    // GlobalIndex overload and key the cache by `4 * channel_ordinal` —
    // matches the position-cache convention in Mapping.cxx and the
    // MIST HoughTransform `lut_key`.
    std::map<int, std::array<float, 2>> index_to_hit_xy;
    {
        constexpr int kDeviceLo  = 192;
        constexpr int kDeviceHi  = 224;
        const int max_chip       = ::gidx::kUsesSplitInTwo ? 4 : 8;
        constexpr int kChannelHi = 64;
        for (int device = kDeviceLo; device < kDeviceHi; ++device)
            for (int chip = 0; chip < max_chip; ++chip)
                for (int channel = 0; channel < kChannelHi; ++channel)
                {
                    const auto gi = ::GlobalIndex::from_components(
                        device, /*fifo=*/0, chip, channel, /*tdc=*/0);
                    auto position = current_mapping.get_position_from_global_index(gi);
                    if (!position)
                        continue;
                    if (fabs((*position)[0]) < 5 && fabs((*position)[1]) < 5)
                        continue;
                    index_to_hit_xy[4 * gi.channel_ordinal()] = *position;
                }
    }

    //  Edge rejection window: 25 ns fixed, converted to clock cycles
    const float edge_rejection_cc = 25.f / BTANA_ALCOR_CC_TO_NS;

    //  ── Trigger selection QA histograms ──────────────────────────────────────
    //  X axis uses registry position (dense, named) instead of raw trigger index.
    //  registry.index_of(raw) maps any observed trigger value to its bin.
    //
    //  h_trigger_qa: per-trigger-type outcome counts
    //    Y bin 1 = accepted, 2 = edge-rejected, 3 = duplicate-rejected
    //  h_frames_per_spill: frame counts per spill per outcome category
    //  h_edge_trigger_position: coarse time of edge-rejected triggers,
    //    to verify the 25 ns cut is well placed

    RootHist<TH2F> h_trigger_qa(
        "h_trigger_qa",
        "Trigger selection QA;trigger;outcome",
        n_triggers, 0, n_triggers,
        3, 0, 3);
    for (int i = 0; i < n_triggers; ++i)
        h_trigger_qa->GetXaxis()->SetBinLabel(i + 1, registry.triggers[i].second.c_str());
    h_trigger_qa->GetYaxis()->SetBinLabel(1, "accepted");
    h_trigger_qa->GetYaxis()->SetBinLabel(2, "edge-rejected");
    h_trigger_qa->GetYaxis()->SetBinLabel(3, "duplicate-rejected");

    RootHist<TH2F> h_frames_per_spill(
        "h_frames_per_spill",
        "Frame counts per spill;spill;category",
        all_spills, 0, all_spills,
        4, 0, 4);
    h_frames_per_spill->GetYaxis()->SetBinLabel(1, "total");
    h_frames_per_spill->GetYaxis()->SetBinLabel(2, "accepted");
    h_frames_per_spill->GetYaxis()->SetBinLabel(3, "had edge trigger");
    h_frames_per_spill->GetYaxis()->SetBinLabel(4, "duplicate-rejected");

    RootHist<TH2F> h_edge_trigger_position(
        "h_edge_trigger_position",
        "Position of edge-rejected triggers;trigger;coarse time (cc)",
        n_triggers, 0, n_triggers,
        500, 0, framer_cfg.frame_size);
    for (int i = 0; i < n_triggers; ++i)
        h_edge_trigger_position->GetXaxis()->SetBinLabel(i + 1, registry.triggers[i].second.c_str());

    std::unordered_map<int, RootHist<TH1F>> h_trigger_time_diff_w_cherenkov;

    //  Enable a 50 MB tree cache before the two GetEntry passes (§4.7 minimum
    //  mitigation): the second full pass over the spill tree at line :275
    //  re-reads every basket from disk without it.  Proper single-pass
    //  restructure remains the open item.
    lightdata_tree->SetCacheSize(50 * 1024 * 1024);
    lightdata_tree->AddBranchToCache("*", true);

    //  ── Loop over spills ─────────────────────────────────────────────────────
    std::map<int, std::vector<float>> map_of_offsets;
    for (int i_spill = 0; i_spill < all_spills; ++i_spill)
    {
        lightdata_tree->GetEntry(i_spill);
        spilldata->get_entry();
        auto &frames_in_spill = spilldata->get_frame_list_link();
        auto &frame_reference = spilldata->get_frame_reference_list_link();

        //  ── First loop for calibration ──────────────────────────────────────────
        for (auto &current_lightdata_struct : frames_in_spill)
        {
            AlcorLightdata current_lightdata(current_lightdata_struct);
            auto current_trigger_list = current_lightdata.get_triggers();
            auto timing_trigger = std::find_if(current_trigger_list.begin(),
                                               current_trigger_list.end(),
                                               [](const TriggerEvent &e)
                                               {
                                                   return e.index == _TRIGGER_STREAMING_RING_FOUND_;
                                               });
            if (timing_trigger != current_trigger_list.end())
            {
                for (const auto &current_cherenkov : current_lightdata.get_cherenkov_hits_link())
                {
                    AlcorFinedata current_hit(current_cherenkov);
                    auto index = current_hit.get_global_index();
                    map_of_offsets[index].push_back(
                        current_hit.get_time_ns() - timing_trigger->fine_time);
                }
            }
        }
    }
    for (auto &[channel_index, values_list] : map_of_offsets)
    {
        if (values_list.size() < 20)
            continue;

        auto offset_value = 0.f;
        auto offset_participants = 0;
        for (auto &value : values_list)
        {
            if (fabs(value) > 30)
                continue;
            offset_value += value;
            offset_participants++;
        }
        offset_value /= offset_participants;

        if (offset_participants < 20)
            continue;

        if (!channel_index)
        {
            AlcorFinedata temy_testt;
            temy_testt.set_global_index(0);
            temy_testt.set_rollover(0);
            temy_testt.set_coarse(0);
            temy_testt.set_fine(0);
            mist::logger::debug(TString::Format("channel %i - offset: %f", channel_index, offset_value).Data());
            mist::logger::debug(TString::Format("channel %i - param0: %f", channel_index, AlcorFinedata::get_param0(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - param1: %f", channel_index, AlcorFinedata::get_param1(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - param2: %f", channel_index, AlcorFinedata::get_param2(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - get_time_ns: %f", channel_index, temy_testt.get_time_ns()).Data());
            AlcorFinedata::set_param2(channel_index, -offset_value / BTANA_ALCOR_CC_TO_NS);
            mist::logger::debug(TString::Format("channel %i - param0: %f", channel_index, AlcorFinedata::get_param0(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - param1: %f", channel_index, AlcorFinedata::get_param1(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - param2: %f", channel_index, AlcorFinedata::get_param2(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - get_time_ns: %f", channel_index, temy_testt.get_time_ns()).Data());
        }
        AlcorFinedata::set_param2(channel_index, -offset_value / BTANA_ALCOR_CC_TO_NS);
    }
    mist::logger::debug("Save face");
    mist::logger::debug("Save face");
    mist::logger::debug("Save face");
    for (int i_spill = 0; i_spill < all_spills; ++i_spill)
    {
        //  Per-spill multi-bar reset (skip first iteration — the subtask is
        //  not yet active on the first pass through).
        if (i_spill > 0)
            progress_bars.restart(/*flush=*/false);
        progress_bars.update(i_spill, all_spills);

        lightdata_tree->GetEntry(i_spill);
        spilldata->get_entry();
        auto &frames_in_spill = spilldata->get_frame_list_link();
        auto &frame_reference = spilldata->get_frame_reference_list_link();

        //  Start-of-spill event: dead lane map
        auto lanes_participating = spilldata->get_not_dead_participants();
        for (auto [device, lanes] : lanes_participating)
            if (device < 200)
                for (auto current_lane : lanes)
                    for (auto i_channel = 0; i_channel < 8; ++i_channel)
                    {
                        // Phase 5: construct the synthetic dead-lane Hit
                        // with a new-layout GlobalIndex.  Split-in-two
                        // trick is applied here at the construction
                        // boundary.  The stored value is the new-layout
                        // raw; the position-cache lookup uses
                        // `4 * channel_ordinal` (the same dense-int key
                        // the position cache was populated with — see the
                        // top-of-function loop and `Mapping.cxx`).
                        const int chip_raw     = current_lane / 4;
                        const int channel_raw  = 8 * (current_lane % 4) + i_channel;
                        const int chip_logical = ::gidx::kUsesSplitInTwo
                                                     ? chip_raw / 2
                                                     : chip_raw;
                        const int channel_log  = ::gidx::kUsesSplitInTwo
                                                     ? channel_raw + 32 * (chip_raw % 2)
                                                     : channel_raw;
                        const auto gi = ::GlobalIndex::from_components(
                            device, current_lane, chip_logical, channel_log, 0);
                        const int pos_key = 4 * gi.channel_ordinal();
                        recodata.add_hit(
                            0., 0., 0.,
                            index_to_hit_xy[pos_key][0],
                            index_to_hit_xy[pos_key][1],
                            gi.raw(),
                            encode_bit(HitmaskDeadLane));
                    }
        recodata.add_trigger({TriggerStartOfSpill, static_cast<uint16_t>(framer_cfg.frame_size / 2)});
        recodata_tree->Fill();
        recodata.clear();

        //  ── Loop over frames ──────────────────────────────────────────────────
        int n_accepted = 0, n_edge = 0, n_duplicate = 0;
        int i_saved_frame = -1;
        i_saved_frame = -1;
        for (auto &current_lightdata_struct : frames_in_spill)
        {
            i_saved_frame++;
            AlcorLightdata current_lightdata(current_lightdata_struct);

            if (i_saved_frame % 1000 == 0)
                post_processing.update(i_saved_frame, frames_in_spill.size());

            h_frames_per_spill->Fill(i_spill, 0.5); // total

            //  ── Trigger selection ───────────────────────────────────────────────
            //  1. Triggers within 25 ns of either boundary → edge-rejected.
            //     Frame not immediately discarded; type may still have a valid instance.
            //  2. Two distinct valid instances of the same type → frame rejected.
            //  3. Two valid instances of the same type within BTANA_TRIGGER_MIN_SEPARATION
            //     cc → temporal duplicate, second dropped silently, frame kept.

            bool frame_rejected = false;
            bool had_edge = false;
            std::map<uint8_t, TriggerEvent> accepted_triggers;

            // Trigger selection order matters:
            //   1. Skip the UNKNOWN sentinel.
            //   2. Edge rejection — edge-rejected triggers do NOT count toward
            //      the per-frame "we've seen this index already" set.
            //   3. Duplicate check (against accepted_triggers from earlier
            //      iterations of this same loop, i.e. earlier in the frame):
            //      - within BTANA_TRIGGER_MIN_SEPARATION cc → temporal dup,
            //        drop silently;
            //      - else → distinct second firing of the same trigger →
            //        reject the whole frame.
            //   4. Otherwise: accept (insert into accepted_triggers), create
            //      the time-diff histogram lazily, fill it.
            // The previous version inserted at the top of the loop BEFORE the
            // duplicate check, so the check at (3) was always true and every
            // trigger was silently dropped on the temporal-duplicate branch.
            for (const auto &current_trigger : current_lightdata.get_triggers())
            {
                if (current_trigger.index == _TRIGGER_UNKNOWN_)
                    continue;

                const int reg_bin = registry.index_of(current_trigger.index) + 0.5; // centre of bin

                bool is_edge = (current_trigger.fine_time < edge_rejection_cc) ||
                               (current_trigger.fine_time > framer_cfg.frame_size - edge_rejection_cc);
                if (is_edge)
                {
                    h_edge_trigger_position->Fill(reg_bin, current_trigger.fine_time);
                    h_trigger_qa->Fill(reg_bin, 1.5); // edge-rejected
                    had_edge = true;
                    continue;
                }

                if (auto it = accepted_triggers.find(current_trigger.index);
                    it != accepted_triggers.end())
                {
                    const auto &prev = it->second;
                    if (std::fabs((float)current_trigger.coarse - (float)prev.coarse) < BTANA_TRIGGER_MIN_SEPARATION)
                        continue; // temporal duplicate, drop silently

                    h_trigger_qa->Fill(reg_bin, 2.5); // duplicate-rejected
                    frame_rejected = true;
                    break;
                }

                // First time seeing this trigger index in this frame — accept.
                accepted_triggers[current_trigger.index] = current_trigger;
                if (!h_trigger_time_diff_w_cherenkov.count(current_trigger.index))
                    h_trigger_time_diff_w_cherenkov[current_trigger.index] =
                        RootHist<TH1F>(
                            TString::Format("h_trigger_time_diff_w_cherenkov_%s", registry.name_of(current_trigger.index).c_str()).Data(),
                            ";#Delta_{t} (t_{Hit} - t_{trigger}) ns;Normalised entries",
                            5e3,
                            -500,
                            500);
                for (const auto &current_cherenkov_hit_struct : current_lightdata.get_cherenkov_hits_link())
                    h_trigger_time_diff_w_cherenkov[current_trigger.index]->Fill(
                        AlcorFinedata(current_cherenkov_hit_struct).get_time_ns() -
                        current_trigger.fine_time);
            }

            if (frame_rejected)
            {
                n_duplicate++;
                h_frames_per_spill->Fill(i_spill, 3.5); // duplicate-rejected
                continue;
            }

            if (had_edge)
            {
                n_edge++;
                h_frames_per_spill->Fill(i_spill, 2.5); // had edge trigger (frame kept)
            }

            for (auto &[index, trigger] : accepted_triggers)
            {
                h_trigger_qa->Fill(registry.index_of(index) + 0.5, 0.5); // accepted
                recodata.add_trigger(trigger);
            }

            //  ── Cherenkov hits ─────────────────────────────────────────────────
            for (const auto &current_cherenkov_hit_struct : current_lightdata.get_cherenkov_hits_link())
                recodata.add_hit(current_cherenkov_hit_struct);

            recodata_tree->Fill();
            recodata.clear();
            n_accepted++;
            h_frames_per_spill->Fill(i_spill, 1.5); // accepted
        }

        mist::logger::info(TString::Format("Spill %i done — accepted: %i  had-edge: %i  duplicate-rejected: %i  total: %zu",
                                i_spill, n_accepted, n_edge, n_duplicate, frames_in_spill.size()).Data());

        //  Reflect the just-completed spill on the main bar.
        progress_bars.update(i_spill + 1, all_spills);
    } // end spill loop

    post_processing.finish(/*flush=*/false);
    progress_bars.finish();

    //  --- --- --- --- --- ---
    //  QA plots
    //  ---
    output_file->cd();
    recodata_tree->Write();
    //  ---
    //  --- Trigger QA
    TDirectory *trigger_dir = output_file->mkdir("Triggers");
    trigger_dir->cd();
    h_trigger_qa->Write();
    h_frames_per_spill->Write();
    h_edge_trigger_position->Write();
    for (auto &[key, val] : h_trigger_time_diff_w_cherenkov)
        val->Write();
    //
    //  input_file and output_file closed automatically by TFilePtr dtors.
    //  End: QA plots
    //  --- --- --- --- --- ---
}

