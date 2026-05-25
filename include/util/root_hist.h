#pragma once

/**
 * @file util/root_hist.h
 * @brief Owning RAII wrapper for ROOT histograms.
 *
 * `RootHist<T>` is a `unique_ptr`-style wrapper for `TH1F`, `TH2F`,
 * `TProfile`, and friends that closes three latent problems with bare
 * `new TH*` allocation:
 *
 *  1. **Leak on early return / exception.** Bare `new TH1F(...)` paired with
 *     an end-of-function `outfile->Close()` leaks on every non-default
 *     control-flow exit.  Compounds linearly with the number of runs on
 *     a runlist driver.
 *  2. **Stray-`gDirectory` ownership.** `TH1::AddDirectory(true)` is the
 *     ROOT default — every fresh histogram silently attaches itself to
 *     whatever `gDirectory` happens to be at construction.  If that
 *     directory belongs to an input `TFile` (very common after a
 *     `->Clone()` whose source was loaded from disk), closing the input
 *     file frees the clone under the caller's feet
 *     (CODE_REVIEW §6.10).  This wrapper calls `SetDirectory(nullptr)`
 *     in its constructor so the wrapped histogram is owned by the
 *     wrapper alone — no ROOT-side surprises.
 *  3. **Copyable raw pointers in containers.** `std::map<K, TH1F*>`
 *     accidentally duplicates the pointer on copy and reshuffle; with
 *     `RootHist<T>` the value is move-only, so any miswiring becomes a
 *     compile error.
 *
 * ## Usage
 *
 *     RootHist<TH1F> h("h_dt", ";dt;entries", 100, 0, 10);
 *     h->Fill(value);                  // operator-> proxies to TH1F
 *     h->Write();                      // explicit Write() — lands in gDirectory
 *
 * The wrapper does **not** auto-write on destruction by default.  Existing
 * explicit `Write()` calls in the writer code are preserved as-is.  To opt
 * in to a write-at-destruction (e.g. when migrating a function whose write
 * section is otherwise hard to express), call:
 *
 *     h.enable_auto_write(target_dir);     // writes to target_dir at scope exit
 *
 * If a manual `Write()` also fires through `operator->`, call `mark_written()`
 * after it so the dtor skips the duplicate.  (Or just don't enable auto-write
 * on histograms you write explicitly.)
 *
 * ## Adopting an existing histogram (e.g. from Clone())
 *
 *     RootHist<TH1F> clone(static_cast<TH1F*>(other->Clone("clone")));
 *     // ctor detaches `clone` from gDirectory — closes §6.10 by construction.
 *
 * ## Maps and vectors
 *
 *     std::unordered_map<int, RootHist<TH1F>> per_channel;
 *     per_channel.try_emplace(ch, "name", "title", 100, 0, 10);
 *     per_channel[ch] = RootHist<TH1F>("name", "title", 100, 0, 10);  // also OK
 *     for (auto &[k, v] : per_channel)   // use auto&[]: RootHist is move-only
 *         v->Write();
 *
 * Move-only; no copy.  Default-constructible (holds nothing) so `map::operator[]`
 * still compiles.
 */

#include <memory>
#include <type_traits>
#include <utility>
#include <TDirectory.h>

namespace rh_detail
{
/// Trait: does `T` expose `SetDirectory(TDirectory*)`?  TH1/TH2/TH3/TProfile
/// do; TGraph does not.  Used to conditionally detach in the ctor.
template <typename U, typename = void>
struct has_set_directory : std::false_type {};

template <typename U>
struct has_set_directory<
    U,
    std::void_t<decltype(std::declval<U &>().SetDirectory(static_cast<TDirectory *>(nullptr)))>>
    : std::true_type {};
} // namespace rh_detail

template <typename T>
class RootHist
{
    std::unique_ptr<T> h_;
    TDirectory *write_dir_ = nullptr;
    bool auto_write_ = false;
    bool written_ = false;

    void detach_from_dir_() noexcept
    {
        if constexpr (rh_detail::has_set_directory<T>::value)
            if (h_)
                h_->SetDirectory(nullptr);
    }

public:
    // ── Construction ────────────────────────────────────────────────────────

    /// Empty wrapper (no histogram).  Needed so `map<K, RootHist<T>>::operator[]`
    /// can default-construct values.  An empty wrapper's `operator->` returns
    /// `nullptr` — dereferencing it crashes, same as today's raw-pointer code.
    RootHist() noexcept = default;

    /// Construct the histogram in place.  Forwarded to `T(args...)`.
    /// SFINAE-disabled when there is no matching `T` ctor for `args...` — in
    /// particular this lets the adopting ctor below pick up `RootHist<T>(T*)`
    /// without colliding here.
    template <typename... Args,
              typename = std::enable_if_t<
                  std::is_constructible_v<T, Args...> &&
                  !(sizeof...(Args) == 1 &&
                    std::conjunction_v<std::is_same<std::decay_t<Args>, T *>...>)>>
    explicit RootHist(Args &&...args)
        : h_(std::make_unique<T>(std::forward<Args>(args)...))
    {
        detach_from_dir_();
    }

    /// Adopt an already-allocated histogram — e.g. the result of a `Clone()`.
    /// The wrapper takes ownership; the wrapped histogram is detached from
    /// `gDirectory` immediately so closing any input file does not free it.
    explicit RootHist(T *raw) noexcept : h_(raw)
    {
        detach_from_dir_();
    }

    // ── Copy disabled, move allowed ─────────────────────────────────────────

    RootHist(const RootHist &) = delete;
    RootHist &operator=(const RootHist &) = delete;
    RootHist(RootHist &&) noexcept = default;
    RootHist &operator=(RootHist &&) noexcept = default;

    // ── Pointer-like access ─────────────────────────────────────────────────

    T *operator->() const noexcept { return h_.get(); }
    T &operator*() const noexcept { return *h_; }
    T *get() const noexcept { return h_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(h_); }

    // ── Lifecycle helpers ───────────────────────────────────────────────────

    /// Release ownership; caller becomes responsible for the histogram.
    T *release() noexcept { return h_.release(); }

    /// Replace the held histogram.  The new one is detached from gDirectory.
    void reset(T *raw = nullptr) noexcept
    {
        h_.reset(raw);
        detach_from_dir_();
        written_ = false;
    }

    // ── Optional write-at-destruction ───────────────────────────────────────

    /// Opt in to writing the histogram in @p dir at scope exit.  Pass
    /// `nullptr` (the default) to write to whatever `gDirectory` is at
    /// destruction time — almost never what you want.  Pass an explicit dir.
    RootHist &enable_auto_write(TDirectory *dir = nullptr) noexcept
    {
        auto_write_ = true;
        write_dir_ = dir;
        return *this;
    }

    /// Tell the wrapper "I already wrote this" — suppresses the dtor write.
    /// Call after a manual `Write()` if you had also called
    /// `enable_auto_write()`.
    void mark_written() noexcept { written_ = true; }

    ~RootHist()
    {
        if (auto_write_ && h_ && !written_)
        {
            TDirectory::TContext ctx(write_dir_ ? write_dir_ : gDirectory);
            h_->Write();
        }
    }
};
