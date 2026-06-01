#pragma once

/**
 * @file GlobalIndex.h
 * @brief Strongly-typed @ref GlobalIndex — packed (device, FIFO, chip,
 *        channel, TDC) address with built-in validation.
 *
 * @section global_index_layout Bit layout (32 bits, little-endian by bit position)
 *
 * | Field        | Width   | Bits      | Range  |
 * |--------------|---------|-----------|--------|
 * | TDC          | 2 bits  | [0,1]     | 0–3    |
 * | channel      | 6 bits  | [2,7]     | 0–63   |
 * | chip         | 3 bits  | [8,10]    | 0–7    |
 * | FIFO         | 7 bits  | [11,17]   | 0–127  |
 * | device       | 11 bits | [18,28]   | 0–2047 |
 * | reserved     | 2 bits  | [29,30]   | 0      |
 * | **valid**    | 1 bit   | [31]      | 0/1    |
 *
 * The reserved bits are strict-zero in v1 and become the schema-version
 * escape hatch if a future v2 layout is introduced.
 *
 * The validity bit (bit 31) is set to @c 1 by every legitimate constructor
 * (@ref from_components, @ref try_from_components) and is @c 0 for
 * default-constructed instances or for raw values that have not been routed
 * through a constructor.  Use @ref is_valid for a fail-fast detector of
 * those cases.
 *
 * @section global_index_split Channel encoding (current 32-ch split-in-two detector)
 *
 * The current ALCOR hardware pairs two 32-channel chips into a single
 * "logical chip" with 64 channels:
 * @code
 *     channel_logical = channel_raw + 32 * (chip_raw % 2)
 *     chip_logical    = chip_raw / 2
 * @endcode
 * The translation happens once at the framer's input boundary.  Analysis
 * code only ever sees logical chip/channel.
 *
 * For the future 64-channel chip the transform is the identity — see
 * @ref gidx::kUsesSplitInTwo.
 *
 * @section global_index_provenance Provenance contract
 *
 * @c GlobalIndex does not carry channel/chip information for *trigger*
 * events: triggers live in their own @c TriggerEvent struct and never
 * route through @c GlobalIndex.  This type is for ALCOR hits exclusively.
 *
 * @section global_index_tdc_vs_channel TDC-level vs global-channel-level addressing
 *
 * Two related "indices" coexist throughout the analysis code:
 *
 *  - **TDC-level**: the full (device, FIFO, chip, channel, TDC) address.
 *    Identifies one of four TDCs on a specific channel.  This is the
 *    @c GlobalIndex raw value by default.  Used by per-TDC calibration
 *    (`AlcorFinedata::calibration_parameters`).
 *  - **Global-channel-level**: same address with the TDC bits zeroed.
 *    Identifies a single SiPM channel (one of the ~512 logical channels
 *    per device) without distinguishing which of its 4 TDCs fired.  Used
 *    as a key for per-channel Mapping (position, dead-channel mask,
 *    same-channel Δt bookkeeping).
 *
 * The accessors are:
 *
 *  - @ref raw / @ref tdc — keep the TDC component.
 *  - @ref global_channel_raw / @ref global_channel — clear the TDC bits,
 *    keep everything else.
 *  - @ref channel_ordinal — small dense counter-style int (one bin per
 *    channel) suitable for histogram axes.
 *
 * Equality of two @ref global_channel values means "same physical channel,
 * possibly different TDCs"; equality of two @ref raw values means "same
 * TDC channel".
 *
 * @note  "Channel" is overloaded here.  The accessor @ref channel returns
 *        the 6-bit channel field *within a single chip* (0–63).  The
 *        helpers above (@ref global_channel, @ref global_channel_raw) refer
 *        to the full per-channel global address.  The detector-level
 *        accessors @ref column and @ref pixel return the ALCOR ASIC's
 *        column-and-row layout (kept as a hardware geometry helper).
 */

#include <cassert>
#include <cstdint>
#include <optional>

