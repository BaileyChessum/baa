#pragma once

#include <baa/BumpCheckpoint.hpp>
#include <baa/BumpMark.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace baa {

template <typename T>
  requires std::is_trivially_destructible_v<T>
class BumpAllocator;

/**
 * @brief Heap-backed bump storage with mark/restore semantics.
 *
 * `BumpAllocator<T>` performs typed allocations through a `Bump`.
 *
 * @warning `Bump` is non-copyable. Move it if ownership must transfer.
 */
class Bump {
public:
  /// Allocate a heap buffer of `capacity` bytes.
  explicit Bump(const std::size_t capacity)
      : buffer(std::make_unique<std::byte[]>(capacity)), cursor(buffer.get()), end(buffer.get() + capacity) {}

  ~Bump() = default;

  Bump(const Bump&) = delete;
  Bump& operator=(const Bump&) = delete;

  Bump(Bump&& other) noexcept : buffer(std::move(other.buffer)), cursor(other.cursor), end(other.end) {
    other.cursor = nullptr;
    other.end = nullptr;
  }

  Bump& operator=(Bump&& other) noexcept {
    if (this != &other)
    {
      buffer = std::move(other.buffer);
      cursor = other.cursor;
      end = other.end;
      other.cursor = nullptr;
      other.end = nullptr;
    }
    return *this;
  }

  /**
   * @brief Construct a `T` in the arena and return a reference to it.
   * @tparam T Object type to construct. Must be trivially destructible.
   * @tparam Args Constructor argument types.
   * @param args Arguments forwarded to `T`'s constructor.
   * @return Reference to the constructed object.
   * @throws std::bad_alloc The arena cannot satisfy the allocation.
   *
   * If construction throws, the arena cursor is rolled back to its prior position.
   */
  template <typename T, typename... Args>
    requires std::is_trivially_destructible_v<T> && std::constructible_from<T, Args...>
  [[nodiscard]] T& emplace(Args&&... args) {
    const BumpMark saved = mark();
    std::byte* raw = allocate<alignof(T)>(sizeof(T));
    if (!raw) [[unlikely]]
      throw std::bad_alloc{};

    try
    { return *::new (raw) T(std::forward<Args>(args)...); }
    catch (...)
    {
      restore_unsafe(saved);
      throw;
    }
  }

  /**
   * @brief Default-construct `count` objects of type `T` in the arena.
   * @tparam T Element type. Must be trivially destructible and default constructible.
   * @param count Number of objects to construct.
   * @return A span over the constructed objects, or an empty span when `count == 0`.
   * @throws std::bad_alloc The request overflows or the arena cannot satisfy the allocation.
   *
   * If construction throws, the arena cursor is rolled back to its prior position.
   */
  template <typename T>
    requires std::is_trivially_destructible_v<T> && std::is_default_constructible_v<T>
  [[nodiscard]] std::span<T> emplace_array(std::size_t count) {
    if (count == 0)
      return {};

    const BumpMark saved = mark();
    // Spell this directly so the header does not need <limits>.
    if (count > static_cast<std::size_t>(-1) / sizeof(T)) [[unlikely]]
      throw std::bad_alloc{};

    std::byte* raw = allocate<alignof(T)>(sizeof(T) * count);
    if (!raw) [[unlikely]]
      throw std::bad_alloc{};

    T* ptr = reinterpret_cast<T*>(raw);
    try
    {
      for (std::size_t i = 0; i < count; ++i)
        ::new (ptr + i) T();
    }
    catch (...)
    {
      restore_unsafe(saved);
      throw;
    }

    return std::span<T>(ptr, count);
  }

  /**
   * @brief Save the current cursor position.
   * @note This is the low-level explicit checkpoint primitive. Prefer `checkpoint()` for
   * scope-bounded temporary allocation that should roll back automatically unless released.
   * @return A mark that can later be passed to `restore()` or `restore_unsafe()`.
   */
  [[nodiscard]] BumpMark mark() const noexcept { return {cursor}; }

