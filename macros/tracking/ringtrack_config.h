#pragma once
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>

// ============================================================
//  RingtrackConfig — legge ringtrack.conf e fornisce accessor
// ============================================================
struct RingtrackConfig
{
    std::map<std::string, std::string> _data;

    void load(const std::string &path)
    {
        std::ifstream f(path);
        if (!f)
            throw std::runtime_error("[RingtrackConfig] Cannot open: " + path);

        std::string line;
        while (std::getline(f, line))
        {
            // rimuovi spazi iniziali
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            line = line.substr(start);

            // salta commenti e righe vuote
            if (line.empty() || line[0] == '#') continue;

            // trova il '='
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key   = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));

            // rimuovi commento inline
            size_t comment = value.find('#');
            if (comment != std::string::npos)
                value = trim(value.substr(0, comment));

            _data[key] = value;
        }
    }

    // --- accessor con default ---
    std::string get_string(const std::string &key, const std::string &def = "") const
    {
        auto it = _data.find(key);
        return (it != _data.end()) ? it->second : def;
    }

    int get_int(const std::string &key, int def = 0) const
    {
        auto it = _data.find(key);
        if (it == _data.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }

    float get_float(const std::string &key, float def = 0.f) const
    {
        auto it = _data.find(key);
        if (it == _data.end()) return def;
        try { return std::stof(it->second); } catch (...) { return def; }
    }

    bool get_bool(const std::string &key, bool def = false) const
    {
        auto it = _data.find(key);
        if (it == _data.end()) return def;
        const std::string &v = it->second;
        if (v == "true"  || v == "1" || v == "yes") return true;
        if (v == "false" || v == "0" || v == "no")  return false;
        return def;
    }

    void print() const
    {
        std::cout << "[RingtrackConfig] loaded settings:\n";
        for (auto &kv : _data)
            std::cout << "  " << kv.first << " = " << kv.second << "\n";
    }

private:
    static std::string trim(const std::string &s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    }
};