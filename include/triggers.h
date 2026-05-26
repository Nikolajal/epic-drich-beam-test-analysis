#pragma once

/**
 * @file triggers.h
 * @brief Umbrella header for the trigger subsystem.
 *
 * Re-exports the cross-cutting types and config schema that every consumer
 * of the trigger subsystem needs.  Plays the same role for `triggers/` that
 * [`utility.h`](utility.h) plays for `util/`.
 *
 * Subsystem layout:
 *
 * | Header | Contents |
 * |---|---|
 * | [`triggers/events.h`](triggers/events.h)     | `TriggerNumber` enum, default tables, `TriggerEvent` struct |
 * | [`triggers/config.h`](triggers/config.h)     | `DeviceTrigger`, `ChannelTrigger`, `TriggerConfigSet`, `trigger_conf_reader`, `pack_channel_key` |
 * | [`triggers/registry.h`](triggers/registry.h) | `TriggerRegistry` (depends on the two above + ROOT TH2) |
 *
 * Algorithm headers in `triggers/` (e.g. [`triggers/streaming.h`](triggers/streaming.h))
 * are **not** re-exported by the umbrella — same rationale as `utility.h`
 * not re-exporting algorithm-shaped helpers.  Include them deliberately
 * from the translation units that need them.
 *
 * The subsystem's community-facing design notes live at
 * [`triggers/DISCUSSION.md`](triggers/DISCUSSION.md).
 *
 * ### TOML config format
 *
 * @code{.toml}
 * # Device-mode: hardware tags the word as a trigger.
 * [[trigger]]
 * name   = "luca_and_finger"
 * index  = 0
 * device = 196
 * delay  = 117
 *
 * # Channel-mode: a data channel is forced into the trigger path.
 * [[trigger]]
 * name   = "finger_chip1"
 * index  = 2
 * device = 196
 * fifo   = 5
 * column = 3
 * pixel  = 0
 * delay  = 80
 * @endcode
 *
 * See [`triggers/config.h`](triggers/config.h) for the full schema and
 * [`conf/trigger_conf.2025.toml`](../conf/trigger_conf.2025.toml) for live
 * examples of both modes (including the `eo_channel` alternative).
 */

#include "triggers/events.h"
#include "triggers/config.h"
#include "triggers/registry.h"
