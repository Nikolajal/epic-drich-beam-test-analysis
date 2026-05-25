#include "alcor_data.h"

void AlcorData::link_to_tree(TTree *input_tree)
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
    if (input_tree->GetBranch("HitMask"))
        input_tree->SetBranchAddress("HitMask", &data.HitMask);
}

void AlcorData::write_to_tree(TTree *output_tree)
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
    output_tree->Branch("HitMask", &data.HitMask, "HitMask/i"); // fix: was &data.fine
}