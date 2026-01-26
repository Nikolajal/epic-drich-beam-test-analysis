#pragma once

#include <fstream>
#include "TH2F.h"
#include "TF1.h"
#include "alcor_data.h"
#include "TCanvas.h"

struct alcor_finedata_struct : public alcor_data_struct
{
    alcor_finedata_struct() = default;
    alcor_finedata_struct(const alcor_data_struct &d)
    {
        device = d.device;
        fifo = d.fifo;
        type = d.type;
        counter = d.counter;
        column = d.column;
        pixel = d.pixel;
        tdc = d.tdc;
        rollover = d.rollover;
        coarse = d.coarse;
        fine = d.fine;
    }
};

class alcor_finedata : public alcor_data
{
private:
    alcor_finedata_struct finedata;
    inline static std::unordered_map<int, std::array<float, 3>> calibration_parameters = {};

    // Fit function
    TF1 *fine_dist_fit_function = new TF1("fine_dist_fit_function", "[0]*((1./(1+TMath::Exp(-[1]*(x-[2]))))-(1./(1+TMath::Exp(-[1]*(x-[3])))))");
    void set_standard_function();

protected:
public:
    //  Constructor
    alcor_finedata() = default;
    alcor_finedata(const alcor_finedata_struct &s) : alcor_data(s), finedata(s) {}
    alcor_finedata(const alcor_data_struct &d) : alcor_data(d), finedata(d) {}
    alcor_finedata(const alcor_finedata &o) : alcor_data(o), finedata(o.get_data_struct()) {}

    //  Getters
    float get_phase() const;

    //  Setters
    void set_param0(int global_tdc_index, float value) { calibration_parameters[global_tdc_index][0] = value; }
    void set_param1(int global_tdc_index, float value) { calibration_parameters[global_tdc_index][1] = value; }
    void set_param2(int global_tdc_index, float value) { calibration_parameters[global_tdc_index][2] = value; }

    //  Generate calibration
    void generate_calibration(TH2F *);
    void write_calib_to_file(std::string filename);
    void read_calib_from_file(std::string filename, bool clear_first = true, bool overwrites = true);
};

void alcor_finedata::set_standard_function()
{
    fine_dist_fit_function->SetParLimits(0, 0., 1e6);
    fine_dist_fit_function->SetParLimits(1, 0.5, 5.);
    fine_dist_fit_function->SetParLimits(2, 10., 50.);
    fine_dist_fit_function->SetParLimits(3, 80., 120.);
}

void alcor_finedata::generate_calibration(TH2F *calibration_histogram)
{
    calibration_parameters.clear();

    TH1F *h_par1 = new TH1F("h_par1", "h_par1", 40, 10, 50);
    TH1F *h_par2 = new TH1F("h_par2", "h_par2", 40, 80, 120);
    TH2F *h_par_corr = new TH2F("h_par_corr", "", 120, 0, 120, 120, 0, 120);

    //  Loop over the calibration histogram
    set_standard_function();
    auto channels = 0;
    new TCanvas();
    for (auto xbin = 1; xbin <= calibration_histogram->GetNbinsX(); xbin++)
    {
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

        //  Check the result is consistent
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
        {
            continue;
        }
        //  Store the calibration
        calibration_parameters[xbin - 1] = {first_parameter, second_parameter, 0.};
        h_par1->Fill(first_parameter);
        h_par2->Fill(second_parameter);
        h_par_corr->Fill(first_parameter, second_parameter);
    }
    cout << channels << endl;

    //  --- TFIX
    new TCanvas();
    h_par1->Draw();
    new TCanvas();
    h_par2->Draw();
    new TCanvas();
    h_par_corr->Draw("COLZ");
}

float alcor_finedata::get_phase() const
{
    auto calib_it = calibration_parameters.find(get_global_tdc_index());
    if (calib_it != calibration_parameters.end())
    {
        auto calibration_parameter = calib_it->second;
        if ((calibration_parameter[1] == 0) && (calibration_parameter[0] == 0))
            return 0.;
        auto current_fine_value = static_cast<float>(finedata.fine);
        auto phase = -(current_fine_value - calibration_parameter[0]) / (calibration_parameter[1] - calibration_parameter[0]);
        phase -= calibration_parameter[2];
        return phase;
    }
    else
        return 0.;
}

void alcor_finedata::write_calib_to_file(std::string filename)
{
    std::ofstream calib_file(filename);
    if (!calib_file)
        throw std::runtime_error("Cannot open file");
    for (auto [tdc_index, calib_params] : calibration_parameters)
        calib_file << tdc_index << " " << calib_params[0] << " " << calib_params[1] << " " << calib_params[2] << std::endl;
    calib_file.close();
}

void alcor_finedata::read_calib_from_file(std::string filename, bool clear_first, bool overwrites)
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
        if (calibration_parameters.count(key) && !overwrites)
            continue;
        ss >> key >> a >> b >> c;
        calibration_parameters[key] = {a, b, c};
    }
    calib_file.close();
}

#ifdef __ROOTCLING__
#pragma link C++ struct alcor_finedata_struct + ;
#pragma link C++ class std::vector < alcor_finedata_struct> + ;
#endif