#include "alcor_finedata.h"
#include "TROOT.h"
#include <memory>

//  TODO: merge with alcor data, no sense to have this overhead << it no makes perfect sense, data compression | merge data here
//  TODO: understand what is the issue with generate calibration

// =============================================================================
// AlcorFinedataStruct
// =============================================================================

AlcorFinedataStruct::AlcorFinedataStruct(const AlcorDataStruct &d)
{
    // Phase 5: store the new-layout @ref GlobalIndex raw — TDC-level form
    // with the validity bit set.  Split-in-two trick is applied here at the
    // conversion boundary: legacy hardware (chip_raw 0–7, channel_raw 0–31)
    // ↦ logical (chip 0–3, channel 0–63) for the current detector;
    // identity transform for the final 64-ch chip (gated by
    // ::gidx::kUsesSplitInTwo).
    const int chip_raw     = d.fifo / 4;
    const int channel_raw  = d.pixel + 4 * d.column;
    const int chip_logical = ::gidx::kUsesSplitInTwo ? chip_raw / 2
                                                    : chip_raw;
    const int channel_log  = ::gidx::kUsesSplitInTwo
                                 ? channel_raw + 32 * (chip_raw % 2)
                                 : channel_raw;
    GlobalIndex = ::GlobalIndex::from_components(
        d.device, d.fifo, chip_logical, channel_log, d.tdc).raw();

    rollover = static_cast<uint32_t>(d.rollover);
    coarse   = static_cast<uint16_t>(d.coarse);
    fine     = static_cast<uint8_t>(d.fine);
    HitMask = d.HitMask;
}

// =============================================================================
// AlcorFinedata — Derived timing getters
// =============================================================================

float AlcorFinedata::get_phase() const
{
    //  Fast path: once calibration is frozen (via freeze_calibration()),
    //  the table is immutable for the rest of the process — every reader
    //  can skip the std::shared_mutex acquisition entirely.  Per-Hit
    //  shared_lock was a real contention point on the 16-thread framer
    //  even with no contention against writers, because shared_mutex
    //  still does atomic RMW on its reader count (CODE_REVIEW §3.1).
    //
    //  Slow path: during setup the table is still mutable, so we take
    //  the shared lock as before.
    //
    //  We access calibration_parameters and channel_calibration_method
    //  DIRECTLY (not through get_param* / get_calibration_method) to avoid
    //  recursive locking on the non-reentrant std::shared_mutex.
    std::shared_lock<std::shared_mutex> lock(calibration_mutex, std::defer_lock);
    if (!is_calibration_frozen())
        lock.lock();

    auto calib_it = calibration_parameters.find(get_global_index());
    if (calib_it == calibration_parameters.end())
        return 0.f;

    const auto &calibration_parameter = calib_it->second;

    //  Inline of get_calibration_method() — must NOT call the public API here.
    auto method_it = channel_calibration_method.find(get_global_index());
    const auto method = (method_it != channel_calibration_method.end())
                            ? method_it->second
                            : default_calibration_method;

    switch (method)
    {
    case CalibrationMethod::AlcorV2BaseCalib:
    {
        auto current_fine_value = static_cast<float>(get_fine());
        if (calibration_parameter[1] == calibration_parameter[0])
            return 0.f;
        if ((calibration_parameter[1] < current_fine_value) && (calibration_parameter[0] > current_fine_value))
            return -9999.f;
        auto phase = (current_fine_value - calibration_parameter[0]) / (calibration_parameter[1] - calibration_parameter[0]);
        phase -= calibration_parameter[2];
        return phase;
    }

    case CalibrationMethod::AlcorV2FitCalib:
    {
        auto current_fine_value = static_cast<float>(get_fine());
        return (current_fine_value * calibration_parameter[0] - calibration_parameter[1]);
    }

    default:
        return 0.f;
    }
}

// =============================================================================
// AlcorFinedata — Spatial getters (randomised)
// =============================================================================

float AlcorFinedata::get_hit_r_rnd(std::array<float, 2> v) const
{
    const float x = get_hit_x_rnd();
    const float y = get_hit_y_rnd();
    return std::hypot(x - v[0], y - v[1]);
}

