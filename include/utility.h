#pragma once

/**
 * @file utility.h
 * @brief Umbrella header that re-exports every helper under @c include/utility/.
 *
 * Kept as a single-include convenience for legacy call sites.  New code
 * should prefer the focused subheaders directly (`#include "utility/bit_ops.h"`,
 * `#include "utility/global_index.h"`, …) to keep compile-time dependencies tight.
 *
 * @par What lives where
 *
 * | Topic                             | Header                                 |
 * |-----------------------------------|----------------------------------------|
 * | Bit manipulation                  | [utility/bit_ops.h](utility/bit_ops.h)         |
 * | Global TDC index value type       | [utility/global_index.h](utility/global_index.h) |
 * | TOML loader with cutoff sentinel  | [utility/toml_utils.h](utility/toml_utils.h)   |
 * | Run database / run-list / readout | [utility/config_reader.h](utility/config_reader.h) |
 * | Circle fitting                    | [utility/circle_fit.h](utility/circle_fit.h)   |
 * | Ring signal model & 2-D ring fit  | [utility/ring_model.h](utility/ring_model.h)   |
 * | ROOT open-or-build file helper    | [utility/root_io.h](utility/root_io.h)         |
 * | ROOT canvas-drawing helpers       | [utility/root_draw.h](utility/root_draw.h)     |
 * | RAII ROOT histogram wrapper       | [utility/root_hist.h](utility/root_hist.h)     |
 *
 * @note The pre-Phase-5 global Mersenne-Twister (`_global_rd_`, `_global_gen_`,
 *       `_rnd_`) has been removed.  Pixel-smearing call sites use a thread-local
 *       @ref mist::Rnd from `<mist/rnd.h>`; new code must follow that pattern.
 */

#include "utility/bit_ops.h"
#include "utility/global_index.h"
#include "utility/toml_utils.h"
#include "utility/circle_fit.h"
#include "utility/ring_model.h"
#include "utility/root_io.h"
#include "utility/root_draw.h"
#include "utility/root_hist.h"
