#pragma once

/**
 * @file util/root_io.h
 * @brief ROOT file open-or-build helper.
 *
 * Convenience wrapper for the "open this `.root` file, or run a builder
 * function to regenerate it from raw data if missing or corrupted" idiom.
 * Used by the analysis macros to make their inputs self-healing.
 */

#include <memory>
#include <TFile.h>

/**
 * @brief Custom deleter for ROOT `TFile` that closes before deleting.
 *
 * `TFile` destruction does call `Close()` internally, but the order is not
 * guaranteed to be "close → free buffers → write directory record".  An
 * explicit `Close()` first removes that ambiguity and is symmetrical with
 * the manual "always close before delete" idiom you'd write by hand.
 */
struct RootFileDeleter
{
    void operator()(TFile *file) const noexcept
    {
        if (file)
        {
            if (file->IsOpen())
                file->Close();
            delete file;
        }
    }
};

/// Owning handle for a ROOT `TFile` — closes on destruction (CODE_REVIEW §5.9).
using TFilePtr = std::unique_ptr<TFile, RootFileDeleter>;

// The previous `open_or_build_rootfile()` helper (CODE_REVIEW §D-06) was
// removed: it pretended to be general but carried a 10-arg std::function
// builder with 7 args hardcoded inside the helper, and had exactly one
// caller (recodata_writer).  The auto-rebuild logic is now inlined at the
// single call site in src/recodata_writer.cxx where it is explicit and
// readable.  Restore this helper only if a second writer chain (e.g.
// recotrackdata → recodata → lightdata, currently bail-on-missing) needs
// the same pattern.
