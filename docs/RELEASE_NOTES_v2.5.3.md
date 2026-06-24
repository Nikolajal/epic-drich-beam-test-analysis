# v2.5.3 — fix: restore self-contained RootHist (revert mist-hep owned:: adoption)

Bugfix release.  Reverts the v2.5.1 `RootHist` → `mist::hep::owned` shim, which
made the core header `include/utility/root_hist.h` depend on the FetchContent'd
`mist/hep/owned.h`.  That header is absent from the source tree, so ROOT-autoload
/ cling contexts without the mist-hep include path (fresh macros; the
install-mode loader) failed to parse it — breaking struct autoload in scratch
analyses and any install-mode use.

- `RootHist<T>` is again self-contained (standard + ROOT headers only).
- mist-hep is dropped as a dependency (`root_hist.h` was its only consumer).
- v2.5.2's sideband adoption (`mist::stats`, in a compiled `.cxx`) is unaffected.

No behaviour change.  Verified: build green (6 writers), pytest 340, `root_hist.h`
cling-parses clean.
