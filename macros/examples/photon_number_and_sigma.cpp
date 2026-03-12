#include "../lib_loader.h"

/**
 * @file photon_number_and_sigma.cpp
 * @brief Compare the mean number of photons and the SPSR for different run series.
 *
 * This macro follows the same logic used in the other analysis examples in this
 * repository:
 * - the first fit on time-coincident hits is used to estimate the average ring centre
 *   and radius;
 * - a second selection around the average ring is used to estimate the mean
 *   number of photons per frame;
 * - the Single Photon Spatial Resolution (SPSR) is extracted with the same method
 *   used in `ring_spatial_resolution_with_tracking.cpp`, namely from the
 *   resolution-vs-photon-number trend fitted with
 *   `#sqrt{SPSR^{2}/N_{#gamma} + c^{2}}`.
 *
 * At the end, the macro prints the numerical results and draws three comparison
 * canvases:
 * - mirror-position scan;
 * - threshold / bias-voltage scan;
 * - gas comparison.
 *
 * @author Mattia Valenti
 */

 //commenta ogni parte del codice, spiegando cosa fa e perché è stata fatta in quel modo, facendo riferimento alla logica dell'analisi e alle scelte fatte per l'estrazione dei risultati.

namespace
{
std::array<float, 2> time_cut_boundaries = {-45.f, 20.f};

struct run_configuration
{
    std::string run_name;
    std::string plot_label;
    std::string description;
};

struct run_group
{
    std::string tag;
    std::string title;
    std::vector<run_configuration> runs;
};

struct run_result
{
    std::string run_name;
    std::string plot_label;
    std::string description;
    double photon_number = 0.;
    double photon_number_error = 0.;
    double spsr = 0.;
    double spsr_error = 0.;
    int used_frames = 0;
    int first_round_frames = 0;
    bool valid = false;
};

double expand_axis_max(double value)
{
    if (value <= 0.)
        return 1.;
    return 1.25 * value;
}

run_result analyse_single_run(std::string data_repository, const run_configuration &configuration, int max_frames)
{
    //  Result struct to fill and return at the end of the function
    run_result result;
    result.run_name = configuration.run_name;
    result.plot_label = configuration.plot_label;
    result.description = configuration.description;

    //  Open input file and get recodata tree
    std::string input_filename_recodata = data_repository + "/" + configuration.run_name + "/recodata.root";

    //  Load recodata, return if not available
    TFile *input_file_recodata = new TFile(input_filename_recodata.c_str());
    if (!input_file_recodata || input_file_recodata->IsZombie())
    {
        std::cerr << "[WARNING] Could not find recodata for run " << configuration.run_name << std::endl;
        return result;
    }

    //  Get recodata tree and link to recodata object, return if not available
    TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
    if (!recodata_tree)
    {
        std::cerr << "[WARNING] Could not find recodata tree for run " << configuration.run_name << std::endl;
        return result;
    }

    //  Link recodata tree locally
    alcor_recodata *recodata = new alcor_recodata();
    recodata->link_to_tree(recodata_tree);

    //  Get number of frames, limit to max_frames if needed
    auto n_frames = recodata_tree->GetEntries();
    auto all_frames = std::min((int)n_frames, max_frames);

    //  Histograms for first round of fits, to find ring center and radius
    TH1F *h_first_round_X = new TH1F(Form("h_first_round_X_%s", configuration.run_name.c_str()), ";circle center x coordinate (mm)", 120, -30, 30);
    TH1F *h_first_round_Y = new TH1F(Form("h_first_round_Y_%s", configuration.run_name.c_str()), ";circle center y coordinate (mm)", 120, -30, 30);
    TH1F *h_first_round_R = new TH1F(Form("h_first_round_R_%s", configuration.run_name.c_str()), ";circle radius (mm)", 200, 30, 130);
    TH1F *h_n_gamma = new TH1F(Form("h_n_gamma_%s", configuration.run_name.c_str()), ";N_{#gamma}", 197, 3, 200);
    
    // Histograms for second round of fits, to find photon number and SPSR
    TH2F *h_second_round_R_Ngamma = new TH2F(Form("h_second_round_R_Ngamma_%s", configuration.run_name.c_str()), ";circle radius (mm);N_{#gamma}", 200, 30, 130, 197, 3, 200);
    TH1F *h_second_round_R_excluded = new TH1F(Form("h_second_round_R_excluded_%s", configuration.run_name.c_str()), ";circle radius - point radius (mm)", 120, -30, 30);

    //  First loop over frames: find ring center and radius with a first fit on time-coincident hits
    std::vector<std::pair<int, float>> frame_of_interest_ref;

    //  Loop over frames
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        recodata_tree->GetEntry(i_frame);

        if (recodata->is_start_of_spill())
            continue;
        //  Get default hardware trigger for the frame, skip if not available
        auto default_hardware_trigger = recodata->get_trigger_by_index(0);
        if (!default_hardware_trigger)
            continue;

        //  Store frame number and trigger time for later use in second loop
        frame_of_interest_ref.push_back({i_frame, default_hardware_trigger->fine_time});

        //  Vector to store selected points for the fit
        std::vector<std::array<float, 2>> selected_points;
        float avg_radius = 0.f;

        //  Loop on hits
        for (int current_hit = 0; current_hit < recodata->get_recodata().size(); ++current_hit)
        {
            if (recodata->is_afterpulse(current_hit))
                continue;

            //  Time coincidence with trigger
            auto time_delta_wrt_ref = recodata->get_hit_t(current_hit) - default_hardware_trigger->fine_time;
            // Ask for time coincidence, skip if not satisfied
            if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
                continue;

            //  Check the hit has been labeled as ring-belonging, skip if not satisfied
            selected_points.push_back({recodata->get_hit_x(current_hit), recodata->get_hit_y(current_hit)});
            avg_radius += recodata->get_hit_r(current_hit);
        }

        // Fit selected points with a circle, if enough points for the fit (> 4), and save results for later QA
        if (selected_points.size() <= 4)
            continue;

        //  Fit result is {{center_x_value,center_x_error}, {center_y_value,center_y_error}, {radius_value,radius_error}}
        auto fit_result = fit_circle(selected_points, {0.f, 0.f, avg_radius / selected_points.size()}, false, {{}});
        h_first_round_X->Fill(fit_result[0][0]);
        h_first_round_Y->Fill(fit_result[1][0]);
        h_first_round_R->Fill(fit_result[2][0]);
        result.first_round_frames++;
    }

