#include "../lib_loader.h"
#include "utility/root_io.h"
#include "utility/root_hist.h"
#include "utility/config_reader.h"
#include <mist/logger/logger.h>

/**
 * @file afterpulse_treatment.cpp
 * @brief Examine afterpulse behaviour on the cherenkov SiPM channels.
 *
 * @details
 * This exercise investigates how external and software-based triggers
 * coincide with signals recorded by the detector, and quantifies the
 * fraction of those signals that are tagged as afterpulses.
 *
 * **Trigger types:**
 * - **External triggers (0, 1)**
 *   Registered in an ALCOR board, if available.
 *
 * - **Software-based triggers (>= 100)**
 *   Elaborated offline, if available:
 *   - **100: Dummy trigger**
 *     Signals that frames are taken at the start of a spill (first N frames,
 *     defined by `BTANA_FIRST_FRAMES_TRIGGER` in `streaming_framer.h`).
 *     Only coarse timing information is available for this trigger.
 *   - **101: Timing trigger**
 *     Computed from the coincidence of all available timing channels
 *     (a pair of scintillators coupled to two different 4x8 SiPM boards).
 *     The reference time is the average of times registered on all channels,
 *     fine-tuned (currently no offset correction applied).
 *
 * **Afterpulse tagging:**
 * - Afterpulses are detected using `AlcorRecodata::is_afterpulse()`.
 * - A signal is considered an afterpulse if the time difference with the previous
 *   signal on the same channel is below `BTANA_AFTERPULSE_DEADTIME` (defined in `streaming_framer.h`).
 *
 * **Result persistence:**
 * - The principal scalar output is the *afterpulse probability*
 *   (N_afterpulse / N_total), posted per SiPM model and aggregated as "all"
 *   to `standard_results.toml` under the key `afterpulse.probability`.
 *
 * The SiPM-model split (e.g. "1350" / "1375") is resolved from the readout
 * config — the single source of truth — via the cherenkov role's
 * `sensor_for(device)`, never a hardcoded channel range.
 *
 * @author Nicola Rubini
 */

