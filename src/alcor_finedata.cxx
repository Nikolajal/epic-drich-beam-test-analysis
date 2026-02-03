#include "alcor_finedata.h"
#include <sstream>
#include <cmath>
#include <iostream>

alcor_finedata_struct::alcor_finedata_struct(const alcor_data_struct &d)
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

// --- Constructors
alcor_finedata::alcor_finedata() : fine_dist_fit_function(new TF1("fine_dist_fit_function", "[0]*((1./(1+TMath::Exp(-[1]*(x-[2]))))-(1./(1+TMath::Exp(-[1]*(x-[3])))))")) {}

alcor_finedata::alcor_finedata(const alcor_finedata_struct &s)
    : alcor_data(s), finedata(s), fine_dist_fit_function(new TF1("fine_dist_fit_function", "[0]*((1./(1+TMath::Exp(-[1]*(x-[2]))))-(1./(1+TMath::Exp(-[1]*(x-[3])))))")) {}

alcor_finedata::alcor_finedata(const alcor_data_struct &d)
    : alcor_data(d), finedata(d), fine_dist_fit_function(new TF1("fine_dist_fit_function", "[0]*((1./(1+TMath::Exp(-[1]*(x-[2]))))-(1./(1+TMath::Exp(-[1]*(x-[3])))))")) {}

alcor_finedata::alcor_finedata(const alcor_finedata &o)
    : alcor_data(o), finedata(o.get_data_struct()), fine_dist_fit_function(new TF1("fine_dist_fit_function", "[0]*((1./(1+TMath::Exp(-[1]*(x-[2]))))-(1./(1+TMath::Exp(-[1]*(x-[3])))))")) {}

// --- Destructor
alcor_finedata::~alcor_finedata()
{
    delete fine_dist_fit_function;
}

// --- Member functions
void alcor_finedata::set_standard_function()
{
    fine_dist_fit_function->SetParLimits(0, 0., 1e6);
    fine_dist_fit_function->SetParLimits(1, 0.5, 5.);
    fine_dist_fit_function->SetParLimits(2, 10., 50.);
    fine_dist_fit_function->SetParLimits(3, 80., 120.);
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
    return 0.;
}

// --- Setters
void alcor_finedata::set_param0(int global_tdc_index, float value) { calibration_parameters[global_tdc_index][0] = value; }
void alcor_finedata::set_param1(int global_tdc_index, float value) { calibration_parameters[global_tdc_index][1] = value; }
void alcor_finedata::set_param2(int global_tdc_index, float value) { calibration_parameters[global_tdc_index][2] = value; }

// --- File operations
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

void alcor_finedata::generate_calibration(TH2F *calibration_histogram)
{
    // leave your original implementation
}
