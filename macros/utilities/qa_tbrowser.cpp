/**
 * @file macros/utilities/qa_tbrowser.cpp
 * @brief Stand-alone TBrowser launcher used by the QA dashboard.
 *
 * Why a dedicated binary instead of ``root -l <files> -e 'new TBrowser'``:
 *
 *   - The ``root`` launcher exits when stdin isn't a TTY (the
 *     dashboard spawns detached → stdin is ``/dev/null``), so the
 *     TBrowser window would die immediately after popping.
 *   - The Terminal-via-osascript workaround was macOS-only AND
 *     introduced a shell-quoting nightmare across language
 *     boundaries (Python → AppleScript → Bash → ROOT).
 *   - This binary owns its ``TApplication`` explicitly, runs the
 *     GUI event loop directly, and works on any platform with a
 *     graphical ROOT install (Cocoa on macOS, X11 on Linux).
 *
 * Usage::
 *
 *     qa_tbrowser <file1.root> [file2.root...]
 *
 * Each ``.root`` argument is opened (auto-attached as
 * ``_file0``, ``_file1``, …) and a single TBrowser window pops up
 * showing all of them.  Closing the browser window exits cleanly.
 *
 * Launched by ``qa_quicklook/runmanager.py::_on_inspect`` via
 * ``QProcess.startDetached`` — the dashboard fires-and-forgets.
 */

#include <TApplication.h>
#include <TBrowser.h>
#include <TFile.h>
#include <TQObject.h>
#include <TROOT.h>
#include <TString.h>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    //  Snapshot the operator-supplied ``.root`` paths *before* we
    //  hand argv to TApplication.  ROOT's TApplication ctor walks
    //  argv and silently consumes any trailing ``.root`` arguments
    //  (it treats them as files to auto-open), shrinking argc to 1
    //  by the time control returns to us.  Our explicit
    //  TFile::Open loop below then sees nothing and we end up with
    //  an empty TBrowser — the exact failure mode the operator
    //  reported.  By copying the paths out first and passing
    //  TApplication a stub argc=1 / argv[0] only, we keep complete
    //  control of which files get attached.
    std::vector<std::string> root_paths;
    for (int i = 1; i < argc; ++i)
    {
        TString p(argv[i]);
        if (p.EndsWith(".root"))
            root_paths.emplace_back(argv[i]);
    }

    //  Stub argv for TApplication — same argv[0] so the launcher
    //  identifies itself correctly, but no positional ``.root`` to
    //  trigger the auto-open behaviour.
    int stub_argc = 1;
    char *stub_argv[2] = { argv[0], nullptr };
    TApplication app("qa_tbrowser", &stub_argc, stub_argv);

    //  Force native canvas (Cocoa on macOS, X11 on Linux).  Web
    //  display tries to spawn an external browser, exactly what
    //  this binary is designed to avoid.
    gROOT->SetWebDisplay("off");

    //  Attach each path explicitly via TFile::Open — registers the
    //  file with gROOT so it shows up under ROOT Files in the
    //  browser.  Broken paths log a warning + continue so a typo
    //  in one entry doesn't kill the whole inspection session.
    int n_attached = 0;
    for (const auto &path : root_paths)
    {
        TFile *f = TFile::Open(path.c_str());
        if (!f || f->IsZombie())
        {
            std::fprintf(stderr,
                         "[qa_tbrowser] failed to open %s — skipping\n",
                         path.c_str());
            continue;
        }
        std::printf("[qa_tbrowser] attached %s as _file%d\n",
                    path.c_str(), n_attached++);
    }

    if (n_attached == 0)
    {
        std::fprintf(stderr,
                     "[qa_tbrowser] no .root files attached — "
                     "TBrowser will still pop, but empty\n");
    }

    //  The TBrowser owns its window + lives on the heap so it
    //  survives this scope.  Cast-to-void suppresses the
    //  unused-variable warning without the noise of [[maybe_unused]].
    auto *browser = new TBrowser();
    (void)browser;

    //  Make sure the *process* exits when the operator closes the
    //  TBrowser window.  ROOT's default behaviour leaks the
    //  ``app.Run()`` loop — the window goes away but the
    //  qa_tbrowser process keeps running in the background,
    //  eating an X11 / Cocoa session and a row in `ps`.  This
    //  class-level connection covers any ``TGMainFrame`` (which
    //  is what TBrowser ultimately sits inside) so we don't have
    //  to chase the concrete TBrowserImp subclass — single
    //  qa_tbrowser process per window, no orphans.
    TQObject::Connect("TGMainFrame", "CloseWindow()",
                      "TApplication", gApplication, "Terminate(=0)");

    app.Run();
    return 0;
}
