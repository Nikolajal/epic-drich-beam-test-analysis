#include "alcor_finedata.h"

//  TODO: merge with alcor data, no sense to have this overhead << it no makes perfect sense, data compression | merge data here
//  TODO: understand what is the issue with generate calibration

// =============================================================================
// alcor_finedata_struct
// =============================================================================

alcor_finedata_struct::alcor_finedata_struct(const alcor_data_struct &d)
{
    global_index = get_global_index(d.device, d.fifo / 4, d.pixel + 4 * d.column, d.tdc);
    rollover = static_cast<uint32_t>(d.rollover);
    coarse   = static_cast<uint16_t>(d.coarse);
    fine     = static_cast<uint8_t>(d.fine);
    hit_mask = d.hit_mask;
}

// =============================================================================
// alcor_finedata — Constructors
// =============================================================================

alcor_finedata::alcor_finedata() {}

alcor_finedata::alcor_finedata(const alcor_data_struct &s)
    : internal_data(s) {}

alcor_finedata::alcor_finedata(const alcor_finedata_struct &d)
    : internal_data(d) {}

alcor_finedata::alcor_finedata(const alcor_finedata &o)
    : internal_data(o.get_data_struct()) {}

// =============================================================================
// alcor_finedata — Derived timing getters
// =============================================================================

float alcor_finedata::get_phase() const
{
    auto calib_it = calibration_parameters.find(get_global_index());
    if (calib_it != calibration_parameters.end())
    {
        auto calibration_parameter = calib_it->second;
        if ((calibration_parameter[1] == 0) && (calibration_parameter[0] == 0))
            return 0.;
        auto current_fine_value = static_cast<float>(get_fine());
        auto phase = -(current_fine_value - calibration_parameter[0]) / (calibration_parameter[1] - calibration_parameter[0]);
        phase -= calibration_parameter[2];
        return phase;
    }
    return 0.;
}

// =============================================================================
// alcor_finedata — Spatial getters (randomised)
// =============================================================================

float alcor_finedata::get_hit_r_rnd(std::array<float, 2> v) const
{
    const float x = get_hit_x_rnd();
    const float y = get_hit_y_rnd();
    return std::hypot(x - v[0], y - v[1]);
}

float alcor_finedata::get_hit_phi_rnd(std::array<float, 2> v) const
{
    const float x = get_hit_x_rnd();
    const float y = get_hit_y_rnd();
    return std::atan2(y - v[1], x - v[0]);
}

// =============================================================================
// alcor_finedata — Calibration I/O
// =============================================================================

void alcor_finedata::write_calib_to_file(const std::string &filename)
{
    std::ofstream calib_file(filename);
    if (!calib_file)
        throw std::runtime_error("Cannot open file");
    for (auto [tdc_index, calib_params] : calibration_parameters)
        calib_file << tdc_index << " " << calib_params[0] << " " << calib_params[1] << " " << calib_params[2] << std::endl;
}

void alcor_finedata::read_calib_from_file(const std::string &filename, bool clear_first, bool overwrites)
{
    if (clear_first)
        calibration_parameters.clear();
    std::ifstream calib_file(filename);
    if (!calib_file)
        throw std::runtime_error("Cannot open file");
    std::string line;
    while (std::getline(calib_file, line))
    {
        std::stringstream ss(line);
        int key;
        float a, b, c;
        ss >> key >> a >> b >> c;
        if (calibration_parameters.count(key) && !overwrites)
            continue;
        calibration_parameters[key] = {a, b, c};
    }
}