float AlcorFinedata::get_hit_phi_rnd(std::array<float, 2> v) const
{
    const float x = get_hit_x_rnd();
    const float y = get_hit_y_rnd();
    return std::atan2(y - v[1], x - v[0]);
}

// =============================================================================
// AlcorFinedata — Calibration I/O
// =============================================================================

void AlcorFinedata::write_calib_to_file(const std::string &filename)
{
    mist::logger::info("(AlcorFinedata::write_calib_to_file) Requested to write calibration to file");
    std::ofstream calib_file(filename);
    if (!calib_file)
        throw std::runtime_error("Cannot open file");
    std::shared_lock<std::shared_mutex> lock(calibration_mutex);
    for (auto [tdc_index, calib_params] : calibration_parameters)
    {
        auto method_it = channel_calibration_method.find(tdc_index);
        auto method = (method_it != channel_calibration_method.end())
                          ? method_it->second
                          : default_calibration_method;
        calib_file << tdc_index << " "
                   << static_cast<int>(method) << " "
                   << calib_params[0] << " "
                   << calib_params[1] << " "
                   << calib_params[2] << std::endl;
    }
}

void AlcorFinedata::read_calib_from_file(const std::string &filename, bool clear_first, bool overwrites)
{
    std::unique_lock<std::shared_mutex> lock(calibration_mutex);
    if (clear_first)
    {
        calibration_parameters.clear();
        channel_calibration_method.clear();
    }
    std::ifstream calib_file(filename);
    if (!calib_file)
    {
        mist::logger::warning("(AlcorFinedata::read_calib_from_file) Cannot open " + filename + ", skipping load calibration");
        return;
    }
    std::string line;
    while (std::getline(calib_file, line))
    {
        std::stringstream ss(line);
        int method_int, key;
        float a, b, c;
        ss >> key >> method_int >> a >> b >> c;
        if (calibration_parameters.count(key) && !overwrites)
            continue;
        calibration_parameters[key] = {a, b, c};
        channel_calibration_method[key] = static_cast<CalibrationMethod>(method_int);
    }
}

void AlcorFinedata::switch_to_fit_v2(int GlobalIndex, CalibrationMethod calibration_type, float angular_coeff, float offset, float sigma)
{
    set_calibration_method(GlobalIndex, CalibrationMethod::AlcorV2FitCalib);
    auto prev_min = get_param0(GlobalIndex) < 1 ? 30 : get_param0(GlobalIndex);
    auto prev_max = get_param1(GlobalIndex) < 1 ? 100 : get_param1(GlobalIndex);
    set_param0(GlobalIndex, angular_coeff);
    set_param1(GlobalIndex, -(angular_coeff * (prev_max + prev_min) * 0.5 + offset));
    set_param2(GlobalIndex, sigma);
}

