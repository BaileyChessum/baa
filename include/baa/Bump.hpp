#pragma once

#include <baa/BumpMark.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace baa {

template <typename T>
  requires std::is_trivially_destructible_v<T>
class BumpAllocator;

/// Owns a heap-allocated byte buffer and the cursor tracking the current allocation position.
/// BumpAllocator<T> holds a pointer to a Bump and performs typed allocations through it.
///
/// Bump is non-copyable. Move it if ownership must transfer.
class Bump {
public:
  /// Allocate a buffer of `capacity` bytes on the heap.
  explicit Bump(std::size_t capacity)
      : buffer(std::make_unique<std::byte[]>(capacity))
      , cursor(buffer.get())
      , end(buffer.get() + capacity)
  {}

  ~Bump() = default;

  Bump(const Bump&)            = delete;
  Bump& operator=(const Bump&) = delete;

  Bump(Bump&& other) noexcept
      : buffer(std::move(other.buffer))
      , cursor(other.cursor)
      , end(other.end)
  {
    other.cursor = nullptr;
    other.end    = nullptr;
  }

  Bump& operator=(Bump&& other) noexcept {
    if (this != &other) {
      buffer       = std::move(other.buffer);
      cursor       = other.cursor;
      end          = other.end;
      other.cursor = nullptr;
      other.end    = nullptr;
    }
    return *this;
  }

  /// Save the current cursor position. Pass the returned mark to restore() to reclaim all
  /// allocations made after this point.
  [[nodiscard]] BumpMark mark() const noexcept { return {cursor}; }

  /// Restore the cursor to a previously saved mark. Returns false when the mark belongs to a
  /// @warning different backing buffer or points past the end of this Bump.
  [[nodiscard]] bool restore(BumpMark m) noexcept {
    if (m.cursor < buffer.get() || m.cursor > end) [[unlikely]]
      return false;

    restore_unsafe(m);
    return true;
  }

  /// Restore the cursor to a previously saved mark without validation.
  /// The caller must ensure that the mark came from this backing buffer.
  void restore_unsafe(BumpMark m) noexcept {
    cursor = m.cursor;
  }

  /// Reset the cursor to the beginning of the buffer. Invalidates all outstanding pointers.
  void reset() noexcept { cursor = buffer.get(); }

  /// Total buffer size in bytes.
  [[nodiscard]] std::size_t capacity() const noexcept {
    return static_cast<std::size_t>(end - buffer.get());
  }

  /// Bytes consumed by allocations so far (including any alignment padding).
  [[nodiscard]] std::size_t used() const noexcept {
    return static_cast<std::size_t>(cursor - buffer.get());
  }

  /// Bytes remaining before the buffer is exhausted. Does not account for alignment padding
  /// that may be consumed by the next allocation.
  [[nodiscard]] std::size_t remaining() const noexcept {
    return static_cast<std::size_t>(end - cursor);
  }

private:
  /// Align the cursor up to `alignment`, then advance it by `size` bytes.
  /// Returns a pointer to the aligned region, or nullptr if the buffer is exhausted.
  [[nodiscard]] std::byte* allocate(std::size_t size, std::size_t alignment) noexcept {
    if (cursor == nullptr || alignment == 0 || (alignment & (alignment - 1)) != 0) [[unlikely]]
      return nullptr;

    auto addr    = reinterpret_cast<std::uintptr_t>(cursor);
    auto misalignment = addr & (alignment - 1);
    auto padding      = misalignment == 0 ? 0 : alignment - misalignment;

    auto available = static_cast<std::size_t>(end - cursor);
    if (padding > available || size > available - padding) [[unlikely]]
      return nullptr;

    auto aligned = cursor + static_cast<std::ptrdiff_t>(padding);
    cursor = aligned + size;
    return aligned;
  }

  template <typename T>
    requires std::is_trivially_destructible_v<T>
  friend class BumpAllocator;

  std::unique_ptr<std::byte[]> buffer;
  std::byte*                   cursor;
  std::byte*                   end;
};

} // namespace baa
