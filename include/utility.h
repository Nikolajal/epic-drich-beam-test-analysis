#pragma once

/**
 * @file utility.h
 * @brief Umbrella header that re-exports every helper under @c include/util/.
 *
 * Kept as a single-include convenience for legacy call sites.  New code
 * should prefer the focused subheaders directly (`#include "util/bit_ops.h"`,
 * `#include "util/global_index.h"`, …) to keep compile-time dependencies tight.
 *
 * @par What lives where
 *
 * | Topic                             | Header                                 |
 * |-----------------------------------|----------------------------------------|
 * | Bit manipulation                  | [util/bit_ops.h](util/bit_ops.h)         |
 * | Global TDC index value type       | [util/global_index.h](util/global_index.h) |
 * | TOML loader with cutoff sentinel  | [util/toml_utils.h](util/toml_utils.h)   |
 * | Run database / run-list / readout | [util/config_reader.h](util/config_reader.h) |
 * | Circle fitting                    | [util/circle_fit.h](util/circle_fit.h)   |
 * | Ring signal model & 2-D ring fit  | [util/ring_model.h](util/ring_model.h)   |
 * | ROOT open-or-build file helper    | [util/root_io.h](util/root_io.h)         |
 * | ROOT canvas-drawing helpers       | [util/root_draw.h](util/root_draw.h)     |
 * | RAII ROOT histogram wrapper       | [util/root_hist.h](util/root_hist.h)     |
 *
 * @note The pre-Phase-5 global Mersenne-Twister (`_global_rd_`, `_global_gen_`,
 *       `_rnd_`) has been removed.  Pixel-smearing call sites use a thread-local
 *       @ref mist::Rnd from `<mist/rnd.h>`; new code must follow that pattern.
 */

#include "util/bit_ops.h"
#include "util/global_index.h"
#include "util/toml_utils.h"
#include "util/circle_fit.h"
#include "util/ring_model.h"
#include "util/root_io.h"
#include "util/root_draw.h"
#include "util/root_hist.h"