void AlcorFinedata::generate_calibration(TH2F *calibration_histogram, bool overwrite_calibration)
{
    //  Check the batch status
    auto current_batch_ROOT = gROOT->IsBatch();
    gROOT->SetBatch(true);

    //  Check the histogram is valid
    if (!calibration_histogram)
    {
        mist::logger::warning("(AlcorFinedata::generate_calibration) Invalid histogram given, aborting calibration");
        return;
    }

    //  Start calibration
    mist::logger::info("(AlcorFinedata::generate_calibration) Starting fine data calibration from: " + std::string(calibration_histogram->GetName()));

    //  Overwrite protection
    std::unique_lock<std::shared_mutex> lock(calibration_mutex);
    if (overwrite_calibration)
    {
        mist::logger::info("(AlcorFinedata::generate_calibration) Requested clear of previous calibration");
        calibration_parameters.clear();
    }

    //  Fit function
    TF1 *fine_dist_fit_function = new TF1("fine_dist_fit_function", "[0]*((1./(1+TMath::Exp(-[1]*(x-[2]))))-(1./(1+TMath::Exp(-[1]*(x-[3])))))", 0, 256);
    fine_dist_fit_function->SetParLimits(0, 0., 1e6);
    fine_dist_fit_function->SetParLimits(1, 0.5, 5.);
    fine_dist_fit_function->SetParLimits(2, 10., 50.);
    fine_dist_fit_function->SetParLimits(3, 80., 120.);

    //  Reporting
    auto calibrated_channels_before_calibration = calibration_parameters.size();
    std::vector<int> skipped_channels;

    //  Loop on calibration histogram
    for (auto xbin = 1; xbin <= calibration_histogram->GetNbinsX(); xbin++)
    {
        //  Overwrite protection
        if (!overwrite_calibration && calibration_parameters.count(xbin - 1))
            continue;

        //  Small set protection.  unique_ptr handles every continue / return
        //  path including the convergence-skip branch below — the previous
        //  raw new + per-path `delete` leaked the TH1D when the convergence
        //  test failed (CODE_REVIEW §3.2).
        std::unique_ptr<TH1D> current_tdc_fine_calib(
            calibration_histogram->ProjectionY(TString::Format("tmp_%i", xbin).Data(), xbin, xbin));
        current_tdc_fine_calib->SetDirectory(nullptr); // don't pollute gDirectory
        if (current_tdc_fine_calib->GetEntries() < 250)
        {
            skipped_channels.push_back(xbin - 1);
            continue;
        }

        //  Start calibration
        double found_minimum = 0;
        double found_maximum = 0;
        for (auto ibin = 1; ibin <= current_tdc_fine_calib->GetNbinsX(); ibin++)
        {
            if (current_tdc_fine_calib->GetBinContent(ibin) > 5 && found_minimum < 1)
                found_minimum = current_tdc_fine_calib->GetBinCenter(ibin);
            if (current_tdc_fine_calib->GetBinContent(ibin) == 0 && found_minimum > 0 && found_maximum < 1)
            {
                found_maximum = current_tdc_fine_calib->GetBinCenter(ibin);
                break;
            }
        }
        fine_dist_fit_function->SetParameter(0, current_tdc_fine_calib->GetMaximum());
        fine_dist_fit_function->SetParameter(1, 2.5);
        fine_dist_fit_function->SetParameter(2, found_minimum);
        fine_dist_fit_function->SetParLimits(2, found_minimum - 3, found_minimum + 3);
        fine_dist_fit_function->SetParameter(3, found_maximum);
        fine_dist_fit_function->SetParLimits(3, found_maximum - 3, found_maximum + 3);
        current_tdc_fine_calib->Fit(fine_dist_fit_function, "Q");

        auto first_parameter = static_cast<float>(fine_dist_fit_function->GetParameter(2));
        auto second_parameter = static_cast<float>(fine_dist_fit_function->GetParameter(3));
        for (auto i_ter = 0; i_ter < 5; i_ter++)
        {
            if (fabs(second_parameter - first_parameter - 62.5) < 10)
                break;
            current_tdc_fine_calib->Fit(fine_dist_fit_function, "Q");
            first_parameter = static_cast<float>(fine_dist_fit_function->GetParameter(2));
            second_parameter = static_cast<float>(fine_dist_fit_function->GetParameter(3));
        }
        if (fabs(second_parameter - first_parameter - 62.5) > 10)
            continue;   // unique_ptr frees current_tdc_fine_calib here

        calibration_parameters[xbin - 1] = {first_parameter, second_parameter, 0.};
        // unique_ptr frees at end of loop iteration; no manual delete needed.
    }

    //  Debug
    std::string msg = "(AlcorFinedata::generate_calibration) channels: ";
    for (auto channel : skipped_channels)
        msg += std::to_string(channel) + " ";
    msg += "skipped, projection too sparsely";
    mist::logger::debug(msg);

    //  Reporting
    auto calibrated_channels_after_calibration = calibration_parameters.size();
    mist::logger::info("(AlcorFinedata::generate_calibration) Finished calibration! updated " +
                       std::to_string(calibrated_channels_after_calibration - calibrated_channels_before_calibration) +
                       " was: " + std::to_string(calibrated_channels_before_calibration) +
                       " is now: " + std::to_string(calibrated_channels_after_calibration));

    delete fine_dist_fit_function;
    gROOT->SetBatch(current_batch_ROOT);
}

// AlcorFinedata ring-finding adapter (`alcor_find_rings_hough`) removed
// during Phase 3 of the streaming-trigger consolidation — the logic now
// lives inline in `src/triggers/streaming/hough.cxx`, the only consumer.
// See the header comment block at the top of `alcor_finedata.h` and
// `include/triggers/streaming/DISCUSSION.md` § 2 for context.