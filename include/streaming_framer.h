#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include "TFile.h"
#include "TTree.h"
#include "TH1.h"
#include "TH2.h"
#include "TSystem.h"
#include "alcor_spilldata.h"
#include "alcor_data_streamer.h"

#define _FRAME_SIZE_ 1024
#define _FIRST_FRAMES_TRIGGER_ 2500
#define _AFTERPULSE_DEADTIME_ 64 // 200ns

class streaming_framer
{
public:
    // Constructors
    streaming_framer() = default;
    streaming_framer(std::vector<std::string> filenames, uint16_t frame_size = _FRAME_SIZE_);
    streaming_framer(std::vector<std::string> filenames, std::string trigger_config_file, uint16_t frame_size = _FRAME_SIZE_);
    streaming_framer(std::vector<std::string> filenames, std::string trigger_config_file, std::string readout_config_file, uint16_t frame_size = _FRAME_SIZE_);

    // Getters
    alcor_spilldata get_spilldata() const;
    alcor_spilldata &get_spilldata_link();
    std::map<std::string, TH1 *> get_QA_plots();

    // Setters
    void set_spilldata(alcor_spilldata v);
    void set_spilldata_link(alcor_spilldata &v);

    // I/O operations
    bool next_spill();

    // Internal helpers
    void init_QA_plots();

private:
    // Data
    std::vector<alcor_data_streamer> data_streams;
    alcor_spilldata spilldata;

    TH1F *h_frames_per_spill = nullptr;
    TH1F *h_participants_lanes_per_spill = nullptr;
    TH1F *h_dead_lanes_per_spill = nullptr;
    TH2F *h_deadlanes_per_spill = nullptr;

    std::vector<trigger_config_struct> triggers;
    std::unordered_map<uint8_t, trigger_config_struct> triggers_map;
    readout_config_list readout_config;

    uint8_t _current_spill;
    uint16_t _frame_size;

    std::unordered_map<int,uint64_t> afterpulse_map;
    std::map<std::string, uint32_t> _next_spill;
    std::map<std::string, TH1 *> QA_plots;
    std::map<std::string, long double> QA_utility;
};
