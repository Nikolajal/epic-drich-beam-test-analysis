#include "streaming_framer.h"
#include <algorithm>
#include <iostream>
using namespace std;

//  Constructors
streaming_framer::streaming_framer(std::vector<std::string> filenames,
                                   uint16_t frame_size)
    : streaming_framer(filenames, "", "", frame_size) {}
streaming_framer::streaming_framer(std::vector<std::string> filenames,
                                   std::string trigger_config_file,
                                   uint16_t frame_size)
    : streaming_framer(filenames, trigger_config_file, "", frame_size) {}
streaming_framer::streaming_framer(std::vector<std::string> filenames,
                                   std::string trigger_config_file,
                                   std::string readout_config_file,
                                   uint16_t frame_size)
    : _frame_size(frame_size)
{
    // Create streams
    data_streams.reserve(filenames.size());
    for (const auto &current_filename : filenames)
    {
        data_streams.emplace_back(current_filename);
        if (!data_streams.back().is_valid())
            cerr << "[WARNING] Failed to open streamer: " << current_filename << std::endl;
    }

    // QA plots
    init_QA_plots();

    // Trigger map initialization
    triggers = trigger_conf_reader(trigger_config_file);
    for (auto current_trigger : triggers)
        triggers_map[current_trigger.device] = current_trigger;

    // Readout configuration
    readout_config = readout_config_list(readout_config_reader(readout_config_file));

    // Initialize spill counter
    _current_spill = -1;
}

// Getters
alcor_spilldata streaming_framer::get_spilldata() const { return spilldata; }
alcor_spilldata &streaming_framer::get_spilldata_link() { return spilldata; }
std::map<std::string, TH1 *> streaming_framer::get_QA_plots() { return QA_plots; }

// Setters
void streaming_framer::set_spilldata(alcor_spilldata v) { spilldata = v; }
void streaming_framer::set_spilldata_link(alcor_spilldata &v) { spilldata = v; }

// Initialize QA plots
void streaming_framer::init_QA_plots()
{
    TH1::AddDirectory(false); // prevent ROOT directory issues
    h_frames_per_spill = new TH1F("h_frames_per_spill", ";spill;evs", 100, -0.5, 99.5);
    h_participants_lanes_per_spill = new TH1F("h_participants_lanes_per_spill", "Participants lanes per spill", 100, -0.5, 99.5);
    h_dead_lanes_per_spill = new TH1F("h_dead_lanes_per_spill", "Dead lanes per spill", 100, -0.5, 99.5);
    QA_plots["TH2F_fine_calib_global_index"] = new TH2F("TH2F_fine_calib_global_index", ";;", 12000, 0, 12000, 256, 0, 256);
}

