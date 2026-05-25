# Coding conventions

Naming and structural conventions followed across the codebase.  Reference
material for new contributors and code review — not a discussion log.

## Naming

| Kind                                       | Style                  | Examples                                              |
|--------------------------------------------|------------------------|-------------------------------------------------------|
| Variables, free functions, methods         | `snake_case`           | `get_hit_x`, `frame_size`, `current_trigger_hits`     |
| Private members                            | `snake_case_`          | `triggers_`, `frame_mutexes_access_`                  |
| Classes, structs, type aliases             | `PascalCase`           | `AlcorFinedata`, `GlobalIndex`, `TriggerEvent`        |
| Enum values (including legacy plain enums) | `PascalCase`           | `TriggerFirstFrames`, `TriggerTiming`                 |
| Project macros                             | `BTANA_` + `ALL_CAPS`  | `BTANA_BUILD_TESTS`, `BTANA_ALCOR_CC_TO_NS`           |
| Namespaces                                 | `lowercase`            | `mist`, `mist::ring_finding`                          |
| Filenames (incl. macro entry-points)       | `snake_case`           | `parallel_streaming_framer.cxx`, `photon_number.cpp`  |

## Local-vs-type collision rule

When a local variable would otherwise be spelled identically to its type
(`Hit hit`, `TriggerEvent triggerevent`), break the collision with one of:

1. **`current_` prefix** when the variable is simply "the one being processed
   right now": `Hit current_hit;`, `TriggerEvent current_trigger;`.
2. **A semantic role name** when the context warrants more specificity:
   `Hit candidate_hit`, `Hit reference_hit`, `TriggerEvent primary_trigger`.

Prefer the semantic role when several instances of the same type appear in
the same scope; reach for `current_` when no better name is available.
