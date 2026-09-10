#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <thread>

#include "Affinity.h"
#include "Command.h"
#include "Orderbook.h"
#include "SpscQueue.h"
#include "Trade.h"
#include "Usings.h"

// The concurrency boundary. One dedicated thread owns the Orderbook outright and
// is the only thread that ever touches it, so the book keeps no locks and knows
// nothing about threads. Everyone else talks to it through two SPSC rings:
// commands in (Submit — one feed thread), trades out (NextTrade/PopTrade — one
// reporting thread, which may be the feed thread). Both rings are allocated
// once here; in steady state no thread does any heap work.
//
// Shutdown is a Stop command in the inbound ring (a poison pill): it queues
// behind every command submitted before it, so "everything submitted is
// applied" falls out of the FIFO with no second synchronisation channel. After
// Stop() returns the thread is joined and Book() may be inspected.
//
// The matching thread busy-waits, so it wants a core of its own: pass `core` to
// pin it there (hard affinity on Linux; macOS reports Unsupported and the thread
// merely asks for performance-core scheduling). Affinity() tells the outcome.
class MatchingEngine
{
public:
    static constexpr std::size_t QueueCapacity = 1u << 16;

    using Commands = SpscQueue<Command, QueueCapacity>;
    using TradeQueue = SpscQueue<Trade, QueueCapacity>;

    explicit MatchingEngine(OrderIndex capacity = Orderbook::DefaultCapacity,
                            std::optional<unsigned> core = std::nullopt);

    // The matching thread captures `this`.
    MatchingEngine(const MatchingEngine &) = delete;
    MatchingEngine &operator=(const MatchingEngine &) = delete;
    MatchingEngine(MatchingEngine &&) = delete;
    MatchingEngine &operator=(MatchingEngine &&) = delete;
    ~MatchingEngine();

    // Feed side (one thread), valid until Stop(). False = inbound ring full:
    // back-pressure for the caller to handle. Submit spins until there is room.
    [[nodiscard]] bool TrySubmit(const Command &command) noexcept;
    void Submit(const Command &command) noexcept;

    // Reporting side (one thread), zero-copy: read the trade, then PopTrade.
    [[nodiscard]] const Trade *NextTrade() noexcept;
    void PopTrade() noexcept;

    // Pushes the poison pill and joins the matching thread. Idempotent.
    void Stop() noexcept;

    // Only after Stop(): while the thread runs, the book is its alone.
    [[nodiscard]] const Orderbook &Book() const noexcept;

    // Outcome of the pin request: a value when pinned (or when no core was asked
    // for), otherwise why the OS declined.
    [[nodiscard]] std::expected<void, AffinityError> Affinity() const noexcept { return affinity_; }

private:
    void Run();
    void Apply(const Command &command, TradeSink onTrade);

    Orderbook book_;
    Commands inbound_;
    TradeQueue outbound_;
    std::expected<void, AffinityError> affinity_;
    // std::thread, not std::jthread: Apple Clang 17's libc++ (the README's floor)
    // lacks jthread, and nothing here needs it — Stop() joins explicitly and no
    // stop_token is ever requested. Last member: every member it reads is
    // initialised before it starts.
    std::thread thread_;
};
