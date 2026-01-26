#include "../lib/framer.h"
#include "../lib/lightio.h"

const int frame_size = 256;

std::vector<std::string> devices = {
  "kc705-192",
  "kc705-193",
  "kc705-194",
  "kc705-195",
  "kc705-196",
  "kc705-197",
  "kc705-198",
  "kc705-207"
};

void
fillfine(std::string dirname, std::string outfilename = "finedata.root", unsigned int max_spill = kMaxUInt)
{

  std::map<int, TH2F *> h_fine_device;

  /** 
   ** BUILD INPUT FILE LIST
   **/

  std::vector<std::string> filenames;
  for (auto device : devices) {
    for (int ififo = 0; ififo < 25; ++ififo) {
      std::string filename = dirname + "/" + device + "/decoded/alcdaq.fifo_" + std::to_string(ififo) + ".root";
      filenames.push_back(filename);
    }
  }

  /** 
   ** INITIALIZE FRAMER AND PROCESS
   **/

  std::cout << " --- initialize framer: frame size = " << frame_size << std::endl;
  sipm4eic::framer framer(filenames, frame_size);
  framer.set_trigger_coarse_offset(192, 112);
  
  /** loop over spills **/
  int n_spills = 0, n_frames = 0;
  for (int ispill = 0; ispill < max_spill && framer.next_spill(); ++ispill) {
    std::cout << " --- new spill: " << ispill << std::endl;

    /** loop over frames **/
    for (auto &frame : framer.frames()) {
      auto iframe = frame.first;
      auto aframe = frame.second;

      /** fill hits **/
      for (auto &device : aframe) {
	auto idevice = device.first;
	auto adevice = device.second;
	for (auto &chip : adevice.hits) {
	  auto ichip = chip.first;
	  auto achip = chip.second;
	  for (auto &channel : achip) {
	    auto ichannel = channel.first;
	    auto hits = channel.second;
	    for (auto &hit : hits) {

              auto device = idevice;
              if (!h_fine_device.count(device))
                h_fine_device[device] = new TH2F(Form("hFine_%d", device), "hFine", 768, 0, 768, 256, 0, 256);

	      auto fine = hit.fine;
              auto index = hit.device_index();
              auto tdc = hit.tdc;
              auto cindex = tdc + 4 * index;
              h_fine_device[device]->Fill(cindex, fine);          

	    }}}
	
      } /** end of loop over devices and hits **/
    } /** end of loop over frames **/
    
    std::cout << "     spill completed " << std::endl;
  } /** end of loop over spills **/

  std::cout << " --- writing output file: " << outfilename << std::endl;
  auto fout = TFile::Open(outfilename.c_str(), "RECREATE");
  for (auto &h : h_fine_device)
    h.second->Write();
  fout->Close();  

  std::cout << " --- completed " << std::endl;

}
