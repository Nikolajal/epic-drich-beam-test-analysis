#include "../lib/framer.h"
#include "../lib/lightio.h"

// #define TESTBEAM2023
// #define TESTBEAM2024
#define TESTBEAM2025
#define TRIGGER_OFFSET

const int frame_size = 256; // 256 is golden for TB

bool apply_minimal_selection = true;
bool apply_trigger0_selection = false; // should be true
bool apply_trigger1_selection = false;
bool apply_trigger2_selection = false;
bool apply_trigger3_selection = false;
bool apply_timing_selection_OR = false;
bool apply_timing_selection_AND = false;
bool apply_tracking_selection_OR = false;
bool apply_tracking_selection_AND = false;

#if defined(TESTBEAM2023)
int TRIGGER0_device = 192;
int TRIGGER1_device = 0;
int TRIGGER2_device = 0;
int TRIGGER3_device = 0;
int TRIGGER0_offset = 112;
int TRIGGER1_offset = 0;
int TRIGGER2_offset = 0;
int TRIGGER3_offset = 0;
#elif defined(TESTBEAM2024)
int TRIGGER0_device = 193;
int TRIGGER1_device = 194;
int TRIGGER2_device = 195;
int TRIGGER3_device = 196;
int TRIGGER0_offset = 105;
int TRIGGER1_offset = 105;
int TRIGGER2_offset = 105;
int TRIGGER3_offset = 107;
#else
int TRIGGER0_device = 196;
int TRIGGER1_device = 197;
int TRIGGER2_device = 0;
int TRIGGER3_device = 0;
int TRIGGER0_offset = 112;
int TRIGGER1_offset = 122;
int TRIGGER2_offset = 0;
int TRIGGER3_offset = 0;
#endif

#if defined(TESTBEAM2023)
int TIMING1_device = 207, TIMING1_chip = 4;
int TIMING2_device = 207, TIMING2_chip = 5;
#elif defined(TESTBEAM2024)
int TIMING1_device = 200, TIMING1_chip = 2;
int TIMING2_device = 200, TIMING2_chip = 4;
#else
int TIMING1_device = 200, TIMING1_chip = 0;
int TIMING2_device = 200, TIMING2_chip = 2;
#endif

int TRACKING1_device = 200, TRACKING1_chip = 3;
int TRACKING2_device = 200, TRACKING2_chip = 5;

#if defined(TESTBEAM2024)
std::vector<std::string> devices = {
    "kc705-192",
    "kc705-193",
    "kc705-194",
    "kc705-195",
    "kc705-196",
    "kc705-197",
    "kc705-198",
    "kc705-199",
    "kc705-200",
    "kc705-201",
    "kc705-202",
    "kc705-203"};
#elif defined(TESTBEAM2025)
std::vector<std::string> devices = {
    "rdo-192",
    /*"rdo-193",*/
    "rdo-194",
    "rdo-195",
    "rdo-196",
    "rdo-197",
    "rdo-198",
    "rdo-199",
    "kc705-200"};
#endif

