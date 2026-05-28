"""Quick Local QA dashboard for ePIC dRICH beam-test analysis.

Operator cockpit for shift-time work — edit configs, manage the run
database, launch + watch writers, inspect output, tag quality.

Top-level shape
---------------
- ``app``            ``QMainWindow`` with the top-bar tab shell + plot-theme push.
- ``runmanager``     Run Manager: writer cards, run-info edit-in-place flow
                     (Add to database / Edit / cascade-vs-pin), SSH probe,
                     Inspect → ``qa_tbrowser``, Download → rsync wrapper.
- ``qa``             QA tab — per-pipeline-step sub-tabs (Lightdata /
                     Recodata / Recotrack / Pulser calib / Macros), inline
                     PDF viewer, uproot thumbnail grid mirroring the
                     writer's TDirectory tree, click-thru interactive
                     matplotlib modal, monitor-mode auto-refresh,
                     Open-in-ROOT → ``qa_tcanvas``.
- ``runlist``        Database tab — browse / edit ``run-lists/<year>.database.toml``
                     with forward-inheritance, colour-coded quality chips,
                     auto-pin on edit.
- ``runlists``       Named-runlist editor (``<year>.runlists.toml``).
- ``settings``       Settings tab — every ``conf/*.toml`` editable
                     two-way with comment-preserving write-back; setting-set
                     system; per-file validation banner (readout chip overlap).

Reusable widgets
----------------
- ``toml_form``      Recursive bubble-card form over a tomlkit document.
- ``datainspect``    Canonical-file preview + embedded ``Config/`` reader.
- ``thumbs``         uproot histogram → matplotlib thumbnail.

Pure helpers
------------
- ``toml_model``     Walker / setter / ``##``-cutoff / round-trip safety.
- ``conf_layout``    Setting-set symlink / defaults / working overlay.
- ``rundb``          Run-database parser with forward-inheritance +
                     chronological insert + audit log.
- ``readout_validate``  Device/chip overlap detector across roles.
- ``sanity``         V_bias × T expected-band lookup (operator chip).
- ``writers``        Writer catalog (FlagSpec / WriterSpec).
- ``joblock``        Per-(writer, run) lock files for cross-instance state.
- ``runner``         QProcess wrapper with line-buffered log tail +
                     ``\\r``-progress handling.
- ``dbworker``       Background TOML writer pool (keeps GUI responsive).
- ``download``       rsync wrapper, address-prompt persistence, SSH
                     key-auth probe.
- ``theme``          Palette + light/dark QSS + system-follow.
"""
