inline double calculate_angle(double detector_to_telescope_plane, double pixel_position)
{
  return atan(pixel_position / detector_to_telescope_plane);
}
double calculate_angle_resolution(double detector_to_telescope_plane, double detector_to_telescope_plane_error, double pixel_position, double pixel_position_error)
{
  double term1;
  return -1.;
}

const double detector_to_telescope_plane_0 = 235.;                                 // cm
const double detector_to_telescope_plane_1 = detector_to_telescope_plane_0 + 2.55; // cm
const double detector_to_telescope_plane_2 = detector_to_telescope_plane_1 + 22.5; // cm
const double detector_to_telescope_plane_3 = detector_to_telescope_plane_2 + 2.55; // cm
const double pixel_resolution_plane_0 = 3.7e-3;                                    // cm
const double pixel_resolution_plane_1 = 3.4e-3;                                    // cm
const double pixel_resolution_plane_2 = 3.4e-3;                                    // cm
const double pixel_resolution_plane_3 = 3.7e-3;                                    // cm

void test()
{
  std::ifstream infile("/Users/nrubini/Desktop/run471001857_251117001903_tracks.txt");
  if (!infile.is_open())
  {
    std::cerr << "Cannot open input file!" << std::endl;
    return;
  }

  // Output ROOT file
  TFile *outfile = new TFile("tracks.root", "RECREATE");
  TTree *tree = new TTree("tracks", "Tracks from ASCII");

  int eventID, ndof;
  double x0, y0, z0, dx_dz, dy_dz, dz_dz, chi2, chi2ndof, timestamp;

  // Branches
  tree->Branch("eventID", &eventID, "eventID/I");
  tree->Branch("x0", &x0, "x0/D");
  tree->Branch("y0", &y0, "y0/D");
  tree->Branch("z0", &z0, "z0/D");
  tree->Branch("dx_dz", &dx_dz, "dx_dz/D");
  tree->Branch("dy_dz", &dy_dz, "dy_dz/D");
  tree->Branch("dz_dz", &dz_dz, "dz_dz/D");
  tree->Branch("chi2", &chi2, "chi2/D");
  tree->Branch("ndof", &ndof, "ndof/I");
  tree->Branch("chi2ndof", &chi2ndof, "chi2ndof/D");
  tree->Branch("timestamp", &timestamp, "timestamp/D");

  std::string line;
  bool firstLine = true;
  int iline = 0;
  while (std::getline(infile, line))
  {

    if (firstLine)
    { // skip header
      firstLine = false;
      continue;
    }

    if (line.find('*') != std::string::npos)
      continue; // skip '*' rows

    std::stringstream ss(line);

    ss >> eventID >> x0 >> y0 >> z0 >> dx_dz >> dy_dz >> dz_dz >> chi2 >> ndof >> chi2ndof >> timestamp;

    if (!ss.fail())
      tree->Fill();
  }

  std::cout << "Done! Wrote tracks.root with " << tree->GetEntries() << " entries." << std::endl;
  tree->Write();
  outfile->Close();
  infile.close();
}