    //  Check we have valid first-round fits, otherwise return
    if (result.first_round_frames == 0 || h_first_round_R->GetEntries() == 0)
    {
        std::cerr << "[WARNING] No valid first-round fits for run " << configuration.run_name << std::endl;
        return result;
    }

    //  Get ring center and radius from first round of fits, to be used as starting values for second round of fits
    auto found_ring_center_x = h_first_round_X->GetMean();
    auto found_ring_center_y = h_first_round_Y->GetMean();
    auto found_ring_radius = h_first_round_R->GetMean();
    auto found_ring_radius_stddev = h_first_round_R->GetRMS();

    //  If the standard deviation of the radius distribution is 0, set it to 1 to avoid excluding all points in the second round of fits
    if (found_ring_radius_stddev <= 0.)
        found_ring_radius_stddev = 1.;

    // Second loop over frames: select points around the average ring and extract photon number and SPSR
    for (auto i_frame : frame_of_interest_ref)
    {
        recodata_tree->GetEntry(i_frame.first);

        //  Vector to store selected points for the fit
        std::vector<std::array<float, 2>> selected_points;

        //  Loop on hits
        for (int current_hit = 0; current_hit < recodata->get_recodata().size(); ++current_hit)
        {
            if (recodata->is_afterpulse(current_hit))
                continue;

            //  Time coincidence with trigger
            auto time_delta_wrt_ref = recodata->get_hit_t(current_hit) - i_frame.second;
            if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
                continue;

            // Check the hit is within a reasonable distance from the average ring, to exclude outliers that would spoil the fit, skip if not satisfied
            if (std::fabs(recodata->get_hit_r(current_hit, {(float)found_ring_center_x, (float)found_ring_center_y}) - found_ring_radius) > 3.f * found_ring_radius_stddev)
                continue;

            //  Store selected points for the fit
            selected_points.push_back({(float)recodata->get_hit_x(current_hit), (float)recodata->get_hit_y(current_hit)});
        }

        //  Fit selected points with a circle, if enough points for the fit (> 4), and save results for photon number and SPSR extraction
        if (selected_points.size() <= 4)
            continue;

        auto fit_result = fit_circle(selected_points, {(float)found_ring_center_x, (float)found_ring_center_y, (float)found_ring_radius}, true, {{}});

        h_n_gamma->Fill(selected_points.size());
        h_second_round_R_Ngamma->Fill(fit_result[2][0], selected_points.size());
        result.used_frames++;

        //  For SPSR extraction with method 2, exclude one point at a time from the fit and fill the distribution of radius differences (see `ring_spatial_resolution_with_tracking.cpp` for details)
        for (int i_ter = 0; i_ter < selected_points.size(); ++i_ter)
        {
            fit_result = fit_circle(selected_points, {(float)found_ring_center_x, (float)found_ring_center_y, (float)found_ring_radius}, true, {i_ter});
            auto radius = std::hypot(selected_points[i_ter][0] - found_ring_center_x, selected_points[i_ter][1] - found_ring_center_y);
            h_second_round_R_excluded->Fill(fit_result[2][0] - radius);
        }
    }

