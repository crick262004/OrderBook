#pragma once

#include <cstdint>
#include <type_traits>

#include "Order.h"
#include "OrderModify.h"
#include "OrderType.h"
#include "Side.h"
#include "Usings.h"

enum class CommandKind : std::uint8_t
{
    Add,
    Cancel,
    Modify,
    Prune,
    Stop, // poison pill: the matching thread applies everything queued before it, then exits
};

// One inbound-ring slot: a flat, trivially copyable record of one book
// operation. Add/Modify carry the order's fields inline rather than an Order or
// OrderModify object, so the slot is a 24-byte POD that the ring copies by value.
struct Command
{
    OrderId orderId_{};
    Price price_{};
    Quantity quantity_{};
    CommandKind kind_{};
    OrderType orderType_{};
    Side side_{};

    [[nodiscard]] static Command Add(const Order &order) noexcept
    {
        return Command{order.GetOrderId(), order.GetPrice(),     order.GetInitialQuantity(),
                       CommandKind::Add,   order.GetOrderType(), order.GetSide()};
    }

    [[nodiscard]] static Command Cancel(OrderId orderId) noexcept
    {
        Command command;
        command.orderId_ = orderId;
        command.kind_ = CommandKind::Cancel;
        return command;
    }

    // orderType_ is left defaulted: a modify keeps the resting order's type, which
    // only the book knows.
    [[nodiscard]] static Command Modify(const OrderModify &modify) noexcept
    {
        Command command;
        command.orderId_ = modify.GetOrderId();
        command.price_ = modify.GetPrice();
        command.quantity_ = modify.GetQuantity();
        command.kind_ = CommandKind::Modify;
        command.side_ = modify.GetSide();
        return command;
    }

    [[nodiscard]] static Command Prune() noexcept
    {
        Command command;
        command.kind_ = CommandKind::Prune;
        return command;
    }

    [[nodiscard]] static Command Stop() noexcept
    {
        Command command;
        command.kind_ = CommandKind::Stop;
        return command;
    }

    [[nodiscard]] Order ToOrder() const noexcept { return Order{orderType_, orderId_, side_, price_, quantity_}; }
    [[nodiscard]] OrderModify ToModify() const noexcept { return OrderModify{orderId_, side_, price_, quantity_}; }
};

static_assert(std::is_trivially_copyable_v<Command>);
static_assert(sizeof(Command) == 24, "largest-first layout: 16 bytes of scalars, three 1-byte enums, pad to 8");