// I/O operations
bool streaming_framer::next_spill()
{
    spilldata.clear();
    _current_spill++;
    bool current_spill_has_data = false;

    auto &frame_list = spilldata.get_frame_link();

    // --- First frames trigger
    for (auto i_frame = 0; i_frame < _FIRST_FRAMES_TRIGGER_; ++i_frame)
    {
        auto &current_lightdata = frame_list[i_frame];
        auto &current_trigger_hits = current_lightdata.trigger_hits;
        current_trigger_hits.push_back({100, static_cast<uint16_t>(_frame_size / 2.), static_cast<float>(_ALCOR_CC_TO_NS_ * _frame_size / 2.)});
    }

    // loop over input streams
    for (auto &current_stream : data_streams)
    {
        if (!current_stream.is_valid())
            continue;

        //  Tell where we are in the file evaluation
        std::string path = current_stream.get_filename();
        std::string filename = std::filesystem::path(path).filename().string();
        std::cout << "\33[2K\r[INFO] Spill " << (int)_current_spill << " processing file: " << filename << flush;

        auto trigger_counter = 0;
        while (current_stream.read_next())
        {
            auto &current_data = current_stream.current();
            auto current_device = current_data.get_device();
            auto current_chip = current_data.get_chip();
            auto current_readout_tag_list = readout_config.find_by_device_and_chip(current_device, current_chip);

            // Skip if not tagged
            if (current_readout_tag_list.size() == 0 && (current_chip != (99 / 4)))
                continue;

            // Start spill
            if (current_data.is_start_spill())
            {
                current_spill_has_data = true;
                auto current_fifo = current_data.get_fifo();

                auto &current_participants_mask = spilldata.get_participants_mask_link();
                current_participants_mask[static_cast<uint8_t>(current_device)] += encode_bits({(uint8_t)current_fifo});
                h_participants_lanes_per_spill->Fill((int)_current_spill);

                if (current_data.coarse_time_clock() != 0)
                {
                    auto &current_dead_mask = spilldata.get_dead_mask_link();
                    current_dead_mask[static_cast<uint8_t>(current_device)] += encode_bits({(uint8_t)current_fifo});
                    h_dead_lanes_per_spill->Fill((int)_current_spill);
                }
            }

            // end of spill
            if (current_data.is_end_spill())
                break;

            // Refactoring time to relate to frame reference
            uint32_t frame_index = current_data.get_coarse_global_time() / (_frame_size * 1.);
            uint64_t frame_coarse = current_data.get_coarse_global_time() % static_cast<uint64_t>(_frame_size);
            uint64_t frame_coarse_global = current_data.get_coarse_global_time();

            // Triggers
            //  --- trigger hit
            if (current_data.is_trigger_tag())
            {
                trigger_counter++;
                if (!triggers_map.count(current_device))
                    triggers_map[current_device] = {"UNDF", 255, 0, static_cast<uint16_t>(current_device)};
                auto current_trg_index = triggers_map[current_device].index;
                auto current_trg_delay = triggers_map[current_device].delay;
                frame_index = (current_data.get_coarse_global_time() - current_trg_delay) / (_frame_size * 1.);
                frame_coarse = (current_data.get_coarse_global_time() - current_trg_delay) % static_cast<uint64_t>(_frame_size);
                auto &current_lightdata = frame_list[frame_index];
                auto &current_trigger_hits = current_lightdata.trigger_hits;
                if (!triggers_map.count(current_device))
                    current_trigger_hits.push_back({static_cast<uint8_t>(current_trg_index), static_cast<uint16_t>(current_device), 0.});
                else
                    current_trigger_hits.push_back({static_cast<uint8_t>(current_trg_index), static_cast<uint16_t>(frame_coarse), static_cast<float>(_ALCOR_CC_TO_NS_ * frame_coarse)});
                // QA
                if (!QA_utility.count(Form("TH1F_delta_time_trigger_%i", current_trg_index)))
                {
                    QA_utility[Form("TH1F_delta_time_trigger_%i", current_trg_index)] = current_data.get_coarse_global_time();
                }
                else
                {
                    if (!QA_plots.count(Form("TH1F_delta_time_trigger_%i", current_trg_index)))
                        QA_plots[Form("TH1F_delta_time_trigger_%i", current_trg_index)] = new TH1F(Form("TH1F_delta_time_trigger_%i", current_trg_index), ";trigger hit 0 - trigger hit 1;entries", 2000, -20000, 20000);
                    QA_plots[Form("TH1F_delta_time_trigger_%i", current_trg_index)]->Fill(current_data.get_coarse_global_time() - QA_utility[Form("TH1F_delta_time_trigger_%i", current_trg_index)]);
                    //  --- QA Plot :
                    if (!QA_plots.count(Form("h_trigger_%i_ref_per_frame", current_trg_index)))
                        QA_plots[Form("h_trigger_%i_ref_per_frame", current_trg_index)] = new TH1F(Form("h_trigger_%i_ref_per_frame", current_trg_index), ";frame ID;Trigger", 2000, 0, 2000000);
                    QA_plots[Form("h_trigger_%i_ref_per_frame", current_trg_index)]->Fill(frame_index);
                    QA_utility[Form("TH1F_delta_time_trigger_%i", current_trg_index)] = current_data.get_coarse_global_time();
                }
            }

            //  Assigning frame
            //  ---  related time in data
            current_data.set_rollover(0);
            current_data.set_coarse(frame_coarse);

            // ALCOR hit
            if (current_data.is_alcor_hit())
            {
                //  ---
                //  Afterpulse mask
                //  TODO: implement a new mask to signal the hit is labeled afterpulse
                current_data.set_mask(0);
                auto current_channel = current_data.get_global_index();
                if (auto search = afterpulse_map.find(current_channel); search != afterpulse_map.end())
                    if (frame_coarse_global < afterpulse_map[current_channel])
                        current_data.add_mask_bit(_HITMASK_afterpulse);
                afterpulse_map[current_channel] = frame_coarse_global + _AFTERPULSE_DEADTIME_;
                //  ---

                auto &current_lightdata = frame_list[frame_index];
                if (std::find(current_readout_tag_list.begin(), current_readout_tag_list.end(), "timing") != current_readout_tag_list.end())
                {
                    auto &current_timing_hits = current_lightdata.timing_hits;
                    current_timing_hits.push_back(std::move(static_cast<alcor_finedata_struct>(current_data.get_data_struct())));
                }
                if (std::find(current_readout_tag_list.begin(), current_readout_tag_list.end(), "tracking") != current_readout_tag_list.end())
                {
                    auto &current_tracking_hits = current_lightdata.tracking_hits;
                    current_tracking_hits.push_back(std::move(static_cast<alcor_finedata_struct>(current_data.get_data_struct())));
                }
                if (std::find(current_readout_tag_list.begin(), current_readout_tag_list.end(), "cherenkov") != current_readout_tag_list.end())
                {
                    auto &current_cherenkov_hits = current_lightdata.cherenkov_hits;
                    current_cherenkov_hits.push_back(std::move(static_cast<alcor_finedata_struct>(current_data.get_data_struct())));
                }
                QA_plots["TH2F_fine_calib_global_index"]->Fill(current_data.get_global_tdc_index(), current_data.get_fine());
            }
        }
    }
    return current_spill_has_data;
}
