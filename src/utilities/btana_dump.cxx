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

#include <TFile.h>
#include <TTree.h>

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

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

    AlcorSpilldata spilldata;
    spilldata.link_to_tree(t);

    const long total = t->GetEntries();
    const long n = (n_entries < 0) ? total : std::min<long>(n_entries, total);

    std::cout << "lightdata: " << total << " spill(s); printing " << n << "\n";
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

    AlcorRecodata recodata;
    if (!recodata.link_to_tree(t))
    {
        std::cerr << "btana-dump: failed to link AlcorRecodata to 'recodata' tree\n";
        return 1;
    }

    const long total = t->GetEntries();
    const long n = (n_entries < 0) ? total : std::min<long>(n_entries, total);

    std::cout << "recodata: " << total << " frame(s); printing " << n << "\n";
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

    AlcorRecotrackdata rtd;
    rtd.link_to_tree(t);

    const long total = t->GetEntries();
    const long n = (n_entries < 0) ? total : std::min<long>(n_entries, total);

    std::cout << "recotrackdata: " << total << " frame(s); printing " << n << "\n";
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

int dump_file(const std::string &file_path, long n_entries)
{
    const auto fmt = detect_format(file_path);
    if (fmt == DumpFormat::Unknown)
    {
        std::cerr << "btana-dump: " << file_path
                  << " — no recognised tree (lightdata / recodata / recotrackdata)\n";
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
    default:
        return 2;
    }
}

} // namespace btana::utilities