void lightwriter(std::vector<std::string> filenames, std::string outfilename, std::string fineoutfilename, unsigned int max_spill = 50, bool verbose = false)
{

  auto io = new sipm4eic::lightio;
  io->write_to_tree(outfilename);

  std::map<int, TH2F *> h_fine_device;

  std::cout << " --- initialize framer: frame size = " << frame_size << std::endl;
  sipm4eic::framer framer(filenames, frame_size);
  framer.verbose(verbose);

#ifdef TRIGGER_OFFSET
  framer.set_trigger_coarse_offset(TRIGGER0_device, TRIGGER0_offset);
  framer.set_trigger_coarse_offset(TRIGGER1_device, TRIGGER1_offset);
  framer.set_trigger_coarse_offset(TRIGGER2_device, TRIGGER2_offset);
  framer.set_trigger_coarse_offset(TRIGGER3_device, TRIGGER3_offset);
#endif

  /** loop over spills **/
  int n_spills = 0, n_frames = 0;
  for (int ispill = 0; ispill < max_spill && framer.next_spill(); ++ispill)
  {
    /**
     ** FINE FILL
     **/

    /** loop over frames **/
    for (auto &frame : framer.frames())
    {
      auto iframe = frame.first;
      auto aframe = frame.second;

      /** fill hits **/
      for (auto &device : aframe)
      {
        auto idevice = device.first;
        auto adevice = device.second;
        for (auto &chip : adevice.hits)
        {
          auto ichip = chip.first;
          auto achip = chip.second;
          for (auto &channel : achip)
          {
            auto ichannel = channel.first;
            auto hits = channel.second;
            for (auto &hit : hits)
            {

              auto device = idevice;
              if (!h_fine_device.count(device))
                h_fine_device[device] = new TH2F(Form("hFine_%d", device), "hFine", 768, 0, 768, 256, 0, 256);

              auto fine = hit.fine;
              auto index = hit.device_index();
              auto tdc = hit.tdc;
              auto cindex = tdc + 4 * index;
              h_fine_device[device]->Fill(cindex, fine);
            }
          }
        }

      } /** end of loop over devices and hits **/
    } /** end of loop over frames **/

    /**
     ** LIGHT DATA
     **/

    io->new_spill(ispill);

    for (auto &part : framer.part_mask())
    {
      auto idevice = part.first;
      auto amask = part.second;
      io->add_part(idevice, amask);
    }
    for (auto &dead : framer.dead_mask())
    {
      auto idevice = dead.first;
      auto amask = dead.second;
      io->add_dead(idevice, amask);
    }

    /** loop over frames **/
    for (auto &frame : framer.frames())
    {
      auto iframe = frame.first;
      auto aframe = frame.second;

      io->new_frame(iframe);

      /** information to define selections **/
      auto TRIGGER0_n = aframe[TRIGGER0_device].triggers.size();
      auto TRIGGER1_n = aframe[TRIGGER1_device].triggers.size();
      auto TRIGGER2_n = aframe[TRIGGER2_device].triggers.size();
      auto TRIGGER3_n = aframe[TRIGGER3_device].triggers.size();
      auto TIMING1_n = aframe[TIMING1_device].hits[TIMING1_chip].size();
      auto TIMING2_n = aframe[TIMING2_device].hits[TIMING2_chip].size();
      auto TRACKING1_n = aframe[TRACKING1_device].hits[TRACKING1_chip].size();
      auto TRACKING2_n = aframe[TRACKING2_device].hits[TRACKING2_chip].size();

      /** minimal selection **/
      if (apply_minimal_selection &&
          (TRIGGER0_n == 0 && TRIGGER1_n == 0 && TRIGGER2_n == 0 && TRIGGER3_n == 0 &&
           TIMING1_n == 0 && TIMING2_n == 0 &&
           TRACKING1_n == 0 && TRACKING2_n == 0))
        continue;

      /** selection on Luca-AND trigger **/
      if (apply_trigger0_selection && TRIGGER0_n == 0)
        continue;

      /** selection on Luca-GEM trigger **/
      if (apply_trigger1_selection && TRIGGER1_n == 0)
        continue;

      /** selection on low-pressure Cherenkov trigger **/
      if (apply_trigger2_selection && TRIGGER2_n == 0)
        continue;

      /** selection on high-pressure Cherenkov trigger **/
      if (apply_trigger3_selection && TRIGGER3_n == 0)
        continue;

      /** selection on timing scintillators **/
      if (apply_timing_selection_OR && (TIMING1_n == 0 && TIMING2_n == 0))
        continue;
      if (apply_timing_selection_AND && (TIMING1_n == 0 || TIMING2_n == 0))
        continue;

      /** selection on tracking matrices **/
      if (apply_tracking_selection_OR && (TRACKING1_n == 0 && TRACKING2_n == 0))
        continue;
      if (apply_tracking_selection_AND && (TRACKING1_n == 0 || TRACKING2_n == 0))
        continue;

      /** fill trigger0 hits **/
      auto trigger0 = aframe[TRIGGER0_device].triggers;
      for (auto &trigger : trigger0)
        io->add_trigger0(trigger.coarse_time_clock() - iframe * frame_size);

      /** fill trigger1 hits **/
      auto trigger1 = aframe[TRIGGER1_device].triggers;
      for (auto &trigger : trigger1)
        io->add_trigger1(trigger.coarse_time_clock() - iframe * frame_size);

      /** fill trigger2 hits **/
      auto trigger2 = aframe[TRIGGER2_device].triggers;
      for (auto &trigger : trigger2)
        io->add_trigger2(trigger.coarse_time_clock() - iframe * frame_size);

      /** fill trigger3 hits **/
      auto trigger3 = aframe[TRIGGER3_device].triggers;
      for (auto &trigger : trigger3)
        io->add_trigger3(trigger.coarse_time_clock() - iframe * frame_size);

      /*
    // fill timing hits
    for (auto &chip : aframe[207].hits)
    {
      auto ichip = chip.first;
      auto achip = chip.second;
      for (auto &channel : achip)
      {
        auto ichannel = channel.first;
        auto hits = channel.second;
        for (auto &hit : hits)
        {
          auto coarse = hit.coarse_time_clock() - iframe * frame_size;
          io->add_timing(207, hit.device_index(), coarse, hit.fine, hit.tdc);
        }
      }
    }
    */

      /** fill hits **/
      for (auto &device : aframe)
      {
        auto idevice = device.first;
        auto adevice = device.second;
        for (auto &chip : adevice.hits)
        {
          auto ichip = chip.first;
          auto achip = chip.second;
          for (auto &channel : achip)
          {
            auto ichannel = channel.first;
            auto hits = channel.second;
            for (auto &hit : hits)
            {
              auto coarse = hit.coarse_time_clock() - iframe * frame_size;
              if ((idevice == TIMING1_device && ichip == TIMING1_chip) ||
                  (idevice == TIMING2_device && ichip == TIMING2_chip))
                io->add_timing(idevice, hit.device_index(), coarse, hit.fine, hit.tdc);
              else if ((idevice == TRACKING1_device && ichip == TRACKING1_chip) ||
                       (idevice == TRACKING2_device && ichip == TRACKING2_chip))
                io->add_tracking(idevice, hit.device_index(), coarse, hit.fine, hit.tdc);
              else
                io->add_cherenkov(idevice, hit.device_index(), coarse, hit.fine, hit.tdc);
            }
          }
        }

      } // end of loop over devices and hits

      io->add_frame();
      ++n_frames;

    } // end of loop over frames

    io->fill();
    ++n_spills;

  } // end of loop over spills

  std::cout << " --- writing light data output file: " << outfilename << std::endl;
  io->write_and_close();

  if (!fineoutfilename.empty())
  {
    std::cout << " --- writing fine data output file: " << fineoutfilename << std::endl;
    auto fout = TFile::Open(fineoutfilename.c_str(), "RECREATE");
    for (auto &h : h_fine_device)
      h.second->Write();
    fout->Close();
  }

  std::cout << " --- completed: " << n_spills << " spills " << std::endl;
  std::cout << "                " << n_frames << " frames written " << std::endl;
}

void lightwriter(std::string dirname, std::string outfilename, std::string fineoutfilename, unsigned int max_spill = kMaxUInt, bool verbose = false)
{

  std::vector<std::string> filenames;
  for (auto device : devices)
  {
    for (int ififo = 0; ififo < 32; ++ififo)
    {
      std::string filename = dirname + "/" + device + "/decoded/alcdaq.fifo_" + std::to_string(ififo) + ".root";
      filenames.push_back(filename);
    }
    std::string filename = dirname + "/" + device + "/decoded/alcdaq.fifo_" + std::to_string(99) + ".root";
    filenames.push_back(filename);
  }

  lightwriter(filenames, outfilename, fineoutfilename, max_spill, verbose);
}

void lightwriter(std::string runname, unsigned int max_spill = kMaxUInt, bool verbose = false)
{
  lightwriter("./Data/" + runname + "/", "./Data/" + runname + "/lightdata_old.root", "./Data/" + runname + "/finedata.root", max_spill, verbose);
}
