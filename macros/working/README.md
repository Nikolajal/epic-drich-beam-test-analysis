# `macros/working/` — personal scratch space (NOT tracked)

This directory is a per-operator scratch area for **personal, throwaway, or
in-development ROOT macros**. Everything here is git-ignored except this
README (see the `macros/working/*` + `!macros/working/README.md` rules in
the repo-root `.gitignore`), so your experiments never show up as untracked
clutter or accidentally land in a commit. It mirrors the `conf/working/`
convention for per-operator config overlays.

Drop any `.C` / `.cpp` you're hacking on here and run it the usual way, e.g.

```bash
root -l 'macros/working/my_macro.C'
```

## Where macros live

| Directory | Tracked? | Purpose |
|---|---|---|
| `macros/examples/`  | ✅ | Shipped **reference / example** analyses (read these to learn the API). |
| `macros/utilities/` | ✅ | Shipped utility macros **and the writer build targets** (`lightdata_writer.cpp`, …) wired into CMake. |
| `macros/working/`   | ❌ | **Personal scratch** — this directory. Not shipped, not reviewed. |

Rule of thumb: if a macro is meant for others to use or read, it belongs in
`examples/` (or `utilities/` if it's a build target) and must be committed.
If it's just for you, keep it here.
