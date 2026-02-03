#include "alcor_data.h"

// Constructors
alcor_data::alcor_data(const alcor_data_struct &data_struct)
    : data(data_struct) {}

alcor_data::alcor_data(int device, int fifo, int type, int counter,
                       int column, int pixel, int tdc, int rollover,
                       int coarse, int fine)
{
    data.device = device;
    data.fifo = fifo;
    data.type = type;
    data.counter = counter;
    data.column = column;
    data.pixel = pixel;
    data.tdc = tdc;
    data.rollover = rollover;
    data.coarse = coarse;
    data.fine = fine;
}

// --- Getters
alcor_data_struct alcor_data::get_data_struct() const { return data; }
int alcor_data::get_device() const { return data.device; }
int alcor_data::get_fifo() const { return data.fifo; }
int alcor_data::get_type() const { return data.type; }
int alcor_data::get_counter() const { return data.counter; }
int alcor_data::get_column() const { return data.column; }
int alcor_data::get_pixel() const { return data.pixel; }
int alcor_data::get_tdc() const { return data.tdc; }
int alcor_data::get_rollover() const { return data.rollover; }
int alcor_data::get_coarse() const { return data.coarse; }
int alcor_data::get_fine() const { return data.fine; }

// --- Derived getters
int alcor_data::get_chip() const { return data.fifo / 4; }
int alcor_data::get_eo_channel() const { return data.pixel + 4 * data.column + 32 * (get_chip() % 2); }
int alcor_data::get_calib_index() const { return data.tdc + 4 * get_eo_channel() + 128 * get_chip(); }
int alcor_data::get_device_index() const { return get_eo_channel() + 64 * (get_chip() / 2); }
int alcor_data::get_global_index() const { return get_device_index() + 256 * (get_device() - 192); }
int alcor_data::get_global_tdc_index() const { return 4 * get_global_index() + get_tdc(); }
uint64_t alcor_data::get_coarse_global_time() const { return get_coarse() + get_rollover() * rollover_to_clock; }

// --- Setters
void alcor_data::set_data_struct_copy(alcor_data_struct input_data) { data = input_data; }
void alcor_data::set_data_struct_linked(alcor_data_struct &input_data) { data = input_data; }
void alcor_data::set_device(int val) { data.device = val; }
void alcor_data::set_fifo(int val) { data.fifo = val; }
void alcor_data::set_type(int val) { data.type = val; }
void alcor_data::set_counter(int val) { data.counter = val; }
void alcor_data::set_column(int val) { data.column = val; }
void alcor_data::set_pixel(int val) { data.pixel = val; }
void alcor_data::set_tdc(int val) { data.tdc = val; }
void alcor_data::set_rollover(int val) { data.rollover = val; }
void alcor_data::set_coarse(int val) { data.coarse = val; }
void alcor_data::set_fine(int val) { data.fine = val; }

// --- Hit checks
bool alcor_data::is_alcor_hit() const { return get_type() == alcor_hit; }
bool alcor_data::is_trigger_tag() const { return get_type() == trigger_tag; }
bool alcor_data::is_start_spill() const { return get_type() == start_spill; }
bool alcor_data::is_end_spill() const { return get_type() == end_spill; }

// --- Time
int alcor_data::coarse_time_clock() const { return get_coarse() + get_rollover() * rollover_to_clock; }
double alcor_data::coarse_time_ns() const { return get_coarse() * coarse_to_ns + get_rollover() * rollover_to_ns; }

// --- Comparison
bool alcor_data::operator<(const alcor_data &c) const { return coarse_time_ns() < c.coarse_time_ns(); }
bool alcor_data::operator<=(const alcor_data &c) const { return coarse_time_ns() <= c.coarse_time_ns(); }
bool alcor_data::operator>(const alcor_data &c) const { return coarse_time_ns() > c.coarse_time_ns(); }
bool alcor_data::operator>=(const alcor_data &c) const { return coarse_time_ns() >= c.coarse_time_ns(); }

// --- ROOT I/O
void alcor_data::link_to_tree(TTree *input_tree)
{
    if (!input_tree) return;
    input_tree->SetBranchAddress("device", &data.device);
    input_tree->SetBranchAddress("fifo", &data.fifo);
    input_tree->SetBranchAddress("type", &data.type);
    input_tree->SetBranchAddress("counter", &data.counter);
    input_tree->SetBranchAddress("column", &data.column);
    input_tree->SetBranchAddress("pixel", &data.pixel);
    input_tree->SetBranchAddress("tdc", &data.tdc);
    input_tree->SetBranchAddress("rollover", &data.rollover);
    input_tree->SetBranchAddress("coarse", &data.coarse);
    input_tree->SetBranchAddress("fine", &data.fine);
}

void alcor_data::write_to_tree(TTree *output_tree)
{
    if (!output_tree) return;
    output_tree->Branch("device", &data.device, "device/I");
    output_tree->Branch("fifo", &data.fifo, "fifo/I");
    output_tree->Branch("type", &data.type, "type/I");
    output_tree->Branch("counter", &data.counter, "counter/I");
    output_tree->Branch("column", &data.column, "column/I");
    output_tree->Branch("pixel", &data.pixel, "pixel/I");
    output_tree->Branch("tdc", &data.tdc, "tdc/I");
    output_tree->Branch("rollover", &data.rollover, "rollover/I");
    output_tree->Branch("coarse", &data.coarse, "coarse/I");
    output_tree->Branch("fine", &data.fine, "fine/I");
}
