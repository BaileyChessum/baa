#pragma once

#include <baa/Bump.hpp>

#include <cstddef>
#include <new>
#include <type_traits>

namespace baa {

/// Typed allocator over a Bump. Satisfies the C++ named Allocator requirement.
/// T must be trivially destructible — Bump does not track or invoke destructors.
///
/// BumpAllocator is a non-owning handle; the Bump it references must outlive it.
/// deallocate() is a no-op: memory is reclaimed only via Bump::reset() or Bump::restore().
template <typename T>
  requires std::is_trivially_destructible_v<T>
class BumpAllocator {
public:
  using value_type = T;

  explicit BumpAllocator(Bump& bump) noexcept : bump(&bump) {}

  template <typename U>
  BumpAllocator(const BumpAllocator<U>& other) noexcept : bump(other.bump) {}

  /// Allocate space for n objects of type T. Throws std::bad_alloc if the Bump is exhausted.
  [[nodiscard]] T* allocate(std::size_t n) {
    if (n > static_cast<std::size_t>(-1) / sizeof(T)) [[unlikely]]
      throw std::bad_alloc{};

    std::byte* raw = bump->allocate(sizeof(T) * n, alignof(T));
    if (!raw) [[unlikely]]
      throw std::bad_alloc{};
    return reinterpret_cast<T*>(raw);
  }

  /// No-op. Memory is reclaimed in bulk via Bump::reset() or Bump::restore().
  void deallocate(T*, std::size_t) noexcept {}

  /// Two BumpAllocators are equal when they share the same backing Bump.
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