namespace gidx
{
/// @brief Build-time flag controlling whether the helper functions
/// @ref GlobalIndex::real_chip and @ref GlobalIndex::chip_local_channel
/// apply the split-in-two transform.
///
/// Set to @c true while the analysis runs on the 32-channel ALCOR
/// detector that pairs chip 0+1, 2+3, … into 64-channel logical chips.
/// Flip to @c false (and rebuild) when the final 64-channel chip is
/// deployed — the two helpers then collapse to the identity.
static constexpr bool kUsesSplitInTwo = true;

/// @brief Lowest ALCOR device id in the readout — the base from which
/// the channel ordinal is derived (`channel_ordinal = (device - kFirstDevice) * …`).
/// Cherenkov devices occupy @c [kFirstDevice, kTimingDeviceLo); the timing
/// chip sits at @c kTimingDeviceLo.  Single source of truth for the 192
/// base previously duplicated across the writers and @ref mapping.
static constexpr int kFirstDevice = 192;

/// @brief Boundary device id separating Cherenkov devices (@c < kTimingDeviceLo)
/// from the timing chip (@c ≥ kTimingDeviceLo).  The canonical
/// `device < kTimingDeviceLo` test for "is this a Cherenkov device".
static constexpr int kTimingDeviceLo = 200;

/// @brief Generous exclusive upper bound for device-iteration loops.
/// @ref Mapping filters unmapped devices, so this only needs to bracket
/// the populated range; it is intentionally loose, not a hard count.
static constexpr int kDeviceUpperBound = 224;
} // namespace gidx

/**
 * @brief Validated, packed address of a single ALCOR TDC channel.
 *
 * Stored as a single @c uint32_t (the @ref raw value).  All construction
 * paths set the validity bit at position 31; @ref is_valid distinguishes a
 * blessed construction from a default-constructed or pre-migration raw.
 *
 * @par Storage
 * @c GlobalIndex can be stored in a ROOT TTree branch as a @c UInt_t; the
 * branch sees only the 32-bit raw.  Decode on read with the @c uint32_t
 * constructor (for new-format files) or @ref from_legacy (for old-format).
 *
 * @par Thread safety
 * Trivially copyable and immutable after construction — safe to share
 * across threads.
 */
class GlobalIndex
{
public:
    // ------------------------------------------------------------------
    //  Bit layout constants
    // ------------------------------------------------------------------

    /// @name Field widths and shifts
    /// @{
    static constexpr int kTdcBits = 2;
    static constexpr int kChannelBits = 6;
    static constexpr int kChipBits = 3;
    static constexpr int kFifoBits = 7;
    static constexpr int kDeviceBits = 11;
    static constexpr int kReservedBits = 2;

    static constexpr int kTdcShift = 0;
    static constexpr int kChannelShift = kTdcShift + kTdcBits;        // 2
    static constexpr int kChipShift = kChannelShift + kChannelBits;   // 8
    static constexpr int kFifoShift = kChipShift + kChipBits;         // 11
    static constexpr int kDeviceShift = kFifoShift + kFifoBits;       // 18
    static constexpr int kReservedShift = kDeviceShift + kDeviceBits; // 29

    /// @brief Bit set by every legitimate constructor; checked by @ref is_valid.
    static constexpr uint32_t kValidBit = uint32_t{1} << 31;
    /// @}

    /// @name Field masks (post-shift)
    /// @{
    static constexpr uint32_t kTdcMask = (uint32_t{1} << kTdcBits) - 1;           // 0x03
    static constexpr uint32_t kChannelMask = (uint32_t{1} << kChannelBits) - 1;   // 0x3F
    static constexpr uint32_t kChipMask = (uint32_t{1} << kChipBits) - 1;         // 0x07
    static constexpr uint32_t kFifoMask = (uint32_t{1} << kFifoBits) - 1;         // 0x7F
    static constexpr uint32_t kDeviceMask = (uint32_t{1} << kDeviceBits) - 1;     // 0x7FF
    static constexpr uint32_t kReservedMask = (uint32_t{1} << kReservedBits) - 1; // 0x03
    /// @}

    // ------------------------------------------------------------------
    //  Construction
    // ------------------------------------------------------------------

