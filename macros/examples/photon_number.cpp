#include "../lib_loader.h"

/**
 * @file ring_spatial_resolution.cpp
 * @brief Calculate the spatial resolution of the ring.
 *
 * This exercise estimates the center and radius of a ring of hits and then
 * computes the spatial resolution using multiple methods.
 *
 * @details
 * **Workflow:**
 * 1. **Initial ring center and radius estimation**
 *    - Select points tagged as "ring-like" by a density scan algorithm
 *      (based on time coincidence and radial proximity).
 *    - Filter hits that are **not labeled as After-Pulses (APs)** and within a
 *      reasonable time window relative to the trigger.
 *    - Fit the selected points with a circle to determine the center `(x_0, y_0)`
 *      and radius `R`.
 *    - Fixing the center will aid the subsequent resolution calculation.
 *
 * 2. **Spatial resolution calculation** (three methods):
 *    - **Method 1: Variable resolution vs. participant hits**
 *      - Fit points assigned to the ring.
 *      - Plot resulting radius as a function of the number of participant hits (photo-electrons, p.e.).
 *
 *    - **Method 2: Single Photon Spatial Resolution (SPSR)**
 *      - Fit points assigned to the ring.
 *      - Remove one point at a time.
 *      - Compute the difference in radius between the fit result and the removed point.
 *
 *    - **Method 3: SPSR alternative**
 *      - Compute the difference in radius between the first-round radius result
 *        and each selected point.
 *
 * **Notes:**
 * - Method 1 provides variable resolution as a function of participants (p.e.).
 * - Methods 2 and 3 provide the spatial resolution for a single photon.
 *
 * @author Nicola Rubini
 */

void photon_number(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
  //  Input files
  std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";

  //  Load recodata, return if not available
  TFile *input_file_recodata = new TFile(input_filename_recodata.c_str());
  if (!input_file_recodata || input_file_recodata->IsZombie())
  {
    std::cerr << "[WARNING] Could not find recodata, making it" << std::endl;
    return;
  }

  //  Link recodata tree locally
  TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
  alcor_recodata *recodata = new alcor_recodata();
  recodata->link_to_tree(recodata_tree);

  //  Get number of frames, limited to maximum requested frames
  auto n_frames = recodata_tree->GetEntries();
  auto all_frames = min((int)n_frames, (int)max_frames);

  //  Work in progress, stay tuned for updates!
}