#pragma once

#include <cstdint>
#include <vector>
#include <random>
#include <unordered_set>
#include <filesystem>
#include <Math/Functor.h>
#include <Fit/Fitter.h>
#include <TEllipse.h>
#include <TH1.h>
#include <TFile.h>
#include <TTree.h>
#include <TF1.h>
#include <Math/Minimizer.h>
#include <functional>
#include <iostream>
#include <TFile.h>

/**
 * @brief Encode a single bit into a 32-bit mask.
 * @param active_bit Index of the bit to set (0..31)
 * @return 32-bit mask with only that bit set
 */
inline uint32_t encode_bit(uint8_t active_bit)
{
  return (active_bit < 32) ? (1u << active_bit) : 0;
}

/**
 * @brief Encode multiple bits into a 32-bit mask.
 * @param active_bits Vector of bit indices to set (0..31)
 * @return 32-bit mask with all specified bits set
 */
inline uint32_t encode_bits(const std::vector<uint8_t> &active_bits)
{
  uint32_t mask = 0;
  for (uint8_t bit : active_bits)
    if (bit < 32)
      mask |= (1u << bit);
  return mask;
}

/**
 * @brief Count trailing zeros (portable C++17)
 * @param mask 32-bit mask
 * @return Index of least significant set bit, 32 if mask is 0
 */
inline uint8_t count_trailing_zeros(uint32_t mask)
{
  if (mask == 0)
    return 32;
  uint8_t count = 0;
  while ((mask & 1u) == 0)
  {
    mask >>= 1;
    ++count;
  }
  return count;
}

/**
 * @brief Decode a 32-bit mask into the indices of set bits.
 * @param mask 32-bit mask
 * @return Vector of indices where bits are set
 */
inline std::vector<uint8_t> decode_bits(uint32_t mask)
{
  std::vector<uint8_t> result;
  result.resize(32, 0);
  while (mask)
  {
    uint8_t bit = count_trailing_zeros(mask);
    result.push_back(bit);
    mask &= ~(1u << bit); // clear that bit
  }

  return result;
}

class Logger
{
public:
  enum class colour_tag : int
  {
    BLACK = 30,
    RED = 31,
    GREEN = 32,
    YELLOW = 33,
    BLUE = 34,
    MAGENTA = 35,
    CYAN = 36,
    WHITE = 37,
    BRIGHT_BLACK = 90,
    BRIGHT_RED = 91,
    BRIGHT_GREEN = 92,
    BRIGHT_YELLOW = 93,
    BRIGHT_BLUE = 94,
    BRIGHT_MAGENTA = 95,
    BRIGHT_CYAN = 96,
    BRIGHT_WHITE = 97,
    RESET = 0
  };
  enum class style_tag : int
  {
    NONE = 0,
    BOLD = 1,
    DIM = 2,
    ITALIC = 3,
    UNDERLINE = 4,
    BLINK_SLOW = 5,
    BLINK_FAST = 6,
    REVERSED = 7,
    HIDDEN = 8,
    STRIKETHROUGH = 9
  };

  enum class level_tag
  {
    ERROR,
    WARNING,
    INFO,
    DEBUG
  };

  using style_list = std::vector<style_tag>;
  using text_config = std::pair<colour_tag, style_list>;

  static std::string ansi(colour_tag colour = colour_tag::RESET, style_list styles = {style_tag::NONE})
  {
    std::string code = "";
    if (!styles.empty())
      for (auto current_style : styles)
        if (static_cast<int>(current_style) != 0) // skip NONE
          code += std::to_string(static_cast<int>(current_style)) + ";";
    code += std::to_string(static_cast<int>(colour)); // append colour
    return "\033[" + code + "m";
  }
  static void log(const std::string &msg, colour_tag c = colour_tag::RESET, style_list s = {style_tag::NONE}) { std::cout << ansi(c, s) << msg << ansi() << "\n"; }
};

// TODO clean-up & incorporate into second repository for general utilities
//  Random generator utility
//  --- Create a random device for seeding
inline std::random_device _global_rd_;
//  --- Create a Mersenne Twister engine (fast, high quality)
inline std::mt19937 _global_gen_(_global_rd_());
//  --- Float distribution in [0,1)
inline std::uniform_real_distribution<> _rnd_(0.0, 1.0); // floats [0,1)

