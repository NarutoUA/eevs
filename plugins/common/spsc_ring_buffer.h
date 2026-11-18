#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace eevs::plugins::common {

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) bounded ring buffer.
 *
 * @tparam T        Element type.
 * @tparam Capacity Buffer size (power of two >= 2). Usable capacity is `Capacity - 1`.
 */
template <typename T, size_t Capacity>
    requires (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0)
class SpscRingBuffer {
public:
    constexpr SpscRingBuffer() = default;
    ~SpscRingBuffer() = default;

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    template <typename U>
        requires std::is_convertible_v<U&&, T>
    [[nodiscard]] bool TryPush(U&& item) noexcept(std::is_nothrow_assignable_v<T&, U&&>) {
        const size_t writePos = m_writeIndex.load(std::memory_order_relaxed);
        const size_t nextWritePos = (writePos + 1) & kMask;

        if (nextWritePos == m_readIndex.load(std::memory_order_acquire)) {
            return false;
        }

        m_storage[writePos] = std::forward<U>(item);
        m_writeIndex.store(nextWritePos, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool TryPop(T& outItem) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const size_t readPos = m_readIndex.load(std::memory_order_relaxed);
        if (readPos == m_writeIndex.load(std::memory_order_acquire)) {
            return false;
        }

        outItem = std::move(m_storage[readPos]);
        m_readIndex.store((readPos + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>) {
        T item{};
        if (!TryPop(item)) {
            return std::nullopt;
        }
        return item;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return m_readIndex.load(std::memory_order_acquire) ==
               m_writeIndex.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t SizeApprox() const noexcept {
        const size_t writePos = m_writeIndex.load(std::memory_order_acquire);
        const size_t readPos = m_readIndex.load(std::memory_order_acquire);
        return (writePos - readPos) & kMask;
    }

    [[nodiscard]] static constexpr size_t MaxCapacity() noexcept {
        return Capacity - 1;
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    alignas(64) std::atomic<size_t> m_writeIndex{0};
    alignas(64) std::atomic<size_t> m_readIndex{0};
    std::array<T, Capacity> m_storage{};
};

} // namespace eevs::plugins::common