    //  Check we have valid second-round fits, otherwise return
    if (result.used_frames == 0 || h_n_gamma->GetEntries() == 0)
    {
        std::cerr << "[WARNING] No valid second-round selections for run " << configuration.run_name << std::endl;
        return result;
    }

    //  Get mean photon number and error from the distribution of selected points per frame
    result.photon_number = h_n_gamma->GetMean();
    result.photon_number_error = h_n_gamma->GetMeanError();

    //  Extract SPSR with method 1: fit the resolution-vs-photon-number trend with `#sqrt{SPSR^{2}/N_{#gamma} + c^{2}}`, where SPSR and c are free parameters of the fit
    TGraphErrors *g_resolution = new TGraphErrors();
    g_resolution->SetName(Form("g_resolution_%s", configuration.run_name.c_str()));

    //  Loop over the y_bin, i.e. Ngamma, of the 2D histogram to find the resolution vs p.e.
    for (int y_bin = 1; y_bin <= h_second_round_R_Ngamma->GetNbinsY(); ++y_bin)
    {
        //  Get the photon number corresponding to the current y_bin
        auto n_gamma = h_second_round_R_Ngamma->GetYaxis()->GetBinCenter(y_bin);
        //  Slice the 2D histogram to get the distribution of radius values for the current photon number
        auto current_r_slice = h_second_round_R_Ngamma->ProjectionX(Form("r_slice_%s_%d", configuration.run_name.c_str(), y_bin), y_bin, y_bin);

        //  Check we have enough statistics for the fit, skip if not satisfied
        if (current_r_slice->GetEntries() < 100)
        {
            delete current_r_slice;
            continue;
        }

        //  Fit the slice with a Gaussian function to extract the resolution, skip if fit fails or if the fit error on the resolution is too large
        auto fit_gaus = new TF1(Form("fit_gaus_%s_%d", configuration.run_name.c_str(), y_bin), "gaus", 0, 150);
        current_r_slice->Fit(fit_gaus, "QNR");

        //  Check fit success and reliability, skip if not satisfied
        if (fit_gaus->GetParameter(2) <= 0.)
        {
            delete current_r_slice;
            delete fit_gaus;
            continue;
        }

        //  If the relative error on the resolution is larger than 7.5%, we consider the fit result unreliable and skip it
        //if (fit_gaus->GetParError(2) / fit_gaus->GetParameter(2) > 0.075)
        //{
        //    delete current_r_slice;
        //    delete fit_gaus;
        //    continue;
        //}

        //  Assign the resolution value and error to the TGraph for the current photon number
        auto current_point = g_resolution->GetN();
        g_resolution->SetPoint(current_point, n_gamma, fit_gaus->GetParameter(2));
        g_resolution->SetPointError(current_point, 0., fit_gaus->GetParError(2));

        delete current_r_slice;
        delete fit_gaus;
    }

    //  Fit the resolution-vs-photon-number trend with `#sqrt{SPSR^{2}/N_{#gamma} + c^{2}}` to extract the SPSR, if we have at least 2 points in the TGraph, otherwise get the SPSR from the distribution of radius differences filled with method 2 (see `ring_spatial_resolution_with_tracking.cpp` for details)
    if (g_resolution->GetN() >= 2)
    {
        TF1 *f_resolution = new TF1(Form("f_resolution_%s", configuration.run_name.c_str()), "TMath::Sqrt([0] * [0] / x + [1] * [1])", 0.1, 200.);
        f_resolution->SetParameters(2.5, 0.5);
        f_resolution->SetParName(0, "SPSR");
        f_resolution->SetParName(1, "constant");
        g_resolution->Fit(f_resolution, "Q");
        result.spsr = f_resolution->GetParameter(0);
        result.spsr_error = f_resolution->GetParError(0);
    }
    else
    {
        result.spsr = h_second_round_R_excluded->GetStdDev();
        result.spsr_error = h_second_round_R_excluded->GetStdDevError();
    }