    /// @brief Default-constructed GlobalIndex — @c raw_ is 0 and @ref is_valid returns @c false.
    /// Use the named factories to obtain a valid instance.
    constexpr GlobalIndex() noexcept = default;

    /// @brief Reconstruct a @c GlobalIndex from a stored raw value.
    ///
    /// Explicit to prevent silent @c int → @c GlobalIndex conversions.
    /// Used after reading from a TTree branch.
    constexpr explicit GlobalIndex(uint32_t raw) noexcept : raw_(raw) {}

    /// @brief Validated factory returning @c std::nullopt on any out-of-range component.
    ///
    /// Prefer this over @ref from_components when parsing data of unknown
    /// provenance (raw hardware readout, file with optional fields, …).
    /// The returned @c GlobalIndex always has @ref is_valid @c true.
    [[nodiscard]] static constexpr std::optional<GlobalIndex>
    try_from_components(int device, int fifo, int chip, int channel, int tdc) noexcept
    {
        if (device < 0 || device >= (1 << kDeviceBits))
            return std::nullopt;
        if (fifo < 0 || fifo >= (1 << kFifoBits))
            return std::nullopt;
        if (chip < 0 || chip >= (1 << kChipBits))
            return std::nullopt;
        if (channel < 0 || channel >= (1 << kChannelBits))
            return std::nullopt;
        if (tdc < 0 || tdc >= (1 << kTdcBits))
            return std::nullopt;
        return GlobalIndex(
            kValidBit |
            (uint32_t(device) << kDeviceShift) |
            (uint32_t(fifo) << kFifoShift) |
            (uint32_t(chip) << kChipShift) |
            (uint32_t(channel) << kChannelShift) |
            uint32_t(tdc) /* shift 0 */);
    }

    /// @brief Asserting factory built on @ref try_from_components.
    ///
    /// Debug builds @c assert that every component is in range; release
    /// builds skip the check (zero overhead).  Use only when the caller
    /// *knows* its inputs are valid.
    [[nodiscard]] static constexpr GlobalIndex
    from_components(int device, int fifo, int chip, int channel, int tdc)
    {
        auto v = try_from_components(device, fifo, chip, channel, tdc);
        assert(v.has_value() && "GlobalIndex: out-of-range component");
        return *v;
    }

    // Note: the legacy-format adapters (`from_legacy`, `from_legacy_channel`)
    // were removed at the end of the migration.  Storage is the new
    // layout natively; pre-migration `.root` files must be re-written by
    // re-running the writers on the raw FIFO input.

    /// @brief Inverse of @ref tdc_ordinal — reconstruct the packed
    /// `GlobalIndex` from the dense, counter-style integer index used
    /// in histogram x-axes that bin one TDC sub-cell per bin.
    ///
    /// The forward direction is
    /// @code
    ///   tdc_ordinal() = channel_ordinal() * 4 + tdc()
    /// @endcode
    /// with @ref channel_ordinal layout depending on
    /// @ref gidx::kUsesSplitInTwo:
    /// - split-in-two (current 32-ch detector):
    ///   `(device - 192) * 256 + real_chip * 32 + chip_local_channel`
    /// - flat (final 64-ch detector):
    ///   `(device - 192) * 512 + chip * 64 + channel`
    ///
    /// This factory inverts the formula and returns @c std::nullopt
    /// when the recovered components are out-of-range.  Useful for
    /// downstream code that consumes a per-TDC histogram whose
    /// x-axis was filled via @ref tdc_ordinal — e.g.
    /// @c AlcorFinedata::generate_calibration.
    [[nodiscard]] static constexpr std::optional<GlobalIndex>
    try_from_tdc_ordinal(int ordinal) noexcept
    {
        if (ordinal < 0)
            return std::nullopt;
        const int tdc_idx       = ordinal & 0x3;
        const int channel_ord   = ordinal >> 2;
        if constexpr (gidx::kUsesSplitInTwo)
        {
            constexpr int kChansPerDevice = 256;
            constexpr int kChansPerChip   = 32;
            const int device_offset       = channel_ord / kChansPerDevice;
            const int device_id           = gidx::kFirstDevice + device_offset;
            const int channel_in_device   = channel_ord % kChansPerDevice;
            const int real_chip_id        = channel_in_device / kChansPerChip;
            const int chip_local_chan     = channel_in_device % kChansPerChip;
            //  Recover the new-layout (chip_logical, channel_logical)
            //  pair from (real_chip_id, chip_local_chan).  In the
            //  split-in-two scheme each pair of hardware chips maps
            //  to one logical chip with channels [0..63]; the upper
            //  32 channels come from the odd-indexed real chip.
            const int chip_logical    = real_chip_id / 2;
            const int channel_logical = chip_local_chan + (real_chip_id & 1 ? 32 : 0);
            //  FIFO follows the same packing the writers use
            //  elsewhere (see pulser_calib_writer).
            const int fifo_id = 4 * chip_logical + ((channel_logical & 31) >> 3);
            return try_from_components(device_id, fifo_id, chip_logical, channel_logical, tdc_idx);
        }
        else
        {
            constexpr int kChansPerDevice = 512;
            constexpr int kChansPerChip   = 64;
            const int device_offset     = channel_ord / kChansPerDevice;
            const int device_id         = gidx::kFirstDevice + device_offset;
            const int channel_in_device = channel_ord % kChansPerDevice;
            const int chip_id           = channel_in_device / kChansPerChip;
            const int channel_id        = channel_in_device % kChansPerChip;
            const int fifo_id           = 4 * chip_id + ((channel_id & 31) >> 3);
            return try_from_components(device_id, fifo_id, chip_id, channel_id, tdc_idx);
        }
    }

