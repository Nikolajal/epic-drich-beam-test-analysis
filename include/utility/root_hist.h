#pragma once

/**
 * @file utility/root_hist.h
 * @brief Owning RAII wrapper for ROOT histograms — a thin ergonomic shim over
 *        @ref mist::hep::owned::root_ptr.
 *
 * The ownership rules ROOT fights (detach-on-create so an open `TFile` can't
 * double-free a histogram; ownership-correct deletion) live in **mist-hep**
 * (`mist::hep::owned::make/clone/adopt` + `root_deleter`).  `RootHist<T>` adds
 * only what the writers rely on on top of that owning handle:
 *
 *  - in-place construction `RootHist<T> h("name", title, …)` (forwards to
 *    `owned::make<T>`),
 *  - adoption of an existing pointer `RootHist<T>(other->Clone(...))`
 *    (forwards to `owned::adopt<T>`),
 *  - a move-only, default-constructible value type so
 *    `std::unordered_map<K, RootHist<T>>` compiles and never copies a pointer,
 *  - pointer-like access (`operator->`, `*`, `get`, `bool`, `release`,
 *    `reset`).
 *
 * Semantics are unchanged from the previous standalone implementation — the
 * detach/deletion logic is just sourced from mist-hep now instead of duplicated
 * here.  Does not auto-write on destruction (explicit `Write()` calls are
 * preserved); `enable_auto_write()` opts in, kept for interface compatibility.
 *
 * ## Usage
 *
 *     RootHist<TH1F> h("h_dt", ";dt;entries", 100, 0, 10);
 *     h->Fill(value);                  // operator-> proxies to TH1F
 *     h->Write();                      // explicit Write() — lands in gDirectory
 *
 *     RootHist<TH1F> clone(static_cast<TH1F*>(other->Clone("clone")));
 *     std::unordered_map<int, RootHist<TH1F>> per_channel;  // move-only value
 */

#include <type_traits>
#include <utility>

#include <TDirectory.h>

#include <mist/hep/owned.h>

template <typename T>
class RootHist
{
    mist::hep::owned::root_ptr<T> h_;
    TDirectory *write_dir_ = nullptr;
    bool auto_write_ = false;
    bool written_ = false;

public:
    // ── Construction ────────────────────────────────────────────────────────

    /// Empty wrapper (no histogram) — so `map<K, RootHist<T>>::operator[]` can
    /// default-construct.  Its `operator->` returns `nullptr`, exactly as the
    /// previous implementation and raw-pointer code did.
    RootHist() noexcept = default;

    /// Construct the histogram in place via `owned::make<T>` (detaches it from
    /// gDirectory).  SFINAE-disabled for the single-`T*` case so the adopting
    /// ctor below is chosen for that overload.
    template <typename... Args,
              typename = std::enable_if_t<
                  std::is_constructible_v<T, Args...> &&
                  !(sizeof...(Args) == 1 &&
                    std::conjunction_v<std::is_same<std::decay_t<Args>, T *>...>)>>
    explicit RootHist(Args &&...args)
        : h_(mist::hep::owned::make<T>(std::forward<Args>(args)...))
    {
    }

    /// Adopt an already-allocated histogram (e.g. a `Clone()`) via
    /// `owned::adopt<T>` — takes ownership and detaches it from gDirectory.
    explicit RootHist(T *raw) noexcept : h_(mist::hep::owned::adopt<T>(raw)) {}

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

    /// Replace the held histogram; the new one is detached from gDirectory.
    void reset(T *raw = nullptr) noexcept
    {
        h_ = mist::hep::owned::adopt<T>(raw);
        written_ = false;
    }

    // ── Optional write-at-destruction (kept for interface compatibility) ─────

    RootHist &enable_auto_write(TDirectory *dir = nullptr) noexcept
    {
        auto_write_ = true;
        write_dir_ = dir;
        return *this;
    }

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
