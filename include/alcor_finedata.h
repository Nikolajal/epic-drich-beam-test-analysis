#pragma once

#include <fstream>
#include <unordered_map>
#include <array>
#include "TH2F.h"
#include "TF1.h"
#include "TCanvas.h"
#include "alcor_data.h"

struct alcor_finedata_struct : public alcor_data_struct
{
    alcor_finedata_struct() = default;
    alcor_finedata_struct(const alcor_data_struct &d);
};

class alcor_finedata : public alcor_data
{
private:
    alcor_finedata_struct finedata;
    inline static std::unordered_map<int, std::array<float, 3>> calibration_parameters = {};
    void set_standard_function();

public:
    // Constructors
    alcor_finedata();
    alcor_finedata(const alcor_finedata_struct &s);
    alcor_finedata(const alcor_data_struct &d);
    alcor_finedata(const alcor_finedata &o);

    // Getters
    float get_phase() const;

    // Setters
    void set_param0(int global_tdc_index, float value);
    void set_param1(int global_tdc_index, float value);
    void set_param2(int global_tdc_index, float value);

    // Calibration
    void generate_calibration(TH2F *calibration_histogram);
    void write_calib_to_file(const std::string &filename);
    void read_calib_from_file(const std::string &filename, bool clear_first = true, bool overwrites = true);
};