    // ------------------------------------------------------------------
    //  Field accessors
    // ------------------------------------------------------------------

    /// @brief Raw 32-bit representation — store this in a TTree branch.
    [[nodiscard]] constexpr uint32_t raw() const noexcept { return raw_; }

    [[nodiscard]] constexpr int tdc() const noexcept { return static_cast<int>((raw_ >> kTdcShift) & kTdcMask); }
    [[nodiscard]] constexpr int channel() const noexcept { return static_cast<int>((raw_ >> kChannelShift) & kChannelMask); }
    [[nodiscard]] constexpr int chip() const noexcept { return static_cast<int>((raw_ >> kChipShift) & kChipMask); }
    [[nodiscard]] constexpr int fifo() const noexcept { return static_cast<int>((raw_ >> kFifoShift) & kFifoMask); }
    [[nodiscard]] constexpr int device() const noexcept { return static_cast<int>((raw_ >> kDeviceShift) & kDeviceMask); }

    /// @brief @c true iff the validity bit (bit 31) is set.
    ///
    /// Returns @c false for default-constructed instances, raw zeros, and
    /// any unmigrated legacy value passed directly to the @c uint32_t
    /// constructor (use @ref from_legacy on those instead).
    [[nodiscard]] constexpr bool is_valid() const noexcept { return (raw_ & kValidBit) != 0; }

    /// @brief @c true iff the reserved bits (29–30) are zero.  v1 layout requires this.
    [[nodiscard]] constexpr bool reserved_bits_are_zero() const noexcept
    {
        return ((raw_ >> kReservedShift) & kReservedMask) == 0;
    }

    // ------------------------------------------------------------------
    //  Global-channel view (TDC bits zeroed)
    // ------------------------------------------------------------------

    /// @brief Raw 32-bit value with the TDC bits cleared.
    ///
    /// Two @c GlobalIndex instances with the same @ref global_channel_raw
    /// share the same physical channel; they may still differ in TDC.  Use
    /// this as a key for per-channel maps (position, dead-channel mask,
    /// same-channel Δt bookkeeping).
    ///
    /// @note "Channel" here means the full (device, FIFO, chip, channel)
    ///       address — one of the 8 × 64 logical SiPM channels per device.
    ///       Not to be confused with the @ref channel accessor, which
    ///       returns the 6-bit channel field within a single chip.
    [[nodiscard]] constexpr uint32_t global_channel_raw() const noexcept
    {
        return raw_ & ~(uint32_t(kTdcMask) << kTdcShift);
    }