//  Index utility
//  TODO Should be moved to mapping.h as it heavily depends on the mapping and requested device, chip, pdu, etc
inline int get_hit_tdc_from_global_tdc_index(int global_tdc_index) { return global_tdc_index % 4; }
inline int get_device_from_global_tdc_index(int global_tdc_index) { return 192 + (global_tdc_index / 1024); }
inline int get_fifo_from_global_tdc_index(int global_tdc_index) { return (global_tdc_index % 1024) / 32; }
inline int get_chip_from_global_tdc_index(int global_tdc_index) { return get_fifo_from_global_tdc_index(global_tdc_index) / 4; }
inline int get_eo_channel_index_from_global_tdc_index(int global_tdc_index) { return ((global_tdc_index % 1024) % (32 * 4)) / 4 + 32 * (get_chip_from_global_tdc_index(global_tdc_index) % 2); }
inline int get_column_from_global_tdc_index(int global_tdc_index) { return ((global_tdc_index % 1024) % 32) / 4; }
inline int get_pixel_from_global_tdc_index(int global_tdc_index) { return ((global_tdc_index % 1024) % 32) % 4; }
inline int get_calib_index_from_global_tdc_index(int global_tdc_index) { return get_hit_tdc_from_global_tdc_index(global_tdc_index) + 4 * get_eo_channel_index_from_global_tdc_index(global_tdc_index) + 128 * get_chip_from_global_tdc_index(global_tdc_index); }
inline int get_device_index_from_global_tdc_index(int global_tdc_index) { return get_eo_channel_index_from_global_tdc_index(global_tdc_index) + 64 * (get_chip_from_global_tdc_index(global_tdc_index) / 2); }
inline int get_pdu_from_global_tdc_index(int global_tdc_index) { return global_tdc_index / 256 + 1; }
inline int get_matrix_from_global_tdc_index(int global_tdc_index) { return (global_tdc_index % 256 / 4) + 1; }
inline uint32_t get_global_index(int device, int chip, int channel, int tdc) { return (device - 192) * 32 * 8 * 4 + chip * 32 * 4 + channel * 4 + tdc; }

//  Fit circle
//  --- Fit results: X, Y, R and errors
using circle_fit_results = std::array<std::array<float, 2>, 3>;
//  --- Fit function
inline circle_fit_results fit_circle(std::vector<std::array<float, 2>> points, std::array<float, 3> initial_values, bool fix_XY = true, std::vector<int> exclude_points = {{}})
{
  circle_fit_results result;

  //  Chi2 minimisation for points in a circle
  auto chi2_function = [&](const double *parameters)
  {
    float chi2 = 0;
    for (int iPnt = 0; iPnt < points.size(); iPnt++)
    {
      if (std::find(exclude_points.begin(), exclude_points.end(), iPnt) != exclude_points.end())
        continue;
      double delta_x = points[iPnt][0] - parameters[0];
      double delta_y = points[iPnt][1] - parameters[1];
      double delta_r = parameters[2] - std::sqrt(delta_x * delta_x + delta_y * delta_y);
      chi2 += delta_r * delta_r;
    }
    return chi2;
  };

  // wrap chi2 function in a function object for the fit
  ROOT::Math::Functor fit_function(chi2_function, 3);
  ROOT::Fit::Fitter fitter;

  //  Set initial values and variables names
  double internal_initial_values[3] = {initial_values[0], initial_values[1], initial_values[2]};
  fitter.SetFCN(fit_function, internal_initial_values);
  fitter.Config().ParSettings(0).SetName("x0");
  fitter.Config().ParSettings(1).SetName("y0");
  fitter.Config().ParSettings(2).SetName("R");
  fitter.Config().ParSettings(2).SetLowerLimit(0);
  if (fix_XY)
  {
    fitter.Config().ParSettings(0).Fix();
    fitter.Config().ParSettings(1).Fix();
  }

  //  Fitting
  if (!fitter.FitFCN())
  {
    // Error("fit_circle", "Fit failed");
    //  return {{{-2., 0.}, {-2., 0.}, {-2., 0.}}};
  }
  const ROOT::Fit::FitResult &fit_result = fitter.Result();

  auto iTer = -1;
  for (auto current_parameter : fit_result.Parameters())
  {
    iTer++;
    result[iTer][0] = current_parameter;
    result[iTer][1] = fit_result.Errors()[iTer];
  }

  //  Calculate chi2
  double *test = new double[3];
  test[0] = result[0][0];
  test[1] = result[1][0];
  test[2] = result[2][0];
  auto myChi2 = chi2_function(test);

  return result;
}

inline TFile *open_or_build_rootfile(const std::string &filename,
                                     std::function<void(std::string, std::string, int)> builder,
                                     const std::string &data_repository,
                                     const std::string &run_name,
                                     int max_spill,
                                     bool force_rebuild = false)
{
  //  Try to open the file
  if (!force_rebuild)
  {
    TFile *input_file = TFile::Open(filename.c_str(), "READ");
    if (input_file && !input_file->IsZombie())
      return input_file;
    delete input_file; // Delete if zombie
  }
  std::cout << "[WARNING] File '" << filename << "' missing, corrupted or rebuild forced, creating it\n";

  //  Re-build file
  builder(data_repository, run_name, max_spill);

  TFile *input_file = TFile::Open(filename.c_str(), "READ");
  if (!input_file || input_file->IsZombie())
  {
    std::cerr << "[ERROR] Could not open file '" << filename << "' even after rebuilding\n";
    delete input_file;
    return nullptr;
  }
  return input_file;
}

