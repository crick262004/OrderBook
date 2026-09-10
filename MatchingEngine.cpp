#include "MatchingEngine.h"

#include <cassert>

MatchingEngine::MatchingEngine(OrderIndex capacity, std::optional<unsigned> core)
    : book_{capacity}, thread_{[this] { Run(); }}
{
    if (core.has_value())
    {
        affinity_ = PinToCore(thread_, *core);
    }
}

MatchingEngine::~MatchingEngine()
{
    Stop();
}

bool MatchingEngine::TrySubmit(const Command &command) noexcept
{
    // After Stop() nobody drains the ring: a Submit would spin forever once it filled.
    assert(thread_.joinable());
    return inbound_.TryPush(command);
}

void MatchingEngine::Submit(const Command &command) noexcept
{
    while (!TrySubmit(command))
    {
    }
}

const Trade *MatchingEngine::NextTrade() noexcept
{
    return outbound_.Front();
}

void MatchingEngine::PopTrade() noexcept
{
    outbound_.Pop();
}

void MatchingEngine::Stop() noexcept
{
    if (!thread_.joinable())
    {
        return;
    }

    Submit(Command::Stop());
    thread_.join();
}

const Orderbook &MatchingEngine::Book() const noexcept
{
    assert(!thread_.joinable());
    return book_;
}

void MatchingEngine::Run()
{
    // Where the OS has no hard affinity, at least ask for its fast cores.
    PreferPerformanceCores();

    // A named lvalue, so the TradeSink built from it below points at live stack
    // for the whole loop (a FunctionRef borrows; a temporary lambda would dangle).
    // A trade is never dropped: a full outbound ring stalls matching until the
    // reporter catches up, and the back-pressure flows upstream to Submit.
    auto publish = [this](const Trade &trade) noexcept
    {
        while (!outbound_.TryPush(trade))
        {
        }
    };

    for (;;)
    {
        const Command *const command = inbound_.Front();
        if (command == nullptr)
        {
            // Busy-wait: this thread's whole job is to be here the instant a
            // command lands. No yield, no sleep, no spin hint (measured slower,
            // see Affinity.h) — it is pinned to a core for exactly this.
            continue;
        }

        if (command->kind_ == CommandKind::Stop)
        {
            inbound_.Pop();
            return;
        }

        // Apply reads the slot in place; Pop only once it is done with it.
        Apply(*command, publish);
        inbound_.Pop();
    }
}

void MatchingEngine::Apply(const Command &command, TradeSink onTrade)
{
    switch (command.kind_)
    {
    case CommandKind::Add:
        book_.AddOrder(command.ToOrder(), onTrade);
        break;
    case CommandKind::Cancel:
        book_.CancelOrder(command.orderId_);
        break;
    case CommandKind::Modify:
        book_.ModifyOrder(command.ToModify(), onTrade);
        break;
    case CommandKind::Prune:
        book_.PruneGoodForDayOrders();
        break;
    case CommandKind::Stop:
        break; // consumed by Run
    }
}
