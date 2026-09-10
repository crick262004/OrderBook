#include "Affinity.h"

#if defined(__linux__)

#include <pthread.h>

namespace
{

std::expected<void, AffinityError> PinHandle(pthread_t handle, unsigned core) noexcept
{
    if (core >= CPU_SETSIZE)
    {
        return std::unexpected{AffinityError::InvalidCore};
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(handle, sizeof(set), &set) != 0)
    {
        return std::unexpected{AffinityError::Failed};
    }

    return {};
}

} // namespace

std::expected<void, AffinityError> PinToCore(std::thread &thread, unsigned core) noexcept
{
    return PinHandle(thread.native_handle(), core);
}

std::expected<void, AffinityError> PinCurrentThread(unsigned core) noexcept
{
    return PinHandle(pthread_self(), core);
}

ScopedPin::ScopedPin(unsigned core) noexcept
    : restore_{pthread_getaffinity_np(pthread_self(), sizeof(saved_), &saved_) == 0}
{
    result_ = PinCurrentThread(core);
}

ScopedPin::~ScopedPin()
{
    if (restore_)
    {
        pthread_setaffinity_np(pthread_self(), sizeof(saved_), &saved_);
    }
}

void PreferPerformanceCores() noexcept
{
    // Hard affinity is the whole answer on Linux; nothing further to ask for.
}

#elif defined(__APPLE__)

#include <pthread/qos.h>

std::expected<void, AffinityError> PinToCore(std::thread &, unsigned) noexcept
{
    return std::unexpected{AffinityError::Unsupported};
}

std::expected<void, AffinityError> PinCurrentThread(unsigned) noexcept
{
    return std::unexpected{AffinityError::Unsupported};
}

ScopedPin::ScopedPin(unsigned core) noexcept : result_{PinCurrentThread(core)} {}

ScopedPin::~ScopedPin() = default;

void PreferPerformanceCores() noexcept
{
    // The one scheduling lever macOS exposes: USER_INTERACTIVE steers the thread
    // to performance cores and away from the efficiency cluster.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

#else

std::expected<void, AffinityError> PinToCore(std::thread &, unsigned) noexcept
{
    return std::unexpected{AffinityError::Unsupported};
}

std::expected<void, AffinityError> PinCurrentThread(unsigned) noexcept
{
    return std::unexpected{AffinityError::Unsupported};
}

ScopedPin::ScopedPin(unsigned core) noexcept : result_{PinCurrentThread(core)} {}

ScopedPin::~ScopedPin() = default;

void PreferPerformanceCores() noexcept {}

#endif
