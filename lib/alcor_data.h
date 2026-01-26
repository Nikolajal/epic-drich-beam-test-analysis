#pragma once

#include "TTree.h"
#include "utility.h"

#define _ALCOR_CC_TO_NS_ 3.125

struct alcor_data_struct
{
  /*
  uint8_t  device;
  uint8_t  fifo;
  uint8_t  type;
  uint8_t  counter;
  uint8_t  column;
  uint8_t  pixel;
  uint8_t  tdc;
  uint32_t rollover;
  uint16_t coarse;
  uint8_t fine;
  */
  int device;
  int fifo;
  int type;
  int counter;
  int column;
  int pixel;
  int tdc;
  int rollover;
  int coarse;
  int fine;
  alcor_data_struct() = default;
};

enum alcor_hit_struct
{
  alcor_hit = 1,
  trigger_tag = 9,
  start_spill = 7,
  end_spill = 15
};

class alcor_data
{

private:
  alcor_data_struct data;
  static constexpr int rollover_to_clock = 32768;
  static constexpr double coarse_to_ns = _ALCOR_CC_TO_NS_;
  static constexpr int rollover_to_ns = rollover_to_clock * coarse_to_ns;

public:
  // Constructors
  alcor_data() = default;
  alcor_data(const alcor_data_struct &data_struct)
      : data(data_struct) {};
  alcor_data(int device, int fifo, int type, int counter,
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
  };

  // Getters
  // --- Native, direct getters to class variables
  alcor_data_struct get_data_struct() const { return data; };
  int get_device() const { return data.device; };
  int get_fifo() const { return data.fifo; };
  int get_type() const { return data.type; };
  int get_counter() const { return data.counter; };
  int get_column() const { return data.column; };
  int get_pixel() const { return data.pixel; };
  int get_tdc() const { return data.tdc; };
  int get_rollover() const { return data.rollover; };
  int get_coarse() const { return data.coarse; };
  int get_fine() const { return data.fine; };
  // --- Derived, getters to combinationa of class variables
  int get_chip() const { return data.fifo / 4; };
  int get_eo_channel() const { return data.pixel + 4 * data.column + 32 * (get_chip() % 2); };
  int get_calib_index() const { return data.tdc + 4 * get_eo_channel() + 128 * get_chip(); };
  int get_device_index() const { return get_eo_channel() + 64 * (get_chip() / 2); };
  int get_global_index() const { return get_device_index() + 256 * (get_device() - 192); };
  int get_global_tdc_index() const { return 4 * get_global_index() + get_tdc(); };
  uint64_t get_coarse_global_time() const { return get_coarse() + get_rollover() * rollover_to_clock; };

  // Setters
  void set_data_struct_copy(alcor_data_struct input_data) { data = input_data; };
  void set_data_struct_linked(alcor_data_struct &input_data) { data = input_data; };
  void set_device(int val) { data.device = val; };
  void set_fifo(int val) { data.fifo = val; };
  void set_type(int val) { data.type = val; };
  void set_counter(int val) { data.counter = val; };
  void set_column(int val) { data.column = val; };
  void set_pixel(int val) { data.pixel = val; };
  void set_tdc(int val) { data.tdc = val; };
  void set_rollover(int val) { data.rollover = val; };
  void set_coarse(int val) { data.coarse = val; };
  void set_fine(int val) { data.fine = val; };

  //  Single hit operators
  bool is_alcor_hit() const { return get_type() == alcor_hit; };
  bool is_trigger_tag() const { return get_type() == trigger_tag; };
  bool is_start_spill() const { return get_type() == start_spill; };
  bool is_end_spill() const { return get_type() == end_spill; };
  int coarse_time_clock() const { return get_coarse() + get_rollover() * rollover_to_clock; };
  double coarse_time_ns() const { return get_coarse() * coarse_to_ns + get_rollover() * rollover_to_ns; };

  //  Comparator operator for sorting and bool operations
  bool operator<(const alcor_data &comparing_hit) const { return coarse_time_ns() < comparing_hit.coarse_time_ns(); };
  bool operator<=(const alcor_data &comparing_hit) const { return coarse_time_ns() <= comparing_hit.coarse_time_ns(); };
  bool operator>(const alcor_data &comparing_hit) const { return coarse_time_ns() > comparing_hit.coarse_time_ns(); };
  bool operator>=(const alcor_data &comparing_hit) const { return coarse_time_ns() >= comparing_hit.coarse_time_ns(); };

  //  I/O on root files
  void link_to_tree(TTree *input_tree);
  void write_to_tree(TTree *output_tree);
};

void alcor_data::link_to_tree(TTree *input_tree)
{
  if (!input_tree)
    return;
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
  if (!output_tree)
    return;
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

#ifdef __ROOTCLING__
#pragma link C++ struct alcor_data_struct + ;
#pragma link C++ class std::vector < alcor_data_struct> + ;
#endif