    /// @brief A channel-level sibling of this index — same device/FIFO/chip/channel,
    /// TDC zeroed.  Validity bit is preserved.
    ///
    /// @code
    /// GlobalIndex gi = GlobalIndex::from_components(197, 5, 3, 42, 1);
    /// GlobalIndex gc = gi.global_channel();
    /// assert(gc.tdc() == 0);
    /// assert(gc.channel() == gi.channel());
    /// assert(gc.is_valid());
    /// @endcode
    [[nodiscard]] constexpr GlobalIndex global_channel() const noexcept
    {
        return GlobalIndex(global_channel_raw());
    }

    /// @brief Dense, counter-style per-channel ordinal — suitable for
    ///        histogram axes that bin one channel per bin.
    ///
    /// Returns a *small* integer (in contrast to @ref global_channel_raw,
    /// which carries the full packed encoding including the validity and
    /// device bits, and produces sparse values with large gaps between
    /// devices).  The formula matches the legacy `tdc_raw / 4` value
    /// **bit-exact** for the current 32-ch detector, so existing
    /// hitmap histograms binned on the legacy channel-level integer
    /// continue to work with the packed GlobalIndex storage — just
    /// substitute `gi.channel_ordinal()` for `legacy_tdc_raw / 4`.
    ///
    /// @note  Two ordinals refer to the same physical channel iff the
    ///        underlying @ref global_channel values are equal, but the
    ///        ordinal is dense (no gaps) and small enough to use as a
    ///        TH1F/TH2F axis without wasting bins.
    ///
    /// @par Formula
    /// - Current detector (split-in-two, `gidx::kUsesSplitInTwo == true`):
    ///   @code (device - 192) * 256 + real_chip * 32 + chip_local_channel @endcode
    ///   Range: 0 to (max_device - 192) × 256 + 255.  For the current
    ///   beam-test (devices 192–207), the range is 0..4095.
    /// - Final detector (`gidx::kUsesSplitInTwo == false`):
    ///   @code (device - 192) * 512 + chip * 64 + channel @endcode
    ///   The factor doubles because each chip now has 64 channels.  The
    ///   `-192` offset is retained for continuity with current data; when
    ///   the final detector lands, re-evaluate whether to drop it.
    [[nodiscard]] constexpr int channel_ordinal() const noexcept
    {
        // device() is uint16_t; subtracting kFirstDevice underflows wildly when
        // the GlobalIndex is default-constructed (device() == 0).  Callers using
        // the return value as a histogram bin would then index huge negative
        // bins  Guard with an assert (debug) and saturate
        // to 0 on the invalid case (release) so the bin index stays sensible.
        assert(is_valid() && device() >= gidx::kFirstDevice &&
               "channel_ordinal: requires is_valid() && device() >= kFirstDevice");
        if (!is_valid() || device() < gidx::kFirstDevice)
            return 0;
        if constexpr (gidx::kUsesSplitInTwo)
            return (device() - gidx::kFirstDevice) * 256 + real_chip() * 32 + chip_local_channel();
        else
            return (device() - gidx::kFirstDevice) * 512 + chip() * 64 + channel();
    }

    /// @brief Dense, counter-style per-TDC ordinal — `channel_ordinal * 4 + tdc`.
    ///
    /// Bit-exact equivalent of the pre-Phase-5 "global tdc index" (the
    /// legacy `tdc_raw` value): a small integer in [0, 4 × max_ordinal + 3]
    /// suitable for histogram axes that bin one TDC sub-cell per bin.
    ///
    /// Use this in place of @ref raw() / @ref get_global_tdc_index() when
    /// the value is going to a `TH1::Fill` / `SetBinContent` — those need a
    /// dense small integer, not the packed 32-bit form.
    [[nodiscard]] constexpr int tdc_ordinal() const noexcept
    {
        return channel_ordinal() * 4 + tdc();
    }

    // ------------------------------------------------------------------
    //  Derived helpers
    // ------------------------------------------------------------------

    /// @brief Even-odd channel index — alias for @ref channel in the new layout (0–63).
    [[nodiscard]] constexpr int eo_channel() const noexcept { return channel(); }