    result.valid = (result.photon_number > 0.) && (result.spsr > 0.);

    //  Print run summary
    std::cout << "Run " << result.run_name << " | " << result.description
              << " | <N_gamma> = " << result.photon_number << " +- " << result.photon_number_error
              << " | SPSR = " << result.spsr << " +- " << result.spsr_error
              << " | used frames = " << result.used_frames << std::endl;

    return result;
}

//  Function to print the summary of results for a run group and to draw the comparison canvas for the group
void draw_group_summary(const run_group &group, const std::vector<run_result> &results)
{
    //  Get the maximum photon number and SPSR values among the results to set the axis limits of the canvas
    auto n_runs = (int)group.runs.size();
    auto max_photon = 0.;
    auto max_spsr = 0.;
    for (const auto &result : results)
    {
        if (!result.valid)
            continue;
        if (result.photon_number + result.photon_number_error > max_photon)
            max_photon = result.photon_number + result.photon_number_error;
        if (result.spsr + result.spsr_error > max_spsr)
            max_spsr = result.spsr + result.spsr_error;
    }

    //  Expand axis limits by 25% to leave some space for the legend and to improve readability, and to avoid having the highest point at the edge of the canvas
    auto photon_axis_max = expand_axis_max(max_photon);
    auto spsr_axis_max = expand_axis_max(max_spsr);
    auto scale_factor = photon_axis_max / spsr_axis_max;

    //  Create histograms and graphs for the canvas
    TH1F *h_frame = new TH1F(Form("h_frame_%s", group.tag.c_str()), (group.title + ";Configuration;<N_{#gamma}> / SPSR").c_str(), n_runs, 0.5, n_runs + 0.5);
    h_frame->SetMinimum(0.);
    h_frame->SetMaximum(photon_axis_max);
    h_frame->SetStats(0);
    h_frame->SetLineColor(kWhite);
    h_frame->GetYaxis()->SetTitle("Mean N_{#gamma}");
    h_frame->GetYaxis()->SetTitleColor(kBlue + 1);
    h_frame->GetYaxis()->SetLabelColor(kBlue + 1);
    h_frame->GetXaxis()->SetLabelSize(0.045);

    //  Graph for photon number and SPSR, with different marker styles and colors for better readability
    TGraphErrors *g_photon = new TGraphErrors();
    g_photon->SetName(Form("g_photon_%s", group.tag.c_str()));
    g_photon->SetMarkerStyle(20);
    g_photon->SetMarkerSize(1.2);
    g_photon->SetMarkerColor(kBlue + 1);
    g_photon->SetLineColor(kBlue + 1);
    g_photon->SetLineWidth(2);

    //  Graph for SPSR, with different marker style and color for better readability, and scaled to have the same maximum value as the photon number graph, to be able to show both graphs on the same canvas with the same y-axis
    TGraphErrors *g_spsr = new TGraphErrors();
    g_spsr->SetName(Form("g_spsr_%s", group.tag.c_str()));
    g_spsr->SetMarkerStyle(21);
    g_spsr->SetMarkerSize(1.2);
    g_spsr->SetMarkerColor(kRed + 1);
    g_spsr->SetLineColor(kRed + 1);
    g_spsr->SetLineWidth(2);

    //  Fill graphs with results from the analysis of each run in the group, and print a summary of results for the group
    auto photon_point = 0;
    auto spsr_point = 0;

    //  Loop over runs in the group and fill graphs with valid results, while skipping invalid results and printing a warning message for them
    for (int i_run = 0; i_run < n_runs; ++i_run)
    {
        h_frame->GetXaxis()->SetBinLabel(i_run + 1, group.runs[i_run].plot_label.c_str());

        if (!results[i_run].valid)
            continue;

        //  Fill photon number graph, with error bars, for the current run, and fill SPSR graph, with error bars and scaled to have the same maximum value as the photon number graph, for the current run
        g_photon->SetPoint(photon_point, i_run + 1, results[i_run].photon_number);
        g_photon->SetPointError(photon_point, 0., results[i_run].photon_number_error);
        photon_point++;

        g_spsr->SetPoint(spsr_point, i_run + 1, results[i_run].spsr * scale_factor);
        g_spsr->SetPointError(spsr_point, 0., results[i_run].spsr_error * scale_factor);
        spsr_point++;
    }

    //  Set style options for the canvas
    gStyle->SetOptStat(0); 

    //  Create canvas and draw graphs, with a secondary y-axis for the SPSR graph, and a legend to distinguish between the two graphs
    TCanvas *canvas = new TCanvas(Form("c_%s", group.tag.c_str()), group.title.c_str(), 1200, 700);
    canvas->SetGridy();
    canvas->SetBottomMargin(0.22);
    canvas->SetLeftMargin(0.10);
    canvas->SetRightMargin(0.12);

    // Draw the frame histogram to set the axis limits and labels, and then draw the photon number and SPSR graphs on top of it, with the same y-axis but different colors for better readability
    h_frame->Draw();
    h_frame->LabelsOption("v", "X");
    g_photon->Draw("LP SAME");
    g_spsr->Draw("LP SAME");

    //  Create a secondary y-axis on the right side of the canvas for the SPSR graph, with the same scale as the photon number graph, and with a different color for better readability
    TGaxis *right_axis = new TGaxis(n_runs + 0.5, 0., n_runs + 0.5, photon_axis_max, 0., spsr_axis_max, 510, "+L");
    right_axis->SetTitle("SPSR (mm)");
    right_axis->SetTitleColor(kRed + 1);
    right_axis->SetLabelColor(kRed + 1);
    right_axis->SetLineColor(kRed + 1);
    right_axis->Draw();

    //  Create and draw a legend to distinguish between the photon number and SPSR graphs, with different marker styles and colors for better readability
    TLegend *legend = new TLegend(0.14, 0.80, 0.36, 0.90);
    legend->SetBorderSize(0);
    legend->SetFillStyle(0);
    legend->AddEntry(g_photon, "Mean N_{#gamma}", "lp");
    legend->AddEntry(g_spsr, "SPSR", "lp");
    legend->Draw();
}

