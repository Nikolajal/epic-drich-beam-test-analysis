/**
 * @file macros/utilities/qa_tcanvas.cpp
 * @brief Stand-alone TCanvas launcher used by the QA dashboard.
 *
 * Sibling of ``qa_tbrowser``: instead of popping a TBrowser, this
 * binary opens a single histogram from a ROOT file and draws it on
 * a fresh TCanvas — operator gets the full native ROOT interactivity
 * (FitPanel, axis menus, stat-box controls, log/lin toggle, …)
 * without a Terminal window in the middle.
 *
 * Why this exists:
 *
 *   - The original ``Open in ROOT`` button shelled out via
 *     ``osascript + Terminal + root -l -e '<expr>'`` so ROOT got a
 *     real TTY.  That works but inflicts a Terminal window on the
 *     operator every time they click the button — the user
 *     specifically asked to avoid it.
 *   - Spawning ``root`` detached from Qt (no Terminal) leaves stdin
 *     on ``/dev/null`` and ROOT's launcher exits before the canvas
 *     comes up.
 *   - This binary owns its ``TApplication`` explicitly, runs the
 *     GUI event loop directly, and works on any platform with a
 *     graphical ROOT install — same trick we use for ``qa_tbrowser``.
 *
 * Usage::
 *
 *     qa_tcanvas <file.root> <hist_path>
 *
 * ``hist_path`` is the full "/"-joined TDirectory path to the
 * histogram (e.g. ``Triggers/h_trigger_hit_multiplicity_FIRST_FRAMES``).
 * TH2-derived objects auto-draw with ``"colz"``; TH1 falls back to
 * the default draw option.
 *
 * Launched by ``qa_quicklook/qa.py::_spawn_root_canvas`` via
 * ``QProcess.startDetached``.  The dashboard fires-and-forgets.
 */

#include <TApplication.h>
#include <TCanvas.h>
#include <TFile.h>
#include <TH1.h>
#include <TQObject.h>
#include <TROOT.h>
#include <TString.h>
#include <TStyle.h>
#include <TSystem.h>

#include <cstdio>
#include <string>

int main(int argc, char **argv)
{
    //  Snapshot the operator-supplied positional args *before*
    //  TApplication eats them (same argv-shrinking gotcha that
    //  qa_tbrowser hits — see that file's header for the long
    //  story).  Two positional args expected: file + hist path.
    std::string file_path;
    std::string hist_path;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a.empty() || a[0] == '-')
            continue; // ignore flags TApplication may want
        if (file_path.empty())
            file_path = a;
        else if (hist_path.empty())
            hist_path = a;
    }
    if (file_path.empty() || hist_path.empty())
    {
        std::fprintf(stderr,
                     "[qa_tcanvas] usage: qa_tcanvas <file.root> <hist_path>\n");
        return 1;
    }

    //  Stub argv so TApplication can't consume our positionals.
    int stub_argc = 1;
    char *stub_argv[2] = {argv[0], nullptr};
    TApplication app("qa_tcanvas", &stub_argc, stub_argv);

    //  Force native canvas (Cocoa on macOS, X11 on Linux).
    gROOT->SetWebDisplay("off");
    gStyle->SetOptStat(1111);

    TFile *f = TFile::Open(file_path.c_str());
    if (!f || f->IsZombie())
    {
        std::fprintf(stderr,
                     "[qa_tcanvas] failed to open %s\n",
                     file_path.c_str());
        return 1;
    }

    //  ``Get`` walks "/"-joined TDirectory paths automatically.
    //  Stays in TObject so we can fan out on draw option below.
    TObject *obj = f->Get(hist_path.c_str());
    if (!obj)
    {
        std::fprintf(stderr,
                     "[qa_tcanvas] object not found: %s in %s\n",
                     hist_path.c_str(), file_path.c_str());
        return 1;
    }

    //  TH2 → ``colz``; everything else → default ("" picks the
    //  right thing per class).  Matches the legacy osascript path.
    const char *draw_opt = obj->InheritsFrom("TH2") ? "colz" : "";

    //  Canvas title uses the bare hist path so the window menu /
    //  dock entry reads cleanly.  Operator reported the previous
    //  1100×720 as cramped — especially for the 2D hitmaps where
    //  the colour-axis text crowds the right margin and the bin
    //  numbers crowd the top.  Bumped to match the dashboard's
    //  larger matplotlib inspect dialogs (qa.py uses 1280–1440
    //  width); still fits a 1440×900 panel with ROOT's default
    //  decorations.
    auto *c = new TCanvas("c_qa", hist_path.c_str(), 1400, 900);
    c->cd();
    obj->Draw(draw_opt);

    //  Force the initial paint BEFORE app.Run() — on macOS Cocoa, a
    //  canvas drawn between TApplication construction and Run() can
    //  appear blank until the first window event (resize, click)
    //  pumps the redraw queue.  Modified() marks the pad dirty,
    //  Update() runs the paint synchronously, ProcessEvents drains
    //  any other Qt/Cocoa events queued during canvas construction.
    //  Without this trio, the operator sees a blank white window.
    c->Modified();
    c->Update();
    gSystem->ProcessEvents();

    std::printf("[qa_tcanvas] %s drawn from %s\n",
                hist_path.c_str(), file_path.c_str());

    //  Exit the process when the operator closes the canvas window —
    //  otherwise app.Run() leaks the event loop and qa_tcanvas
    //  hangs around invisibly.  The class-level connection covers
    //  both TCanvas's own ``Closed()`` signal and the underlying
    //  ``TGMainFrame::CloseWindow()`` (clicking the OS title-bar
    //  close button takes that path).  Either route → Terminate.
    TQObject::Connect("TCanvas", "Closed()",
                      "TApplication", gApplication, "Terminate(=0)");
    TQObject::Connect("TGMainFrame", "CloseWindow()",
                      "TApplication", gApplication, "Terminate(=0)");

    app.Run();
    return 0;
}
