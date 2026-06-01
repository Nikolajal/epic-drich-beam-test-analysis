/**
 * @file utilities/btana_dump.cxx
 * @brief Implementation of the auto-detecting on-disk format dumper.
 *
 * Format-detection contract — see corresponding header.  Each
 * per-format function follows the same shape:
 *
 *   1. Open the input file read-only.
 *   2. Look up the canonical tree by name; bail with a clear error.
 *   3. `link_to_tree` the appropriate wrapper class.
 *   4. Iterate up to `n_entries` and pretty-print.
 *
 * Output is stdout-only.  No framework state is mutated (calibration
 * table stays in whatever state the caller had it) — this is a
 * READ-ONLY decoder.
 */

#include "utilities/btana_dump.h"

#include "alcor_lightdata.h"
#include "alcor_recodata.h"
#include "alcor_recotrackdata.h"
#include "alcor_spilldata.h"
#include "triggers/events.h"

#include <TDirectory.h>
#include <TFile.h>
#include <TH1.h>
#include <TKey.h>
#include <TList.h>
#include <TNamed.h>
#include <TParameter.h>
#include <TTree.h>

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace btana::utilities
{

const char *format_name(DumpFormat f)
{
    switch (f)
    {
    case DumpFormat::Lightdata:
        return "lightdata";
    case DumpFormat::Recodata:
        return "recodata";
    case DumpFormat::Recotrackdata:
        return "recotrackdata";
    case DumpFormat::PulserCalibQa:
        return "pulser_calib_qa";
    default:
        return "unknown";
    }
}

DumpFormat detect_format(const std::string &file_path)
{
    std::unique_ptr<TFile> f(TFile::Open(file_path.c_str(), "READ"));
    if (!f || f->IsZombie())
        return DumpFormat::Unknown;

    //  Prefer the richest format if multiple are present.
    if (f->Get<TTree>("recotrackdata"))
        return DumpFormat::Recotrackdata;
    if (f->Get<TTree>("recodata"))
        return DumpFormat::Recodata;
    if (f->Get<TTree>("lightdata"))
        return DumpFormat::Lightdata;
    //  No recognised tree — could still be a metadata-only file like
    //  pulser_calib_qa.root.  Recognise it by the TDirectory layout
    //  the producer writes (Config + RunSummary + Fits + Spreads).
    if (f->Get<TDirectory>("Config") && f->Get<TDirectory>("Spreads"))
        return DumpFormat::PulserCalibQa;
    return DumpFormat::Unknown;
}

namespace
{

//  Resolve a TriggerEvent::index to a printable name via the built-in
//  default table from triggers/events.h.  Falls back to the numeric
//  index if it's not one of the well-known ones (custom triggers from
//  conf/trigger_conf.toml don't surface here — dumping those by name
//  would require loading + parsing the TOML, which the dumper deliberately
//  avoids to stay self-contained).
std::string trigger_label(uint8_t idx)
{
    for (int i = 0; i < n_default_triggers; ++i)
        if (static_cast<uint8_t>(all_default_triggers[i]) == idx)
            return default_names[i];
    std::ostringstream os;
    os << "trig#" << static_cast<int>(idx);
    return os.str();
}

void print_trigger(const TriggerEvent &t, const std::string &indent = "    ")
{
    std::cout << indent
              << std::left << std::setw(22) << trigger_label(t.index)
              << "  coarse=" << std::setw(6) << t.coarse
              << "  fine_time=" << std::fixed << std::setprecision(2)
              << std::setw(8) << t.fine_time << " ns"
              << (t.is_secondary ? "   [secondary]" : "")
              << "\n";
}

//  ── Metadata walker ──────────────────────────────────────────────
//  Walk a TDirectory (recursively into sub-TDirectories) and print
//  every TParameter / TNamed it encounters as `name = value`.
//  Used by every format-specific dump to surface the embedded Config/
//  block (and RunSummary, Spreads, etc. on the pulser QA file).
//  Histograms are summarised in one line (entries / mean / RMS) and
//  not dumped bin-by-bin.

void print_scalar_for_key(const std::string &dir_label, TKey *key, TDirectory *dir)
{
    const std::string cls = key->GetClassName();
    const std::string name = key->GetName();
    const std::string prefix = "  " + dir_label + (dir_label.empty() ? "" : "/") + name;

    //  TParameter<T> is templated — try the common instantiations.
    if (cls == "TParameter<int>")
    {
        if (auto *p = dynamic_cast<TParameter<int> *>(dir->Get(name.c_str())))
            std::cout << prefix << " = " << p->GetVal() << "\n";
    }
    else if (cls == "TParameter<double>")
    {
        if (auto *p = dynamic_cast<TParameter<double> *>(dir->Get(name.c_str())))
            std::cout << prefix << " = " << p->GetVal() << "\n";
    }
    else if (cls == "TParameter<float>")
    {
        if (auto *p = dynamic_cast<TParameter<float> *>(dir->Get(name.c_str())))
            std::cout << prefix << " = " << p->GetVal() << "\n";
    }
    else if (cls == "TParameter<Long64_t>")
    {
        if (auto *p = dynamic_cast<TParameter<Long64_t> *>(dir->Get(name.c_str())))
            std::cout << prefix << " = " << p->GetVal() << "\n";
    }
    else if (cls == "TNamed")
    {
        if (auto *n = dynamic_cast<TNamed *>(dir->Get(name.c_str())))
            std::cout << prefix << " = " << n->GetTitle() << "\n";
    }
    //  TH* — short one-line summary; full hist dump would flood the terminal.
    else if (cls.rfind("TH1", 0) == 0 || cls.rfind("TH2", 0) == 0 ||
             cls.rfind("TH3", 0) == 0)
    {
        if (auto *h = dynamic_cast<TH1 *>(dir->Get(name.c_str())))
            std::cout << prefix
                      << "   [" << cls << "]"
                      << "  entries=" << h->GetEntries()
                      << "  mean=" << h->GetMean()
                      << "  rms=" << h->GetRMS() << "\n";
    }
    //  Sub-directory marker — recursion handled by the caller.
    //  Other types (TTree, TGraph, …) silently skipped.
}

void walk_directory(TDirectory *dir, const std::string &dir_label)
{
    if (!dir)
        return;
    TList *keys = dir->GetListOfKeys();
    if (!keys)
        return;
    //  First pass: print non-directory keys.
    TIter next(keys);
    while (auto *obj = next())
    {
        auto *key = static_cast<TKey *>(obj);
        const std::string cls = key->GetClassName();
        if (cls == "TDirectoryFile" || cls == "TDirectory")
            continue;
        print_scalar_for_key(dir_label, key, dir);
    }
    //  Second pass: recurse into sub-directories.
    next.Reset();
    while (auto *obj = next())
    {
        auto *key = static_cast<TKey *>(obj);
        const std::string cls = key->GetClassName();
        if (cls != "TDirectoryFile" && cls != "TDirectory")
            continue;
        const std::string name = key->GetName();
        auto *sub = dir->Get<TDirectory>(name.c_str());
        const std::string label = dir_label.empty() ? name : (dir_label + "/" + name);
        std::cout << "  " << label << "/\n";
        walk_directory(sub, label);
    }
}

//  Surface a file's TDirectory metadata: Config + RunSummary + any other
//  scalar block.  Trees and per-entry data are NOT printed here — that's
//  the job of the format-specific dumpers above.
void dump_file_metadata(TFile *f)
{
    if (!f || f->IsZombie())
        return;
    //  Quick check: does the file have anything worth dumping?  Skip
    //  the header line if there are no scalars to print, to avoid
    //  cluttering output for files with only trees.
    bool has_metadata = false;
    TList *keys = f->GetListOfKeys();
    if (keys)
    {
        TIter next(keys);
        while (auto *obj = next())
        {
            auto *key = static_cast<TKey *>(obj);
            const std::string cls = key->GetClassName();
            if (cls == "TDirectoryFile" || cls == "TDirectory" ||
                cls == "TParameter<int>" || cls == "TParameter<double>" ||
                cls == "TParameter<float>" || cls == "TParameter<Long64_t>" ||
                cls == "TNamed" ||
                cls.rfind("TH1", 0) == 0 || cls.rfind("TH2", 0) == 0)
            {
                has_metadata = true;
                break;
            }
        }
    }
    if (!has_metadata)
        return;

    std::cout << "\n=== embedded metadata ===\n";
    walk_directory(f, "");
}

} // namespace

int dump_lightdata(const std::string &file_path, long n_entries)
{
    std::unique_ptr<TFile> f(TFile::Open(file_path.c_str(), "READ"));
    if (!f || f->IsZombie())
    {
        std::cerr << "btana-dump: cannot open " << file_path << "\n";
        return 1;
    }
    auto *t = f->Get<TTree>("lightdata");
    if (!t)
    {
        std::cerr << "btana-dump: no 'lightdata' tree in " << file_path << "\n";
        return 1;
    }

    //  Metadata first (Config/, RunSummary/, …) so the operator sees
    //  the file's provenance before scrolling into the per-entry data.
    dump_file_metadata(f.get());

    AlcorSpilldata spilldata;
    spilldata.link_to_tree(t);

    const long total = t->GetEntries();
    const long n = (n_entries < 0) ? total : std::min<long>(n_entries, total);

    std::cout << "\nlightdata: " << total << " spill(s); printing " << n << "\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    for (long i = 0; i < n; ++i)
    {
        t->GetEntry(i);
        spilldata.get_entry();
        auto &frames = spilldata.get_frame_list_link();
        auto &frame_ref = spilldata.get_frame_reference_list_link();

        //  Spill-level totals
        std::size_t n_trig = 0, n_tim = 0, n_trk = 0, n_chr = 0;
        for (auto const &fr : frames)
        {
            n_trig += fr.trigger_hits.size();
            n_tim += fr.timing_hits.size();
            n_trk += fr.tracking_hits.size();
            n_chr += fr.cherenkov_hits.size();
        }

        std::cout << "spill " << i
                  << "   frames=" << frames.size()
                  << "   triggers=" << n_trig
                  << "   timing=" << n_tim
                  << "   tracking=" << n_trk
                  << "   cherenkov=" << n_chr
                  << "\n";

        //  Detail: first 3 frames of the first 3 spills only.  Keeps
        //  per-spill output bounded so large files stay readable.
        if (i < 3)
        {
            const std::size_t n_show = std::min<std::size_t>(3, frames.size());
            for (std::size_t k = 0; k < n_show; ++k)
            {
                const auto &fr = frames[k];
                const uint32_t fid = (k < frame_ref.size()) ? frame_ref[k] : 0;
                std::cout << "  frame[" << k << "] id=" << fid
                          << "   trig=" << fr.trigger_hits.size()
                          << "   tim=" << fr.timing_hits.size()
                          << "   trk=" << fr.tracking_hits.size()
                          << "   chr=" << fr.cherenkov_hits.size() << "\n";
                for (const auto &tr : fr.trigger_hits)
                    print_trigger(tr, "      ");
            }
            if (frames.size() > n_show)
                std::cout << "  … " << (frames.size() - n_show)
                          << " more frame(s) (suppressed)\n";
        }
    }
    dump_file_metadata(f.get());
    return 0;
}

int dump_recodata(const std::string &file_path, long n_entries)
{
    std::unique_ptr<TFile> f(TFile::Open(file_path.c_str(), "READ"));
    if (!f || f->IsZombie())
    {
        std::cerr << "btana-dump: cannot open " << file_path << "\n";
        return 1;
    }
    auto *t = f->Get<TTree>("recodata");
    if (!t)
    {
        std::cerr << "btana-dump: no 'recodata' tree in " << file_path << "\n";
        return 1;
    }

    dump_file_metadata(f.get());

    AlcorRecodata recodata;
    if (!recodata.link_to_tree(t))
    {
        std::cerr << "btana-dump: failed to link AlcorRecodata to 'recodata' tree\n";
        return 1;
    }

    const long total = t->GetEntries();
    const long n = (n_entries < 0) ? total : std::min<long>(n_entries, total);

    std::cout << "\nrecodata: " << total << " frame(s); printing " << n << "\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    for (long i = 0; i < n; ++i)
    {
        t->GetEntry(i);
        const auto &hits = recodata.get_recodata();
        const auto &trigs = recodata.get_triggers();
        std::cout << "frame " << i
                  << "   hits=" << hits.size()
                  << "   triggers=" << trigs.size() << "\n";
        for (const auto &tr : trigs)
            print_trigger(tr);
    }
    return 0;
}

int dump_recotrackdata(const std::string &file_path, long n_entries)
{
    std::unique_ptr<TFile> f(TFile::Open(file_path.c_str(), "READ"));
    if (!f || f->IsZombie())
    {
        std::cerr << "btana-dump: cannot open " << file_path << "\n";
        return 1;
    }
    auto *t = f->Get<TTree>("recotrackdata");
    if (!t)
    {
        std::cerr << "btana-dump: no 'recotrackdata' tree in " << file_path << "\n";
        return 1;
    }

    dump_file_metadata(f.get());

    AlcorRecotrackdata rtd;
    rtd.link_to_tree(t);

    const long total = t->GetEntries();
    const long n = (n_entries < 0) ? total : std::min<long>(n_entries, total);

    std::cout << "\nrecotrackdata: " << total << " frame(s); printing " << n << "\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    for (long i = 0; i < n; ++i)
    {
        t->GetEntry(i);
        const auto &hits = rtd.get_recodata();
        const auto &trigs = rtd.get_triggers();
        const auto ntracks = rtd.n_recotrackdata();
        std::cout << "frame " << i
                  << "   hits=" << hits.size()
                  << "   triggers=" << trigs.size()
                  << "   tracks=" << ntracks << "\n";
        for (const auto &tr : trigs)
            print_trigger(tr);
        for (std::size_t k = 0; k < ntracks; ++k)
        {
            std::cout << "    track[" << k << "]"
                      << "   x=" << std::fixed << std::setprecision(2) << rtd.get_det_plane_x(k)
                      << "   y=" << rtd.get_det_plane_y(k)
                      << "   r=" << rtd.get_det_plane_r(k)
                      << "   χ²/ndf=" << std::setprecision(3) << rtd.get_chi2ndof(k)
                      << "\n";
        }
    }
    return 0;
}

int dump_pulser_calib_qa(const std::string &file_path)
{
    std::unique_ptr<TFile> f(TFile::Open(file_path.c_str(), "READ"));
    if (!f || f->IsZombie())
    {
        std::cerr << "btana-dump: cannot open " << file_path << "\n";
        return 1;
    }
    std::cout << "pulser_calib_qa: no per-entry tree; printing embedded metadata.\n";
    std::cout << "──────────────────────────────────────────────────────────\n";
    dump_file_metadata(f.get());
    return 0;
}

int dump_file(const std::string &file_path, long n_entries)
{
    const auto fmt = detect_format(file_path);
    if (fmt == DumpFormat::Unknown)
    {
        std::cerr << "btana-dump: " << file_path
                  << " — no recognised tree (lightdata / recodata / "
                  << "recotrackdata) and no recognised metadata layout "
                  << "(pulser_calib_qa)\n";
        return 2;
    }
    std::cout << "file: " << file_path << "  format: " << format_name(fmt) << "\n";
    switch (fmt)
    {
    case DumpFormat::Lightdata:
        return dump_lightdata(file_path, n_entries);
    case DumpFormat::Recodata:
        return dump_recodata(file_path, n_entries);
    case DumpFormat::Recotrackdata:
        return dump_recotrackdata(file_path, n_entries);
    case DumpFormat::PulserCalibQa:
        return dump_pulser_calib_qa(file_path);
    default:
        return 2;
    }
}

} // namespace btana::utilities
