// tester_conf_path.cxx — exercises util::campaign_of + util::conf_path
// (per-campaign config resolution, conf/sets/<campaign>/ auto-selection).

#include "utility/conf_path.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{
int failures = 0;
void check(bool cond, const char *what)
{
    if (!cond)
    {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}
void check_eq(const std::string &got, const std::string &want, const char *what)
{
    if (got != want)
    {
        std::printf("  FAIL: %s — got '%s', want '%s'\n", what, got.c_str(), want.c_str());
        ++failures;
    }
}
} // namespace

int main()
{
    namespace fs = std::filesystem;

    // 1) campaign_of: the run-id year, or empty for non-dated names.
    std::puts("[tester_conf_path] campaign_of");
    check_eq(util::campaign_of("20251111-164951"), "2025", "2025 run id");
    check_eq(util::campaign_of("20260614-132826"), "2026", "2026 run id");
    check_eq(util::campaign_of("2026.runlists.toml"), "2026", "runlist filename");
    check_eq(util::campaign_of("mixed.runlists.toml"), "", "mixed → empty");
    check_eq(util::campaign_of("abc"), "", "too short / non-numeric → empty");
    check_eq(util::campaign_of(""), "", "empty → empty");

    // 2) conf_path resolution against a temp conf/ tree (chdir so the relative
    //    'conf/...' lookups resolve here, not in the real repo).
    std::puts("[tester_conf_path] conf_path resolution");
    const fs::path cwd0 = fs::current_path();
    const fs::path tmp = fs::temp_directory_path() / "btana_conf_path_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "conf" / "QA");
    fs::create_directories(tmp / "conf" / "sets" / "2025");
    auto touch = [](const fs::path &p)
    { std::FILE *f = std::fopen(p.string().c_str(), "w"); if (f) std::fclose(f); };
    touch(tmp / "conf" / "trigger_conf.toml");           // base default
    touch(tmp / "conf" / "streaming.toml");              // base default
    touch(tmp / "conf" / "QA" / "streaming.toml");       // mode overlay
    touch(tmp / "conf" / "sets" / "2025" / "trigger_conf.toml"); // 2025 bundle
    fs::current_path(tmp);

    // 2025 trigger → the campaign bundle wins (most specific present).
    check_eq(util::conf_path("trigger_conf.toml", "", "2025"),
             "conf/sets/2025/trigger_conf.toml", "2025 → campaign bundle");
    // 2026 trigger → no bundle → base default.
    check_eq(util::conf_path("trigger_conf.toml", "", "2026"),
             "conf/trigger_conf.toml", "2026 → base default");
    // QA streaming, 2025 → no campaign streaming, no campaign+QA → mode overlay.
    check_eq(util::conf_path("streaming.toml", "QA", "2025"),
             "conf/QA/streaming.toml", "2025 QA streaming → mode overlay");
    // 2025 trigger in QA mode → campaign bundle still wins over mode (no
    //  conf/sets/2025/QA/, so falls to conf/sets/2025/).
    check_eq(util::conf_path("trigger_conf.toml", "QA", "2025"),
             "conf/sets/2025/trigger_conf.toml", "2025 QA trigger → campaign bundle");
    // No campaign → shared conf/ only.
    check_eq(util::conf_path("trigger_conf.toml", "", ""),
             "conf/trigger_conf.toml", "no campaign → base");

    fs::current_path(cwd0);
    fs::remove_all(tmp);

    if (failures)
    {
        std::printf("[tester_conf_path] %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("[tester_conf_path] OK");
    return EXIT_SUCCESS;
}
