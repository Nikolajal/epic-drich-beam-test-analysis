"""Quick Local QA dashboard for ePIC dRICH beam-test analysis.

Operator cockpit for shift-time work — edit configs, manage the run
database, launch + watch writers, inspect output, tag quality.

Top-level shape
---------------
- ``app``          ``QMainWindow`` with the top-bar tab shell.
- ``runmanager``   Run Manager tab — launch writers (detached so they
                   survive a dashboard close), watch their log live,
                   inspect output, set quality of the current run.
- ``qa``           QA tab — three-shape skeleton (per-run thumbnails /
                   cross-run trends / quality cockpit); shapes fill in
                   one at a time.
- ``runlist``      Database tab — browse / edit the run database;
                   detect new runs, auto-pin edits, save selections
                   as named runlists.
- ``settings``     Settings tab — every ``conf/*.toml`` editable
                   two-way with comment-preserving write-back, on top
                   of the setting-set system (defaults / sets / working).

Reusable widgets
----------------
- ``toml_form``    Recursive bubble-card form over a tomlkit document.
- ``datainspect``  Canonical-file preview + embedded ``Config/`` reader.

Pure helpers
------------
- ``toml_model``   Walker / setter / ``##``-cutoff / round-trip safety.
- ``conf_layout``  Setting-set symlink / defaults / working overlay.
- ``rundb``        Run-database parser with forward-inheritance.
- ``writers``      Writer catalog (FlagSpec / WriterSpec).
- ``joblock``      Per-(writer, run) lock files for cross-instance state.
- ``runner``       ``subprocess.Popen``-backed detached runner with
                   line-buffered log tail.
- ``theme``        Palette + light/dark QSS + system-follow.
- ``summary``      Per-run reader for ``recodata.root`` (uproot).
"""