  /**
   * @brief Create a scope guard that restores this arena unless released.
   * @return Active checkpoint guard capturing the current cursor position.
   *
   * `rollback()` restores to the saved point immediately. `release()` keeps the current
   * allocations and disables rollback.
   */
  [[nodiscard]] BumpCheckpoint checkpoint() noexcept {
    return BumpCheckpoint(this, mark(), [](Bump* bump, const BumpMark saved) noexcept { bump->restore_unsafe(saved); });
  }

  /**
   * @brief Restore the cursor to a previously saved mark.
   * @param m Mark to restore.
   * @return `true` when the mark is accepted; `false` when it lies outside this arena's buffer.
   * @note This is the low-level explicit checkpoint primitive. Prefer `checkpoint()` unless you
   * need manual mark management.
   */
  [[nodiscard]] bool restore(const BumpMark m) noexcept {
    // Compare integer addresses directly so the header does not need <functional> for std::less<>.
    const auto begin_addr = reinterpret_cast<std::uintptr_t>(buffer.get());
    const auto mark_addr = reinterpret_cast<std::uintptr_t>(m.cursor);
    const auto end_addr = reinterpret_cast<std::uintptr_t>(end);

    if (mark_addr < begin_addr || mark_addr > end_addr) [[unlikely]]
      return false;

    restore_unsafe(m);
    return true;
  }

  /**
   * @brief Restore the cursor to a previously saved mark without validation.
   * @param m Mark to restore.
   * @warning The caller must ensure that `m` came from this arena's backing buffer.
   * @note This is intended for internal or otherwise guaranteed-valid marks such as those held by
   * `BumpCheckpoint`.
   */
  void restore_unsafe(const BumpMark m) noexcept { cursor = m.cursor; }

  /**
   * @brief Reset the cursor to the beginning of the buffer.
   * @warning Invalidates all outstanding pointers and references into the arena, including
   * storage kept alive through `BumpAllocator<T>`.
   */
  void reset() noexcept { cursor = buffer.get(); }

  /// Total buffer size in bytes.
  [[nodiscard]] std::size_t capacity() const noexcept { return static_cast<std::size_t>(end - buffer.get()); }

  /// Bytes consumed so far, including alignment padding.
  [[nodiscard]] std::size_t used() const noexcept { return static_cast<std::size_t>(cursor - buffer.get()); }

  /**
   * Bytes remaining before the buffer is exhausted.
   * @warning This does not account for alignment padding that may be consumed by the next allocation.
   */
  [[nodiscard]] std::size_t remaining() const noexcept { return static_cast<std::size_t>(end - cursor); }

private:
  /**
   * @brief Align the cursor up to `Alignment`, then advance it by `size` bytes.
   * @tparam Alignment Required alignment. Must be a non-zero power of two.
   * @param size Bytes to reserve after alignment.
   * @return Pointer to the aligned region, or `nullptr` when the request cannot be satisfied.
   */
  template <std::size_t Alignment>
  [[nodiscard]] std::byte* allocate(const std::size_t size) noexcept {
    static_assert(Alignment != 0);
    static_assert((Alignment & (Alignment - 1)) == 0);

    if (cursor == nullptr) [[unlikely]]
      return nullptr;

    const auto addr = reinterpret_cast<std::uintptr_t>(cursor);
    const auto misalignment = addr & (Alignment - 1);
    const auto padding = misalignment == 0 ? 0 : Alignment - misalignment;

    if (const auto available = static_cast<std::size_t>(end - cursor);
        padding > available || size > available - padding) [[unlikely]]
      return nullptr;

    const auto aligned = cursor + static_cast<std::ptrdiff_t>(padding);
    cursor = aligned + size;
    return aligned;
  }

  template <typename T>
    requires std::is_trivially_destructible_v<T>
  friend class BumpAllocator;

  std::unique_ptr<std::byte[]> buffer;
  std::byte* cursor;
  std::byte* end;
};

} // namespace baa
