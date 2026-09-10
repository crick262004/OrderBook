#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

// Non-owning reference to a callable — a stand-in for C++26 std::function_ref.
// Two words: the callable's address (type forgotten) and a thunk that knows the
// real type and casts back. Trivially copyable, no allocation, one indirect call
// to invoke. It lets non-template code in a .cpp call any lambda without seeing
// its type, and without std::function's ownership, size, or small-buffer heap.
//
// It borrows the callable, so it is a *parameter* type: valid for the duration
// of the call it is passed to. Storing one as a member initialised from a
// temporary lambda dangles the moment the initialiser ends.
template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)>
{
public:
    // Class-type callables only (lambdas, functors): a plain function has no
    // object to point at. Deduced as a forwarding reference so lvalues bind by
    // address and never copy.
    template <typename F>
        requires(std::is_class_v<std::remove_cvref_t<F>> && !std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
                 std::is_invocable_r_v<R, F &, Args...>)
    FunctionRef(F &&callable) noexcept
        : object_{const_cast<void *>(static_cast<const void *>(std::addressof(callable)))},
          invoke_{[](void *object, Args... args) -> R
                  { return std::invoke(*static_cast<std::add_pointer_t<F>>(object), std::forward<Args>(args)...); }}
    {
    }

    R operator()(Args... args) const { return invoke_(object_, std::forward<Args>(args)...); }

private:
    void *object_;
    R (*invoke_)(void *, Args...);
};
