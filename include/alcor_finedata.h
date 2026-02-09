#pragma once

#include <fstream>
#include <unordered_map>
#include <array>
#include "TH2F.h"
#include "TF1.h"
#include "TCanvas.h"
#include "alcor_data.h"

struct alcor_finedata_struct
{

    uint32_t calib_index;
    int rollover;
    int coarse;
    int fine;
    uint32_t hit_mask;

    alcor_finedata_struct() = default;
    alcor_finedata_struct(const alcor_data_struct &d);
};

class alcor_finedata
{
private:
    alcor_finedata_struct data;
    inline static std::unordered_map<int, std::array<float, 3>> calibration_parameters = {};
    void set_standard_function();

public:
    // Constructors
    alcor_finedata();
    alcor_finedata(const alcor_finedata_struct &s);
    alcor_finedata(const alcor_data_struct &d);
    alcor_finedata(const alcor_finedata &o);

    // Getters
    //  ---
    alcor_finedata_struct get_data_struct() const;
    uint32_t get_calib_index() const;
    int get_rollover() const;
    int get_coarse() const;
    int get_fine() const;
    uint32_t get_mask() const;
    float get_phase() const;
    //  --- Derived
    int get_tdc() const;
    int get_device() const;
    int get_fifo() const;
    int get_chip() const;
    int get_eo_channel() const;
    int get_column() const;
    int get_pixel() const;
    int get_device_index() const;
    int get_global_index() const;

    // Setters
    //  --- Pure setters
    void set_data_struct(const alcor_finedata_struct &d);
    void set_calib_index(uint32_t calib);
    void set_rollover(int r);
    void set_coarse(int c);
    void set_fine(int f);
    void set_mask(uint32_t mask);
    //  --- Derived setters
    void set_param0(int global_tdc_index, float value);
    void set_param1(int global_tdc_index, float value);
    void set_param2(int global_tdc_index, float value);

    // Calibration
    void generate_calibration(TH2F *calibration_histogram);
    void write_calib_to_file(const std::string &filename);
    void read_calib_from_file(const std::string &filename, bool clear_first = true, bool overwrites = true);
};
