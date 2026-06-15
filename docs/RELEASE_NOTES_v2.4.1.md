# beam-test-analysis v2.4.1

Patch release — CI/formatting stability. No functional or API changes.

## Fix

- **clang-format determinism across versions.** `.clang-format` now pins
  `AlignConsecutiveAssignments: None` explicitly.  The LLVM-base default for
  this changed between clang-format **22.1.5** (CI — the newest PyPI wheel) and
  **22.1.6** (the maintainer's local Homebrew), which made the format gate
  flip-flop on aligned-`=` enum blocks (e.g. `AlcorOpMode`): a 22.1.6-formatted
  commit would trip the 22.1.5 "Verify / fix" job into an auto-reformat, and
  vice-versa.  Pinning the setting makes both versions produce identical output
  (there is no 22.1.6 wheel, so CI cannot simply match local).  No source
  reformatting was required — the tree is already consistent under the pinned
  setting.

---
