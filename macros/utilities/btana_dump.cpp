/**
 * @file macros/utilities/btana_dump.cpp
 * @brief `btana-dump` — thin CLI front-end around
 *        @ref btana::utilities::dump_file.
 *
 * Usage:
 *   btana-dump <file.root>            (prints first 5 entries)
 *   btana-dump <file.root> -n 20      (prints first 20 entries)
 *   btana-dump <file.root> -n -1      (prints every entry)
 *
 * The format (lightdata / recodata / recotrackdata) is auto-detected
 * from the tree names present in the file; no flag selects between
 * them.  Output is human-readable, intended for terminal use and quick
 * post-run inspection — not for piping into downstream tools.
 */

#include "utilities/btana_dump.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    CLI::App app{"btana-dump — pretty-print lightdata/recodata/recotrackdata ROOT files"};

    std::string file_path;
    long n_entries = 5;

    app.add_option("file", file_path, "Path to a beam-test ROOT file")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("-n,--entries", n_entries,
                   "Number of entries to print (use -1 for every entry; default 5)");

    CLI11_PARSE(app, argc, argv);

    return btana::utilities::dump_file(file_path, n_entries);
}
