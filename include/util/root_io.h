#pragma once

/**
 * @file util/root_io.h
 * @brief ROOT file open-or-build helper.
 *
 * Convenience wrapper for the "open this `.root` file, or run a builder
 * function to regenerate it from raw data if missing or corrupted" idiom.
 * Used by the analysis macros to make their inputs self-healing.
 */

#include <functional>
#include <iostream>
#include <string>
#include <TFile.h>

/**
 * @brief Open a ROOT file for reading, rebuilding it if missing or corrupted.
 *
 * Tries to open @p filename.  If the file is absent, a zombie, or
 * @p force_rebuild is @c true, calls @p builder with the supplied arguments to
 * regenerate it, then re-opens it.
 *
 * @param filename          Path to the ROOT file to open.
 * @param builder           Callable that creates the file from raw data.
 * @param data_repository   Passed verbatim as the first argument to @p builder.
 * @param run_name          Passed verbatim as the second argument.
 * @param max_spill         Passed verbatim as the third argument.
 * @param force_rebuild     If @c true, always rebuild (default: false).
 * @return                  Open @c TFile* in READ mode, or @c nullptr on failure.
 */
inline TFile *open_or_build_rootfile(const std::string &filename,
                                     std::function<void(std::string, std::string, int, bool, int, std::string, std::string, std::string, std::string, std::string)> builder,
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
    builder(data_repository, run_name, max_spill, force_rebuild, -1, "", "", "", "", "");

    TFile *input_file = TFile::Open(filename.c_str(), "READ");
    if (!input_file || input_file->IsZombie())
    {
        std::cerr << "[ERROR] Could not open file '" << filename << "' even after rebuilding\n";
        delete input_file;
        return nullptr;
    }
    return input_file;
}