void inline draw_circle(std::array<float, 3> parameters, int line_colour = kBlack, int line_style = kSolid, int line_width = 1)
{
  auto result = new TEllipse(parameters[0], parameters[1], parameters[2]);
  result->SetFillStyle(0);
  result->SetLineColor(line_colour);
  result->SetLineStyle(line_style);
  result->SetLineWidth(line_width);
  result->DrawEllipse(parameters[0], parameters[1], parameters[2], 0, 0, 360, 0, "same");
}

// https://root-forum.cern.ch/t/fitting-a-poisson-distribution-to-a-histogram/12078/4
//  [0] = Normalizing parameter
//  [1] / [2] -> mean (mu)
//  x / [2] -> x
//  Gamma( x / [2] + 1 ) = factorial (x / [2])
// inline TF1 *fPhotonFitFunction = new TF1("hPhotonFitFunction", "[0]*TMath::Power(([1]/[2]),(x/[2]))*(TMath::Exp(-([1]/[2])))/TMath::Gamma((x/[2])+1)", -1000, 1000);

/*
//  Data structures
//  === Fit results: X, Y, R and errors
using circle_fit_results = std::array<std::array<float, 2>, 3>;
//  === recodata
//  === === Structure
struct recodata
{
  unsigned short n = 0;
  float x[1024];
  float y[1024];
  float t[1024];
};
//  === === Load data from tree
void load_data(TTree *reco_data_tree, recodata &target_data_struct)
{
  reco_data_tree->SetBranchAddress("n", &target_data_struct.n);
  reco_data_tree->SetBranchAddress("x", &target_data_struct.x);
  reco_data_tree->SetBranchAddress("y", &target_data_struct.y);
  reco_data_tree->SetBranchAddress("t", &target_data_struct.t);
}
//  === === Load data from file
TTree *load_data(std::string filename, recodata &target_data_struct)
{
  auto input_file = TFile::Open(filename.c_str());
  auto input_tree = (TTree *)input_file->Get("recodata");
  load_data(input_tree, target_data_struct);
  return input_tree;
}
//  === Devices
std::map<std::string, int> devices_name = {
    {"kc705-192", 192},
    {"kc705-193", 193},
    {"kc705-194", 194},
    {"kc705-195", 195},
    {"kc705-196", 196},
    {"kc705-197", 197},
    {"kc705-198", 198},
    {"kc705-199", 199},
    {"kc705-200", 200},
    {"kc705-201", 201},
    {"kc705-202", 202},
    {"kc705-207", 207}};
std::map<int, int> devices_enum = {
    {192, 0},
    {193, 1},
    {194, 2},
    {195, 3},
    {196, 4},
    {197, 5},
    {198, 6},
    {199, 7},
    {200, 8},
    {201, 9},
    {202, 10},
    {207, 11}};

// General Info
const float kSiPM_x_dimension = 1.5;
const float kSiPM_y_dimension = 1.5;

//  General utilities
//  === Cartesian to Polar
inline std::array<float, 2> cartesian_to_polar(std::array<float, 2> target, std::array<float, 2> center_shift = {0., 0.})
{
  float x_new_coordinate = target[0] - center_shift[0];
  float y_new_coordinate = target[1] - center_shift[1];
  float r_coordinate = TMath::Sqrt(x_new_coordinate * x_new_coordinate + y_new_coordinate * y_new_coordinate);
  float phi_coordinate = TMath::ATan2(y_new_coordinate, x_new_coordinate);
  return {r_coordinate, phi_coordinate};
}
inline std::array<float, 2> polar_to_cartesian(std::array<float, 2> target, std::array<float, 2> center_shift = {0., 0.})
{
  float r_new_coordinate = target[0] - center_shift[0];
  float phi_new_coordinate = target[1] - center_shift[1];
  float x_coordinate = target[0] * TMath::Cos(target[1]);
  float y_coordinate = target[0] * TMath::Sin(target[1]);
  return {x_coordinate, y_coordinate};
}
//  === Filler functions
bool is_within_SiPM(std::array<float, 2> cartesian_coordinates, std::array<float, 2> SiPM_center)
{
  bool is_within_SiPM = false;
  auto x_distance = fabs(cartesian_coordinates[0] - SiPM_center[0]);
  auto y_distance = fabs(cartesian_coordinates[1] - SiPM_center[1]);
  if ((x_distance) <= kSiPM_x_dimension && fabs(y_distance) <= kSiPM_y_dimension)
    is_within_SiPM = true;
  return is_within_SiPM;
}
bool is_within_ring(std::array<float, 2> cartesian_coordinates, std::array<float, 3> ring_parameters, float ring_radius_sigma = 0.)
{
  //  Improvements:
  //  Make X and Y have different possible values
  //  Check values received for the interval
  bool is_within_ring = false;
  auto target_radius = cartesian_to_polar(cartesian_coordinates, {ring_parameters[0], ring_parameters[1]})[0];
  if ((target_radius >= ring_parameters[2] - ring_radius_sigma) && (target_radius <= ring_parameters[2] + ring_radius_sigma))
    is_within_ring = true;
  return is_within_ring;
}
template <bool rnd_smearing = true>
void fill_persistance(TH2F *hTarget, recodata reco_data)
{
  for (int iPnt = 0; iPnt < reco_data.n; iPnt++)
    hTarget->Fill(rnd_smearing ? gRandom->Uniform(reco_data.x[iPnt] - 1.5, reco_data.x[iPnt] + 1.5) : reco_data.x[iPnt], rnd_smearing ? gRandom->Uniform(reco_data.y[iPnt] - 1.5, reco_data.y[iPnt] + 1.5) : reco_data.y[iPnt]);
}
template <bool allow_overlap = false>
void fill_with_SiPM_coverage(TH2F *target, std::array<float, 2> SiPM_center)
{
  float x_center_bin = target->GetXaxis()->FindBin(SiPM_center[0]);
  float y_center_bin = target->GetYaxis()->FindBin(SiPM_center[1]);
  float x_center_bin_center = target->GetXaxis()->GetBinCenter(x_center_bin);
  float y_center_bin_center = target->GetYaxis()->GetBinCenter(y_center_bin);
  float current_center_bin_content = target->GetBinContent(target->FindBin(x_center_bin, y_center_bin));

  //  Did we set this SiPM already?
  //  * No overlap is permitted
  if ((current_center_bin_content > 0) && !allow_overlap)
    return;

  //  Start with know center bin
  for (auto xBin = 0; xBin < target->GetNbinsX(); xBin++)
  {
    //  Check at least one fill has been done
    bool at_least_one_fill = false;

    float current_bin_center_x_high = (float)(target->GetXaxis()->GetBinCenter(x_center_bin + xBin));
    float current_bin_center_x_low_ = (float)(target->GetXaxis()->GetBinCenter(x_center_bin - xBin));
    for (auto yBin = 0; yBin < target->GetNbinsY(); yBin++)
    {
      float current_bin_center_y_high = (float)(target->GetYaxis()->GetBinCenter(y_center_bin + yBin));
      float current_bin_center_y_low_ = (float)(target->GetYaxis()->GetBinCenter(y_center_bin - yBin));

      //  Set bin contents
      if (is_within_SiPM({current_bin_center_x_high, current_bin_center_y_high}, SiPM_center))
      {
        target->SetBinContent(x_center_bin + xBin, y_center_bin + yBin, 1);
        at_least_one_fill = true;
      }
      if (is_within_SiPM({current_bin_center_x_high, current_bin_center_y_low_}, SiPM_center))
      {
        target->SetBinContent(x_center_bin + xBin, y_center_bin - yBin, 1);
        at_least_one_fill = true;
      }
      if (is_within_SiPM({current_bin_center_x_low_, current_bin_center_y_high}, SiPM_center))
      {
        target->SetBinContent(x_center_bin - xBin, y_center_bin + yBin, 1);
        at_least_one_fill = true;
      }
      if (is_within_SiPM({current_bin_center_x_low_, current_bin_center_y_low_}, SiPM_center))
      {
        target->SetBinContent(x_center_bin - xBin, y_center_bin - yBin, 1);
        at_least_one_fill = true;
      }

      //  return if we stopped filling
      if (!at_least_one_fill)
        break;
    }
    if (!is_within_SiPM({current_bin_center_x_high, y_center_bin_center}, SiPM_center) && !is_within_SiPM({current_bin_center_x_low_, y_center_bin_center}, SiPM_center))
      return;
  }
}
void fill_with_ring_coverage(TH2F *target, std::array<float, 3> ring_parameters, float ring_radius_sigma = 0.)
{
  for (auto xBin = 0; xBin < target->GetNbinsX(); xBin++)
  {
    float current_bin_center_x = (float)(target->GetXaxis()->GetBinCenter(xBin));
    for (auto yBin = 0; yBin < target->GetNbinsY(); yBin++)
    {
      float current_bin_center_y = float(target->GetYaxis()->GetBinCenter(yBin));
      if (is_within_ring({current_bin_center_x, current_bin_center_y}, ring_parameters, ring_radius_sigma))
      {
        target->SetBinContent(xBin, yBin, 1);
      }
      else
      {
        target->SetBinContent(xBin, yBin, 0);
      }
    }
  }
}
//  === Ring finders & fittera
std::vector<std::tuple<float, float, int>>
find_peaks(TH1F *original_histo, int bin_span = 3, float cutoff_threshold = 0.33)
{
  //  Result
  std::vector<std::tuple<float, float, int>> result;

  //  Clone target to manipulate
  auto target_histo = (TH1F *)(original_histo->Clone("tmp_get_filtered_center"));
  auto target_maximum = target_histo->GetMaximum();

  // Loop over bins
  for (auto iBin = 1 + bin_span; iBin <= target_histo->GetNbinsX() - bin_span; iBin++)
  {
    auto neighbourhood_maximum = 0;
    auto current_bin = target_histo->GetBinContent(iBin);
    for (auto iSpan = 1; iSpan <= bin_span; iSpan++)
    {
      auto current_bin_left = target_histo->GetBinContent(iBin - iSpan);
      auto current_bin_right = target_histo->GetBinContent(iBin + iSpan);
      if ((neighbourhood_maximum < current_bin_left) || (neighbourhood_maximum < current_bin_right))
        neighbourhood_maximum = max(current_bin_left, current_bin_right);
      if (neighbourhood_maximum > current_bin)
        break;
    }
    if ((neighbourhood_maximum < current_bin) && (current_bin > cutoff_threshold * target_maximum))
      result.push_back({target_histo->GetBinCenter(iBin), target_histo->GetBinContent(iBin), iBin});
  }

  delete target_histo;
  return result;
}
std::vector<std::vector<std::array<float, 2>>>
find_rings(TH1F *original_histo_X, TH1F *original_histo_Y, float x_center = -1.5, float y_center = -1.5)
{
  //  Clone target to manipulate
  auto target_histo_X = (TH1F *)(original_histo_X->Clone("tmp_find_rings_X"));
  auto target_histo_Y = (TH1F *)(original_histo_Y->Clone("tmp_find_rings_Y"));

  //  Find peaks
  auto current_peaks_X = find_peaks(target_histo_X);
  auto current_peaks_Y = find_peaks(target_histo_Y);

  //  ==  Assumption is made that the beam is within the smaller circle
  auto n_cirles = 0;
  auto current_circle = 0;
  std::vector<std::vector<std::array<float, 2>>> circles;

  //  First loop on x_findings
  for (auto current_peak : current_peaks_X)
  {
    auto current_X = get<0>(current_peak);
    if (current_X < 0)
    {
      n_cirles++;
      circles.push_back({{current_X, y_center}});
      current_circle++;
    }
    else
    {
      current_circle--;
      if (current_circle < 0)
      {
        current_circle = n_cirles;
        n_cirles++;
        circles.insert(circles.begin(), {{current_X, y_center}});
        continue;
      }
      circles[current_circle].push_back({current_X, y_center});
    }
  }

  //  Second loop on y_findings
  auto n_left_rings = 0;
  for (auto current_peak : current_peaks_Y)
  {
    auto current_Y = get<0>(current_peak);
    if (current_Y < 0)
      n_left_rings++;
  }
  if (n_left_rings == n_cirles)
  {
    current_circle = 0;
    for (auto current_peak : current_peaks_Y)
    {
      auto current_Y = get<0>(current_peak);
      if (current_Y < 0)
      {
        circles[current_circle].push_back({x_center, current_Y});
        current_circle++;
      }
      else
      {
        current_circle--;
        if (current_circle < 0)
        {
          current_circle = n_cirles;
          n_cirles++;
          circles.insert(circles.begin(), {{x_center, current_Y}});
          continue;
        }
        circles[current_circle].push_back({x_center, current_Y});
      }
    }
  }

  delete target_histo_X;
  delete target_histo_Y;
  std::reverse(circles.begin(), circles.end());
  return circles;
}
std::vector<std::vector<std::array<float, 2>>>
merge_circles(std::vector<std::vector<std::vector<std::array<float, 2>>>> target_circles)
{
  std::vector<std::vector<std::array<float, 2>>> final_circles;
  for (auto current_circle_array : target_circles)
  {
    auto icircle = -1;
    for (auto current_circle : current_circle_array)
    {
      icircle++;
      if (final_circles.size() < icircle + 1)
        final_circles.push_back({});
      for (auto current_point : current_circle)
      {
        final_circles[icircle].push_back(current_point);
      }
    }
  }
  return final_circles;
}
template <bool fix_XY = true>
circle_fit_results
fit_circle(TGraph *gTarget, std::array<float, 3> initial_values)
{
  circle_fit_results result;

  //  Chi2 minimisation for points in a circle
  auto chi2_function = [&](const double *parameters)
  {
    float chi2 = 0;
    for (int iPnt = 0; iPnt < gTarget->GetN(); iPnt++)
    {
      double delta_x = gTarget->GetPointX(iPnt) - parameters[0];
      double delta_y = gTarget->GetPointY(iPnt) - parameters[1];
      double delta_r = parameters[2] - std::sqrt(delta_x * delta_x + delta_y * delta_y);
      chi2 += delta_r * delta_r;
    }
    return chi2;
  };

  // wrap chi2 function in a function object for the fit
  ROOT::Math::Functor fit_function(chi2_function, 3);
  ROOT::Fit::Fitter fitter;

  //  Set initial values and variables names
  double internal_initial_values[3] = {initial_values[0], initial_values[1], initial_values[2]};
  fitter.SetFCN(fit_function, internal_initial_values);
  fitter.Config().ParSettings(0).SetName("x0");
  fitter.Config().ParSettings(1).SetName("y0");
  fitter.Config().ParSettings(2).SetName("R");
  fitter.Config().ParSettings(2).SetLowerLimit(0);
  if (fix_XY)
  {
    fitter.Config().ParSettings(0).Fix();
    fitter.Config().ParSettings(1).Fix();
  }

  //  Fitting
  if (!fitter.FitFCN())
  {
    // Error("fit_circle", "Fit failed");
    //  return {{{-2., 0.}, {-2., 0.}, {-2., 0.}}};
  }
  const ROOT::Fit::FitResult &fit_result = fitter.Result();

  auto iTer = -1;
  for (auto current_parameter : fit_result.Parameters())
  {
    iTer++;
    result[iTer][0] = current_parameter;
    result[iTer][1] = fit_result.Errors()[iTer];
  }

  //  Calculate chi2
  double *test = new double[3];
  test[0] = result[0][0];
  test[1] = result[1][0];
  test[2] = result[2][0];
  auto myChi2 = chi2_function(test);

  return result;
}
template <bool fix_XY = true>
circle_fit_results
fit_circles(std::vector<TGraphErrors *> gTargets, std::array<float, 2> center_guess, std::vector<float> radiuses_guesses)
{
  //   Result
  circle_fit_results result;

  if (gTargets.size() == 0)
    return result;

  if (gTargets.size() == 1)
    return fit_circle<false>(gTargets[0], {center_guess[0], center_guess[1], radiuses_guesses[0]});

  //  Chi2 minimisation for points in a circle
  auto chi2_function = [&](const double *parameters)
  {
    float chi2 = 0;
    auto iGraph = -1;
    for (auto current_graph : gTargets)
    {
      iGraph++;
      for (int iPnt = 0; iPnt < current_graph->GetN(); iPnt++)
      {
        double delta_x = current_graph->GetPointX(iPnt) - parameters[0];
        double delta_y = current_graph->GetPointY(iPnt) - parameters[1];
        double delta_r = parameters[2 + iGraph] - std::sqrt(delta_x * delta_x + delta_y * delta_y);
        chi2 += delta_r * delta_r;
      }
    }
    return chi2;
  };

  // wrap chi2 function in a function object for the fit
  ROOT::Math::Functor fit_function(chi2_function, 2 + gTargets.size());
  ROOT::Fit::Fitter fitter;

  //  Set initial values and variables names
  double *internal_initial_values = new double[2 + gTargets.size()];
  internal_initial_values[0] = center_guess[0];
  internal_initial_values[1] = center_guess[1];
  auto iTer = 1;
  for (auto current_radius : radiuses_guesses)
  {
    iTer++;
    if (iTer > (2 + gTargets.size()))
      break;
    internal_initial_values[iTer] = current_radius;
  }
  fitter.SetFCN(fit_function, internal_initial_values);
  fitter.Config().ParSettings(0).SetName("x0");
  fitter.Config().ParSettings(1).SetName("y0");

  iTer = -1;
  for (auto current_radius : radiuses_guesses)
  {
    iTer++;
    if (iTer >= gTargets.size())
      break;
    fitter.Config().ParSettings(2 + iTer).SetName(Form("R_%i", iTer));
    fitter.Config().ParSettings(2 + iTer).SetLowerLimit(0);
  }
  if (fix_XY)
  {
    fitter.Config().ParSettings(0).Fix();
    fitter.Config().ParSettings(1).Fix();
  }

  //  Fitting
  if (!fitter.FitFCN())
  {
    // Error("fit_circle", "Fit failed");
    //  return {{{-2., 0.}, {-2., 0.}, {-2., 0.}}};
  }
  const ROOT::Fit::FitResult &fit_result = fitter.Result();

  iTer = -1;
  for (auto current_parameter : fit_result.Parameters())
  {
    iTer++;
    result[iTer][0] = current_parameter;
    result[iTer][1] = fit_result.Errors()[iTer];
  }
  return result;
}
template <bool simultaneous_fit = true>
std::vector<circle_fit_results> fit_multiple_rings(std::vector<std::array<TH1F *, 2>> original_histo_s, std::vector<std::array<float, 2>> centers)
{
  std::vector<std::vector<std::vector<std::array<float, 2>>>> final_circles_array;
  auto iTer = -1;
  for (auto current_xy_pair : original_histo_s)
  {
    iTer++;
    final_circles_array.push_back(find_rings(current_xy_pair[0], current_xy_pair[1], centers[iTer][0], centers[iTer][1]));
  }

  auto final_circles = merge_circles(final_circles_array);

  std::vector<circle_fit_results> final_rings;
  std::vector<TGraphErrors *> gfinal_rings;
  auto current_n_circle = -1;
  for (auto current_circle : final_circles)
  {
    current_n_circle++;
    gfinal_rings.push_back(new TGraphErrors());
    for (auto current_point : current_circle)
    {
      gfinal_rings[current_n_circle]->SetPoint(gfinal_rings[current_n_circle]->GetN(), current_point[0], current_point[1]);
    }
    auto circle_fit = fit_circle<false>(gfinal_rings[current_n_circle], {0, 0, 20});
    final_rings.push_back(circle_fit);
  }

  if (simultaneous_fit)
  {
    auto fit_results = fit_circles<false>(gfinal_rings, {0, 0}, {100, 100, 100, 100, 100});
    auto iRing = -1;
    for (auto &current_ring : final_rings)
    {
      iRing++;
      current_ring[0][0] = fit_results[0][0];
      current_ring[1][0] = fit_results[1][0];
      current_ring[0][1] = fit_results[0][1];
      current_ring[1][1] = fit_results[1][1];
      current_ring[2][0] = fit_results[2 + iRing][0];
      current_ring[2][1] = fit_results[2 + iRing][1];
    }
  }

  return final_rings;
}
template <bool simultaneous_fit = true>
std::vector<circle_fit_results> fit_multiple_rings(TH2F *persistance_2D, std::vector<std::array<float, 2>> slices = {{-0., +0.}, {-12., -12.}, {+12., +12.}})
{
  //  Clone target to manipulate
  auto target_persistance_2D = (TH2F *)(persistance_2D->Clone("target_persistance_2D"));

  //  Define utility functions
  std::vector<std::array<TH1F *, 2>> original_histo_s;
  std::vector<std::array<float, 2>> centers;

  //  Iteration for requested X-Y slices
  auto iTer = -1;
  for (auto current_limits : slices)
  {
    iTer++;

    auto x_target_bin = target_persistance_2D->GetXaxis()->FindBin(current_limits[0]);
    auto y_target_bin = target_persistance_2D->GetXaxis()->FindBin(current_limits[1]);

    auto x_target_bin_center = target_persistance_2D->GetXaxis()->GetBinCenter(x_target_bin);
    auto y_target_bin_center = target_persistance_2D->GetYaxis()->GetBinCenter(y_target_bin);

    auto hXslice = (TH1F *)(target_persistance_2D->ProjectionX(Form("current_projection_X_%i", iTer), x_target_bin, x_target_bin));
    auto hYslice = (TH1F *)(target_persistance_2D->ProjectionY(Form("current_projection_Y_%i", iTer), y_target_bin, y_target_bin));

    original_histo_s.push_back({hXslice, hYslice});
    centers.push_back({(float)(x_target_bin_center), (float)(y_target_bin_center)});
  }

  auto result = fit_multiple_rings<simultaneous_fit>(original_histo_s, centers);
  for (auto current_histos : original_histo_s)
  {
    delete current_histos[0];
    delete current_histos[1];
  }
  delete target_persistance_2D;

  return result;
}
//  === Graphical helpers
TCanvas *get_std_canvas()
{
  TCanvas *result = new TCanvas("", "", 800, 800);
  gStyle->SetOptStat(1110);
  result->SetRightMargin(0.15);
  result->SetTopMargin(0.15);
  result->SetLeftMargin(0.15);
  result->SetBottomMargin(0.15);
  return result;
}
template <bool grid_x = true, bool grid_y = true>
TCanvas *get_std_canvas_2D(std::string name, std::string title, float nXpixels = 1145, float nYpixels = 1000)
{
  TCanvas *cResult = new TCanvas(name.c_str(), title.c_str(), nXpixels, nYpixels);
  gStyle->SetOptStat(0);
  gPad->SetGridx(grid_x);
  gPad->SetGridy(grid_y);
  gPad->SetRightMargin(0.15);
  gPad->SetTopMargin(0.05);
  gPad->SetLeftMargin(0.15);
  gPad->SetBottomMargin(0.15);
  return cResult;
}
void plot_box(std::array<float, 4> parameters, int line_colour = kBlack, int line_style = kSolid, int line_width = 1)
{
  auto result = new TBox(parameters[0], parameters[1], parameters[2], parameters[3]);
  result->SetFillStyle(0);
  result->SetLineColor(line_colour);
  result->SetLineStyle(line_style);
  result->SetLineWidth(line_width);
  result->DrawBox(parameters[0], parameters[1], parameters[2], parameters[3]);
}
void plot_circle(std::array<float, 3> parameters, int line_colour = kBlack, int line_style = kSolid, int line_width = 1)
{
  auto result = new TEllipse(parameters[0], parameters[1], parameters[2]);
  result->SetFillStyle(0);
  result->SetLineColor(line_colour);
  result->SetLineStyle(line_style);
  result->SetLineWidth(line_width);
  result->DrawEllipse(parameters[0], parameters[1], parameters[2], 0, 0, 360, 0, "same");
}
void plot_circle(std::array<std::array<float, 2>, 3> parameters, std::array<std::array<int, 3>, 3> plot_options)
{
  std::array<float, 3> reduced_parameters = {parameters[0][0], parameters[1][0], parameters[2][0]};
  plot_circle(reduced_parameters, plot_options[0][0], plot_options[0][1], plot_options[0][2]);
  reduced_parameters = {parameters[0][0], parameters[1][0], parameters[2][0] + parameters[2][1]};
  plot_circle(reduced_parameters, plot_options[1][0], plot_options[1][1], plot_options[1][2]);
  reduced_parameters = {parameters[0][0], parameters[1][0], parameters[2][0] - parameters[2][1]};
  plot_circle(reduced_parameters, plot_options[1][0], plot_options[1][1], plot_options[1][2]);
  std::array<float, 4> box_parameters = {parameters[0][0] - parameters[0][1], parameters[1][0] - parameters[1][1], parameters[0][0] + parameters[0][1], parameters[1][0] + parameters[1][1]};
  plot_box(box_parameters, plot_options[2][0], plot_options[2][1], plot_options[2][2]);
}
TCanvas *plot_check_coordinates(TH2F *hPersistance2D, std::vector<circle_fit_results> found_rings)
{
  TCanvas *cDrawResult = get_std_canvas_2D("cDrawResult", "cDrawResult", 1145 * 2, 1000);
  cDrawResult->Divide(2, 1);
  TLatex *lLatex = new TLatex();
  cDrawResult->cd(2);
  gPad->SetRightMargin(0.15);
  gPad->SetTopMargin(0.05);
  gPad->SetLeftMargin(0.15);
  gPad->SetBottomMargin(0.15);
  cDrawResult->cd(1);
  gPad->SetRightMargin(0.15);
  gPad->SetTopMargin(0.05);
  gPad->SetLeftMargin(0.15);
  gPad->SetBottomMargin(0.15);
  hPersistance2D->Draw("COLZ");
  auto iRing = -1;
  for (auto current_ring : found_rings)
  {
    iRing++;

    cDrawResult->cd(1);
    std::array<std::array<int, 3>, 3> plot_options = {{{kBlack, kSolid, 3}, {kRed, kDashed, 2}, {kRed, kSolid, 2}}};
    std::array<std::array<float, 2>, 3> plot_coordinates = {{{current_ring[0][0], current_ring[0][1]}, {current_ring[1][0], current_ring[1][1]}, {current_ring[2][0], current_ring[2][1]}}};
    plot_circle(plot_coordinates, plot_options);

    cDrawResult->cd(2);

    lLatex->SetTextSize(0.045);
    lLatex->DrawLatexNDC(0.02, 0.95 + iRing * 0.25, Form("Ring %i", iRing));

    lLatex->SetTextSize(0.03);
    lLatex->DrawLatexNDC(0.02, 0.91 + iRing * 0.25, Form("Guess (mm):"));
    lLatex->DrawLatexNDC(0.18, 0.91 + iRing * 0.25, Form("X_{0} : %.2f#pm%.2f", current_ring[0][0], current_ring[0][1]));
    lLatex->DrawLatexNDC(0.36, 0.91 + iRing * 0.25, Form("Y_{0} : %.2f#pm%.2f", current_ring[1][0], current_ring[1][1]));
    lLatex->DrawLatexNDC(0.54, 0.91 + iRing * 0.25, Form("R_{0} : %.2f#pm%.2f", current_ring[2][0], current_ring[2][1]));
    lLatex->DrawLatexNDC(0.72, 0.91 + iRing * 0.25, Form("#sigma_{R} : %.2f#pm%.2f", 0., 0.));
  }
  return cDrawResult;
}
*/