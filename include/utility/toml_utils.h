#pragma once

/**
 * @file toml_utils.h
 * @brief Cutoff-aware TOML loader.
 *
 * Wraps @c toml++ with a small helper that truncates the file at a sentinel
 * line — typically @c "##" — letting config files keep changelog notes or
 * disabled-entry scratch space below the live configuration without those
 * sections being parsed.
 */

#include <fstream>
#include <stdexcept>
#include <string>
#include <toml++/toml.h>

/**
 * @brief Parse a TOML file, stopping at the first line whose first non-whitespace
 *        content begins with @p cutoff (default: "##").
 *
 * Everything from that line onward is discarded before parsing, letting you
 * place a "## ---- notes below ----" sentinel in config files to keep scratch
 * notes, disabled entries, or changelogs without them being parsed.
 *
 * @param filepath  Path to the TOML file.
 * @param cutoff    Sentinel prefix (default "##").
 * @return          Parsed toml::table (same semantics as toml::parse_file).
 * @throws toml::parse_error on file-open failure or TOML syntax error.
 */
inline toml::table toml_parse_with_cutoff(const std::string &filepath,
                                          const std::string &cutoff = "##")
{
    std::ifstream file(filepath);
    if (!file.is_open())
    {
        // No double-IO on the error path
        // ` here which would re-open
        // the same file just to surface the open error).  std::runtime_error
        // is sufficient; the message + path is all callers need.
        throw std::runtime_error("toml_parse_with_cutoff: cannot open file: " + filepath);
    }

    std::string content, line;
    while (std::getline(file, line))
    {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, cutoff.size(), cutoff) == 0)
            break;
        content += line + '\n';
    }
    return toml::parse(content, filepath);
}