    /// @brief Physical hardware chip — collapses the split-in-two pairing on the current detector.
    ///
    /// When @ref gidx::kUsesSplitInTwo is @c true (current 32-ch detector):
    /// @code 2 * chip() + (channel() >> 5) @endcode
    /// returning the original 0–7 hardware chip index.
    ///
    /// When @ref gidx::kUsesSplitInTwo is @c false (final 64-ch detector):
    /// the identity — @ref chip already is the physical chip.
    [[nodiscard]] constexpr int real_chip() const noexcept
    {
        if constexpr (gidx::kUsesSplitInTwo)
            return 2 * chip() + (channel() >> 5);
        else
            return chip();
    }

    /// @brief Channel index within the physical hardware chip.
    ///
    /// When @ref gidx::kUsesSplitInTwo is @c true: returns 0–31 (the
    /// channel within one of the two paired hardware chips).
    /// When @c false: returns 0–63 — the channel field directly.
    ///
    /// Meaningful in both detector generations; kept as a permanent
    /// helper.
    [[nodiscard]] constexpr int chip_local_channel() const noexcept
    {
        if constexpr (gidx::kUsesSplitInTwo)
            return channel() & 0x1F;
        else
            return channel();
    }

    // ------------------------------------------------------------------
    //  Legacy address-derivation helpers (kept inside the type so the
    //  arithmetic is centralised and stays in sync with the layout)
    // ------------------------------------------------------------------

    /// @brief Column address within the hardware chip (0–7).
    ///
    /// ALCOR pixel layout: an ASIC arranges its 32 pixels into 8 columns × 4
    /// rows.  Formula: `(channel_local % 32) / 4`.  Kept as a detector-level
    /// helper even though no analysis code currently consumes it directly —
    /// future geometry/Mapping code is the expected consumer.
    [[nodiscard]] constexpr int column() const noexcept
    {
        return chip_local_channel() / 4;
    }

    /// @brief Pixel position within an ALCOR column (0–3).
    ///
    /// Returns the row within the column (column is given by @ref column).
    /// Kept as a detector-level helper for the same reason as @ref column —
    /// the ALCOR pixel layout is a fixed-hardware concept worth exposing
    /// even if currently unused at the analysis layer.
    [[nodiscard]] constexpr int pixel() const noexcept
    {
        return chip_local_channel() % 4;
    }

    /// @brief Calibration look-up index — legacy-compatible formula.
    ///
    /// Returns `tdc + 4 * eo_channel + 128 * real_chip`.
    ///
    /// @deprecated this key omits the `device` field, so
    /// multiple devices collide on the same value (a historical bug
    /// in the legacy fine_calib.txt format, since retired in task #172).
    /// Use @ref raw() as the calibration key instead — it encodes
    /// every component and uniquely identifies a TDC across the whole
    /// detector.  Kept here only for downstream callers that haven't
    /// migrated yet; do not use in new code.
    [[deprecated("Use GlobalIndex::raw() as the calibration key — calib_index() collides across devices.")]]
    [[nodiscard]] constexpr int
    calib_index() const noexcept
    {
        return tdc() + 4 * eo_channel() + 128 * real_chip();
    }

    /// @brief Per-device flat index (legacy formula).
    [[nodiscard]] constexpr int device_index() const noexcept
    {
        return eo_channel() + 64 * (chip() / 2);
    }

    // ------------------------------------------------------------------
    //  Comparison
    // ------------------------------------------------------------------

    friend constexpr bool operator==(GlobalIndex a, GlobalIndex b) noexcept { return a.raw_ == b.raw_; }
    friend constexpr bool operator!=(GlobalIndex a, GlobalIndex b) noexcept { return a.raw_ != b.raw_; }
    friend constexpr bool operator<(GlobalIndex a, GlobalIndex b) noexcept { return a.raw_ < b.raw_; }

private:
    uint32_t raw_ = 0; ///< Default = 0 → @ref is_valid @c false until blessed.
};

static_assert(sizeof(GlobalIndex) == 4,
              "GlobalIndex must be exactly 4 bytes for TTree branch compatibility");
