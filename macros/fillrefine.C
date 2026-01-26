#include "../lib/lightio.h"

void
fillrefine(std::string lightdata_infilename, std::string finecalib_infilename, std::string refinedata_outfilename, bool correct = false)
{
  
  sipm4eic::lightio io;
  io.read_from_tree(lightdata_infilename);
  sipm4eic::lightdata::load_fine_calibration(finecalib_infilename);

  /** create output sparse hitogram **/  
  const Int_t ndims = 4; // device, cindex, fine, delta
  Int_t bins[ndims]    = {   16,  768, 128,  1024  };
  Double_t xmin[ndims] = { 192.,   0.,   0.,   -8. };
  Double_t xmax[ndims] = { 208., 768., 128.,    8. };
  THnSparse* hRefine = new THnSparseD("hRefine", "hRefine", ndims, bins, xmin, xmax);  
  
  while (io.next_spill()) {
    while (io.next_frame()) {
      
      auto timing_vector = io.get_timing_vector();
      auto cherenkov_vector = io.get_cherenkov_vector();
      
      /** collect timing hits **/
      std::map<int, sipm4eic::lightdata> timing_hits;
      for (auto &hit : timing_vector) {
	auto index = hit.index;
	if (timing_hits.count(index) && timing_hits[index].time() < hit.time())
	  continue;
	timing_hits[index] = hit;
      }
      
      /** compute reference time **/
      int Nref = timing_hits.size();
      float Tref = 0.;
      for (auto &[index, hit] : timing_hits)
	Tref += hit.time();
      Tref /= Nref;

      /** fill histogram **/
      for (auto &device_vector : {timing_vector, cherenkov_vector}) {
	for (auto &hit : device_vector) {
	  auto T = Tref;

	  /** compute reference time excluding this channel if included in timing **/
	  auto index = hit.index;
	  if (hit.device == 207 && timing_hits.count(index))
	    T = (Tref * Nref - timing_hits[index].time()) / (Nref - 1);
     
	  double delta = hit.coarse - T;
          if (correct) delta = hit.time() - T;
	  hRefine->Fill(hit.device, hit.cindex(), hit.fine, delta);

	}
      }      
    }
  }
  
  /** write output **/
  auto fout = TFile::Open(refinedata_outfilename.c_str(), "RECREATE");
  std::cout << " --- written refinedata: " << refinedata_outfilename << std::endl;
  hRefine->Write();
  fout->Close();
}

TH2*
getrefine(std::string refinedata_infilename, int device, int cindex)
{
  auto fin = TFile::Open(refinedata_infilename.c_str());
  auto hin = (THnSparse *)fin->Get("hRefine");
  auto di = device - 192;
  auto ci = cindex;
  hin->GetAxis(0)->SetRange(di + 1, di + 1);
  hin->GetAxis(1)->SetRange(ci + 1, ci + 1);
  return hin->Projection(3, 2); 
}
