#pragma once

#include <baa/Bump.hpp>

#include <cstddef>
#include <new>
#include <type_traits>

namespace baa {

/**
 * @brief Typed allocator adapter over a `Bump`.
 *
 * Satisfies the C++ named Allocator requirement.
 *
 * @tparam T Allocated element type. Must be trivially destructible because `Bump`
 * does not track or invoke destructors.
 *
 * @warning This is a non-owning handle. The referenced `Bump` must outlive it.
 * @note `deallocate()` is a no-op; memory is reclaimed only via `Bump::reset()`
 * or `Bump::restore()`.
 */
template <typename T>
  requires std::is_trivially_destructible_v<T>
class BumpAllocator {
public:
  using value_type = T;

  explicit BumpAllocator(Bump& bump) noexcept : bump(&bump) {}

  template <typename U>
  explicit BumpAllocator(const BumpAllocator<U>& other) noexcept : bump(other.bump) {}

  /**
   * @brief Allocate space for `n` objects of type `T`.
   * @param n Number of objects to allocate storage for.
   * @return Pointer to suitably aligned storage for `n` objects.
   * @throws std::bad_alloc The request overflows or the arena cannot satisfy the allocation.
   */
  [[nodiscard]] T* allocate(const std::size_t n) {
    // Reject counts that would overflow the byte-size multiplication below.
    if (n > static_cast<std::size_t>(-1) / sizeof(T)) [[unlikely]]
      throw std::bad_alloc{};

    std::byte* raw = bump->template allocate<alignof(T)>(sizeof(T) * n);
    if (!raw) [[unlikely]]
      throw std::bad_alloc{};

    return reinterpret_cast<T*>(raw);
  }

  /**
   * @brief No-op deallocation hook for allocator-aware code.
   * @warning Memory is reclaimed only in bulk via `Bump::reset()` or `Bump::restore()`.
   */
  void deallocate(T*, std::size_t) noexcept {}

  /// @return `true` when both allocators reference the same backing `Bump`.
  template <typename U>
  bool operator==(const BumpAllocator<U>& other) const noexcept {
    return bump == other.bump;
  }

private:
  Bump* bump;

  template <typename U>
    requires std::is_trivially_destructible_v<U>
  friend class BumpAllocator;
};

} // namespace baa