//  Function to print the summary of results for a run group in the terminal, with a clear and organized format, and to distinguish between valid and invalid results with a warning message for invalid results
void print_group_summary(const run_group &group, const std::vector<run_result> &results)
{
    std::cout << std::endl;
    std::cout << "==================== " << group.title << " ====================" << std::endl;
    for (const auto &result : results)
    {
        if (!result.valid)
        {
            std::cout << result.run_name << " | " << result.description << " | result not available" << std::endl;
            continue;
        }

        std::cout << result.run_name
                  << " | " << result.description
                  << " | <N_gamma> = " << result.photon_number << " +- " << result.photon_number_error
                  << " | SPSR = " << result.spsr << " +- " << result.spsr_error
                  << std::endl;
    }
}
} // namespace

//  Main function to run the analysis for all run groups and to draw the comparison canvases for all groups
void photon_number_and_sigma(
    std::string data_repository = "/Users/mattiavalenti/Desktop/analisi_tesi_magistrale/data",
    int max_frames = 100000)
{
    std::vector<run_group> groups = {
        {
            "mirror_scan",
            "Mirror-position comparison",
            {
                {"20251111-105543", "70 mm", "Physics run with the mirror moved at 70 mm from home point"},
                {"20251111-110914", "+60 mm", "Aerogel mirror moved to +60 mm from home point"},
                {"20251111-111835", "+50 mm", "Aerogel mirror moved to +50 mm from home point"},
                {"20251111-113106", "+40 mm QA", "Aerogel mirror moved to +40 mm from home point, with scp running in background for automatic QA"},
                {"20251111-114254", "+40 mm", "Aerogel mirror moved to +40 mm from home point"},
                {"20251111-115851", "+100 mm", "Aerogel mirror moved to +100 mm from home point"},
                {"20251111-124119", "+90 mm", "Aerogel mirror moved to +90 mm from home point"},
                {"20251111-124758", "+80 mm", "Aerogel mirror moved to +80 mm from home point"},
            },
        },
        {
            "threshold_scan",
            "Delta-threshold / voltage comparison",
            {
                {"20251112-011436", "52 V", "Delta threshold 10 at 52 V"},
                {"20251112-014034", "53 V", "Delta threshold 10 at 53 V"},
                {"20251112-020826", "54 V", "Delta threshold 10 at 54 V"},
                {"20251112-025653", "55 V", "Delta threshold 10 at 55 V"},
            },
        },
        {
            "gas_scan",
            "Gas comparison",
            {
                {"20251118-183513", "Ar 3 bar", "Gas Argon at 3 bar"},
                {"20251119-010426", "C2F6", "Gas C2F6"},
            },
        },
    };

    for (const auto &group : groups)
    {
        std::vector<run_result> results;
        for (const auto &configuration : group.runs)
            results.push_back(analyse_single_run(data_repository, configuration, max_frames));

        print_group_summary(group, results);
        draw_group_summary(group, results);
    }
}
