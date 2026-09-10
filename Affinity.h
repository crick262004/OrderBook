#pragma once

#include <cstdint>
#include <expected>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

// CPU affinity for the spinning threads. A busy-waiting thread wants its own
// core: migration empties the L1/L2 it warmed, two spinners on one core only
// progress when a time slice expires (milliseconds, not nanoseconds), and on a
// chip with several core clusters the hop latency depends on which cluster the
// scheduler happened to pick. Pinning turns the spin assumption into a fact.
//
// Linux honours a hard pin (pthread_setaffinity_np). macOS has no hard affinity
// at all — THREAD_AFFINITY_POLICY is a co-location hint that Apple Silicon
// rejects outright — so every pin reports Unsupported there, honestly, and the
// only lever is the QoS class that steers a thread toward performance cores.

enum class AffinityError : std::uint8_t
{
    Unsupported, // the OS offers no hard affinity
    InvalidCore, // core index beyond what the OS can address
    Failed,      // the OS refused (offline core, cpuset restriction, permissions)
};

// From now on the scheduler may run `thread` on `core` and nowhere else.
[[nodiscard]] std::expected<void, AffinityError> PinToCore(std::thread &thread, unsigned core) noexcept;
[[nodiscard]] std::expected<void, AffinityError> PinCurrentThread(unsigned core) noexcept;

// Pins the calling thread for a scope and restores its previous mask after — a
// benchmark's main thread must not stay pinned into the next benchmark.
class ScopedPin
{
public:
    explicit ScopedPin(unsigned core) noexcept;
    ~ScopedPin();

    ScopedPin(const ScopedPin &) = delete;
    ScopedPin &operator=(const ScopedPin &) = delete;
    ScopedPin(ScopedPin &&) = delete;
    ScopedPin &operator=(ScopedPin &&) = delete;

    [[nodiscard]] std::expected<void, AffinityError> Result() const noexcept { return result_; }

private:
    std::expected<void, AffinityError> result_;
#if defined(__linux__)
    cpu_set_t saved_{};
    bool restore_{false};
#endif
};

// Asks the OS to schedule the calling thread on its fastest cores where that is
// a distinct request (macOS QoS); a no-op where affinity already decides.
void PreferPerformanceCores() noexcept;

// Deliberately no spin-loop hint (x86 `pause`, arm64 `isb`/`yield`): such hints
// trade latency for power. Measured here, an `isb` per poll added ~40 ns to a
// ping-pong round trip on Apple Silicon — the waiter notices the new value one
// pipeline flush later per hop. A pinned core owes nothing to anyone else, so
// the spin loops stay tight (ADR 018).