void alcor_finedata::generate_calibration(TH2F *calibration_histogram, bool overwrite_calibration)
{
    calibration_parameters.clear();

    TF1 *fine_dist_fit_function = new TF1("fine_dist_fit_function", "[0]*((1./(1+TMath::Exp(-[1]*(x-[2]))))-(1./(1+TMath::Exp(-[1]*(x-[3])))))", 0, 256);
    fine_dist_fit_function->SetParLimits(0, 0., 1e6);
    fine_dist_fit_function->SetParLimits(1, 0.5, 5.);
    fine_dist_fit_function->SetParLimits(2, 10., 50.);
    fine_dist_fit_function->SetParLimits(3, 80., 120.);

    TH1F *h_par1 = new TH1F("h_par1", "h_par1", 40, 10, 50);
    TH1F *h_par2 = new TH1F("h_par2", "h_par2", 40, 80, 120);
    TH2F *h_par_corr = new TH2F("h_par_corr", "", 120, 0, 120, 120, 0, 120);

    auto channels = 0;
    new TCanvas();
    for (auto xbin = 1; xbin <= calibration_histogram->GetNbinsX(); xbin++)
    {
        if (!overwrite_calibration && calibration_parameters.count(xbin - 1))
            continue;
        auto current_tdc_fine_calib = calibration_histogram->ProjectionY(Form("tmp_%i", xbin), xbin, xbin);
        if (current_tdc_fine_calib->GetEntries() < 250)
            continue;
        channels++;
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
            continue;

        calibration_parameters[xbin - 1] = {first_parameter, second_parameter, 0.};
        h_par1->Fill(first_parameter);
        h_par2->Fill(second_parameter);
        h_par_corr->Fill(first_parameter, second_parameter);
    }

    //  --- TFIX
    new TCanvas();
    h_par1->Draw();
    new TCanvas();
    h_par2->Draw();
    new TCanvas();
    h_par_corr->Draw("COLZ");
}

// =============================================================================
// alcor_finedata — Finding rings algorithms
// =============================================================================

std::vector<mist::ring_finding::ring_result> alcor_finedata::alcor_find_rings_hough(
    mist::ring_finding::hough_transform &ht,
    std::vector<alcor_finedata> &alcor_hits,
    float threshold_fraction,
    int min_hits,
    int min_active,
    int max_rings,
    float collection_radius)
{
    // --- Build generic hit vector, keeping a parallel index map -------------
    // generic_to_alcor[i] gives the index into alcor_hits that corresponds to
    // generic_hits[i].  This lets us write mask bits back after ring finding.
    std::vector<mist::ring_finding::hit> generic_hits;
    std::vector<int> generic_to_alcor;
    generic_hits.reserve(alcor_hits.size());
    generic_to_alcor.reserve(alcor_hits.size());

    for (int i = 0; i < static_cast<int>(alcor_hits.size()); ++i)
    {
        const auto &h = alcor_hits[i];

        // ALCOR-specific filters — do not belong in hough_transform
        if (h.is_afterpulse())
            continue;
        if (h.get_device() >= 200)
            continue;

        generic_hits.push_back({h.get_hit_x(),
                                h.get_hit_y(),
                                h.get_time_ns(),
                                static_cast<int>(4 * h.get_global_channel_index())});
        generic_to_alcor.push_back(i);
    }

    // --- Run the generic ring finder ----------------------------------------
    std::vector<mist::ring_finding::ring_result> rings =
        ht.find_rings(generic_hits, threshold_fraction, min_hits,
                      max_rings, collection_radius);

    // --- Write mask bits back onto the original alcor_finedata hits ---------
    // Ring mask bits in declaration order; extend the array for more rings.
    const std::array<hit_mask, 2> ring_masks = {
        _HITMASK_hough_ring_tag_first,
        _HITMASK_hough_ring_tag_second};

    for (int ring_idx = 0; ring_idx < static_cast<int>(rings.size()); ++ring_idx)
    {
        if (ring_idx >= static_cast<int>(ring_masks.size()))
            break; // no mask bit defined for this ring index

        for (int generic_idx : rings[ring_idx].hit_indices)
            alcor_hits[generic_to_alcor[generic_idx]].add_mask_bit(ring_masks[ring_idx]);
    }

    return rings;
}