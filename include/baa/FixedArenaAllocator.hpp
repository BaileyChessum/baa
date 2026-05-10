#pragma once

#include <baa/FixedArena.hpp>

#include <cstddef>
#include <new>

namespace baa {

/**
 * @brief Typed allocator adapter over a `FixedArena`.
 *
 * Satisfies the C++ named Allocator requirement.
 *
 * `FixedArenaAllocator<T>` allocates raw storage only. Element destruction remains the
 * caller's responsibility, as with ordinary allocator-aware code.
 *
 * @tparam T Allocated element type.
 *
 * @warning This is a non-owning handle. The referenced `FixedArena` must outlive it.
 * @warning `FixedArena::reset()` and `restore()` invalidate storage without calling
 * destructors for allocator-managed objects.
 */
template <typename T>
class FixedArenaAllocator {
public:
  using value_type = T;

  explicit FixedArenaAllocator(FixedArena& arena) noexcept : arena(&arena) {}

  template <typename U>
  explicit FixedArenaAllocator(const FixedArenaAllocator<U>& other) noexcept : arena(other.arena) {}

  /**
   * @brief Allocate space for `n` objects of type `T`.
   * @param n Number of objects to allocate storage for.
   * @return Pointer to suitably aligned storage for `n` objects.
   * @throws std::bad_alloc The request overflows or the arena cannot satisfy the allocation.
   */
  [[nodiscard]] T* allocate(const std::size_t n) {
    if (n > static_cast<std::size_t>(-1) / sizeof(T)) [[unlikely]]
      throw std::bad_alloc{};

    std::byte* raw = arena->template allocate<alignof(T)>(sizeof(T) * n);
    if (!raw) [[unlikely]]
      throw std::bad_alloc{};

    return reinterpret_cast<T*>(raw);
  }

  /**
   * @brief No-op deallocation hook for allocator-aware code.
   * @warning Memory is reclaimed only via `FixedArena::reset()` or `FixedArena::restore()`.
   */
  void deallocate(T*, std::size_t) noexcept {}

  /// @return `true` when both allocators reference the same backing `FixedArena`.
  template <typename U>
  bool operator==(const FixedArenaAllocator<U>& other) const noexcept {
    return arena == other.arena;
  }

private:
  FixedArena* arena;

  template <typename U>
  friend class FixedArenaAllocator;
};

} // namespace baa