void afterpulse_treatment(std::string data_repository, std::string run_name, int max_frames = 10000000,
                          std::string readout_config_file = "conf/readout_config.toml")
{
    //  Sensor split (SiPM model, e.g. "1350" / "1375") is read from the readout
    //  config — the single source of truth — NOT hardcoded by channel range.
    //  Each hit resolves to its ALCOR device, and the cherenkov role's
    //  `sensor_for(device)` returns the model tag (per-device override, else
    //  role-level).
    ReadoutConfigList readout_config(readout_config_reader(readout_config_file));
    const ReadoutConfigStruct *cherenkov = readout_config.find_by_name("cherenkov");
    if (!cherenkov)
        mist::logger::warning("[afterpulse_treatment] no 'cherenkov' role in " + readout_config_file +
                              " — per-sensor afterpulse split disabled");

    //  Input files
    std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";

    //  Load recodata, return if not available
    TFilePtr input_file_recodata(TFile::Open(input_filename_recodata.c_str(), "READ"));
    if (!input_file_recodata || input_file_recodata->IsZombie())
    {
        mist::logger::error("[afterpulse_treatment] Could not open recodata: " + input_filename_recodata);
        return;
    }

    //  Link recodata tree locally
    TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
    AlcorRecodata *recodata = new AlcorRecodata();
    recodata->link_to_tree(recodata_tree);

    //  Get number of frames, limited to maximum requested frames
    auto n_frames = recodata_tree->GetEntries();
    auto all_frames = min((int)n_frames, (int)max_frames);

    //  Time distribution
    RootHist<TH1F> h_t_distribution("h_t_distribution", ";t_{Hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    RootHist<TH1F> h_t_AP_distribution("h_t_AP_distribution", ";t_{Hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    RootHist<TH1F> h_t_noAP_distribution("h_t_noAP_distribution", ";t_{Hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    //  Time distribution
    RootHist<TH1F> h_t_detector_0("h_t_detector_0", ";t_{Hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    RootHist<TH1F> h_t_detector_1("h_t_detector_1", ";t_{Hit} - t_{timing} (ns)", 200, -312.5, 312.5);

    //  Afterpulse counters per SiPM model (sensor tag) plus a global tally.
    //  probability = afterpulse hits / total hits for that population.
    std::map<std::string, long> total_hits_by_sensor;
    std::map<std::string, long> afterpulse_hits_by_sensor;
    long total_hits_all = 0;
    long afterpulse_hits_all = 0;

    //  --- --- --- --- --- ---
    //  Loop on data
    //  ---
    auto i_spill = -1;
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        //  Load data for current frame
        recodata_tree->GetEntry(i_frame);

        //  Takes note of spill evolution
        if (recodata->is_start_of_spill())
        {
            //  You can internally keep track of spills
            i_spill++;

            //  This event is not of physical interest, skip it
            continue;
        }

        //  Select Luca AND trigger (0) or timing trigger (101)
        auto default_hardware_trigger = recodata->get_trigger_by_index(0);
        if (default_hardware_trigger)
        {
            //  Loop on hits
            for (auto current_hit = 0; current_hit < recodata->get_recodata().size(); current_hit++)
            {
                //  Resolve this hit's SiPM model from its device via the readout
                //  config (single source of truth).
                const std::string sensor = cherenkov ? cherenkov->sensor_for(recodata->get_device(current_hit)) : "";
                const bool is_afterpulse = recodata->is_afterpulse(current_hit);

                //  Tally for the afterpulse probability (per sensor + global)
                total_hits_all++;
                if (is_afterpulse)
                    afterpulse_hits_all++;
                if (!sensor.empty())
                {
                    total_hits_by_sensor[sensor]++;
                    if (is_afterpulse)
                        afterpulse_hits_by_sensor[sensor]++;
                }

                //  Fill time distribution to check
                h_t_distribution->Fill(recodata->get_hit_t(current_hit) - default_hardware_trigger->fine_time);

                //  Remove afterpulse
                if (is_afterpulse)
                    h_t_AP_distribution->Fill(recodata->get_hit_t(current_hit) - default_hardware_trigger->fine_time);
                else
                    h_t_noAP_distribution->Fill(recodata->get_hit_t(current_hit) - default_hardware_trigger->fine_time);
            }
        }
    }
    //  ---
    //  Loop on data
    //  --- --- --- --- --- ---

    //  Afterpulse probability with binomial uncertainty
    //  sqrt(p(1-p)/N).  N==0 yields p=err=0 (nothing to report).
    auto afterpulse_probability = [](long afterpulse_hits, long total_hits) -> std::pair<double, double>
    {
        if (total_hits <= 0)
            return {0., 0.};
        const double p = (double)afterpulse_hits / (double)total_hits;
        const double err = std::sqrt(std::max(0., p * (1. - p) / (double)total_hits));
        return {p, err};
    };

    // ── Persist to AnalysisResults ────────────────────────────────────────────
    {
        AnalysisResults ar(data_repository + "/standard_results.toml");
        ResultMap entries;

        //  Aggregate over all cherenkov hits
        const auto [p_all, p_all_err] = afterpulse_probability(afterpulse_hits_all, total_hits_all);
        entries[{run_name, "all", "afterpulse.probability"}] = {p_all, p_all_err};

        //  Per-SiPM-model split (config-resolved; e.g. "1350" / "1375")
        for (const auto &[sensor, total_hits] : total_hits_by_sensor)
        {
            const auto [p, p_err] = afterpulse_probability(afterpulse_hits_by_sensor[sensor], total_hits);
            entries[{run_name, sensor, "afterpulse.probability"}] = {p, p_err};
        }

        ar.update(entries);
        mist::logger::info("[afterpulse_treatment] afterpulse probability written to standard_results.toml for run " + run_name);
    }

    //  Plotting the result
    TCanvas *c_time_delta = new TCanvas("c_time_delta", "Afterpulse check on coincidences of timing and cherenkov sensors");
    gPad->SetLogy();
    h_t_distribution->SetLineColor(kBlack);
    h_t_distribution->SetLineWidth(2);
    h_t_distribution->SetMinimum(1.);
    h_t_distribution->Draw();
    h_t_AP_distribution->SetLineColor(kRed);
    h_t_AP_distribution->Draw("SAME");
    h_t_noAP_distribution->SetLineColor(kBlue);
    h_t_noAP_distribution->Draw("SAME");
}
