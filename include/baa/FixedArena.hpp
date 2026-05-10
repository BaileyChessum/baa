#pragma once

#include <baa/FixedArenaCheckpoint.hpp>
#include <baa/FixedArenaMarker.hpp>

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
class FixedArenaAllocator;

/**
 * @brief Fixed-capacity arena with optional destructor tracking for direct object construction.
 *
 * `FixedArena` keeps the simple single-buffer allocation model, but unlike `Bump`, it can own
 * and destroy non-trivial objects created via `emplace()` and `emplace_array()`.
 *
 * `FixedArenaAllocator<T>` allocates raw storage only. Objects allocated through the allocator
 * are not tracked for destruction by the arena itself.
 *
 * @warning `FixedArena` is non-copyable. Move it if ownership must transfer.
 */
class FixedArena {
public:
  /// Allocate a heap buffer of `capacity` bytes.
  explicit FixedArena(const std::size_t capacity)
      : buffer(std::make_unique<std::byte[]>(capacity)), cursor(buffer.get()), end(buffer.get() + capacity) {}

  ~FixedArena() { reset(); }

  FixedArena(const FixedArena&) = delete;
  FixedArena& operator=(const FixedArena&) = delete;

  FixedArena(FixedArena&& other) noexcept
      : buffer(std::move(other.buffer)), cursor(other.cursor), end(other.end), destructor_head(other.destructor_head) {
    other.cursor = nullptr;
    other.end = nullptr;
    other.destructor_head = nullptr;
  }

  FixedArena& operator=(FixedArena&& other) noexcept {
    if (this != &other) {
      reset();

      buffer = std::move(other.buffer);
      cursor = other.cursor;
      end = other.end;
      destructor_head = other.destructor_head;

      other.cursor = nullptr;
      other.end = nullptr;
      other.destructor_head = nullptr;
    }
    return *this;
  }

  /**
   * @brief Construct a `T` in the arena and return a reference to it.
   * @tparam T Object type to construct.
   * @tparam Args Constructor argument types.
   * @param args Arguments forwarded to `T`'s constructor.
   * @return Reference to the constructed object.
   * @throws std::bad_alloc The arena cannot satisfy the allocation.
   *
   * Non-trivial objects created this way are owned by the arena and are destroyed by
   * `restore()`, `restore_unsafe()`, `reset()`, and the arena destructor as needed.
   */
  template <typename T, typename... Args>
    requires std::constructible_from<T, Args...>
  [[nodiscard]] T& emplace(Args&&... args) {
    const FixedArenaMarker saved = mark();
    ReservedStorage storage = try_carve<alignof(T), !std::is_trivially_destructible_v<T>>(sizeof(T));
    if (!storage.object) [[unlikely]]
      throw std::bad_alloc{};

    try {
      T* object = ::new (storage.object) T(std::forward<Args>(args)...);
      if constexpr (!std::is_trivially_destructible_v<T>)
        link_node(storage.node, object, 1, &destroy_range<T>);
      return *object;
    }
    catch (...) {
      restore_cursor_only(saved);
      throw;
    }
  }

  /**
   * @brief Default-construct `count` objects of type `T` in the arena.
   * @tparam T Element type. Must be default constructible.
   * @param count Number of objects to construct.
   * @return A span over the constructed objects, or an empty span when `count == 0`.
   * @throws std::bad_alloc The request overflows or the arena cannot satisfy the allocation.
   *
   * Non-trivial objects created this way are owned by the arena and are destroyed by
   * `restore()`, `restore_unsafe()`, `reset()`, and the arena destructor as needed.
   */
  template <typename T>
    requires std::is_default_constructible_v<T>
  [[nodiscard]] std::span<T> emplace_array(const std::size_t count) {
    if (count == 0)
      return {};

    if (count > static_cast<std::size_t>(-1) / sizeof(T)) [[unlikely]]
      throw std::bad_alloc{};

    const FixedArenaMarker saved = mark();
    ReservedStorage storage = try_carve<alignof(T), !std::is_trivially_destructible_v<T>>(sizeof(T) * count);
    if (!storage.object) [[unlikely]]
      throw std::bad_alloc{};

    T* objects = reinterpret_cast<T*>(storage.object);
    std::size_t constructed = 0;
    try {
      for (; constructed < count; ++constructed)
        ::new (objects + constructed) T();
    }
    catch (...) {
      destroy_constructed_prefix(objects, constructed);
      restore_cursor_only(saved);
      throw;
    }

    if constexpr (!std::is_trivially_destructible_v<T>)
      link_node(storage.node, objects, count, &destroy_range<T>);

    return std::span<T>(objects, count);
  }

  /**
   * @brief Save the current cursor and owned-destructor state.
   * @note This is the low-level explicit checkpoint primitive. Prefer `checkpoint()` for
   * scope-bounded temporary allocation that should roll back automatically unless released.
   * @return A mark that can later be passed to `restore()` or `restore_unsafe()`.
   */
  [[nodiscard]] FixedArenaMarker mark() const noexcept {
    return {cursor, reinterpret_cast<std::byte*>(destructor_head)};
  }

  /**
   * @brief Create a scope guard that restores this arena unless released.
   * @return Active checkpoint guard capturing the current cursor and destructor state.
   *
   * `rollback()` restores to the saved point immediately and may destroy owned non-trivial
   * objects created since the checkpoint was taken. `release()` keeps current allocations.
   */
  [[nodiscard]] FixedArenaCheckpoint checkpoint() noexcept { return FixedArenaCheckpoint(this, mark()); }

  /**
   * @brief Restore the arena to a previously saved mark.
   * @param m Mark to restore.
   * @return `true` when the mark is accepted; `false` when it does not describe the current arena state.
   *
   * Accepted marks must point into this arena's current buffer, must not advance the cursor,
   * and must refer to the current destruction stack or one of its ancestors.
   *
   * This is the low-level explicit checkpoint primitive. Prefer `checkpoint()` unless you need
   * manual mark management.
   */
  [[nodiscard]] bool restore(const FixedArenaMarker m) noexcept {
    if (!is_valid_mark(m)) [[unlikely]]
      return false;

    restore_unsafe(m);
    return true;
  }

  /**
   * @brief Restore the arena to a previously saved mark without validation.
   * @param m Mark to restore.
   * @warning The caller must ensure that `m` is an ancestor mark for this arena's current state.
   * @note This is intended for internal or otherwise guaranteed-valid marks such as those held by
   * `FixedArenaCheckpoint`.
   */
  void restore_unsafe(const FixedArenaMarker m) noexcept {
    destroy_tracked_until(reinterpret_cast<DestructorNode*>(m.destructor_head));
    cursor = m.cursor;
    destructor_head = reinterpret_cast<DestructorNode*>(m.destructor_head);
  }

  /**
   * @brief Reset the arena to the beginning of its buffer.
   * @warning Destroys all arena-owned non-trivial objects and invalidates outstanding storage,
   * including storage kept alive through `FixedArenaAllocator<T>`.
   */
  void reset() noexcept {
    destroy_tracked_until(nullptr);
    cursor = buffer.get();
    destructor_head = nullptr;
  }

  /// Total buffer size in bytes.
  [[nodiscard]] std::size_t capacity() const noexcept { return static_cast<std::size_t>(end - buffer.get()); }

  /// Bytes consumed so far, including alignment padding and in-buffer destructor metadata.
  [[nodiscard]] std::size_t used() const noexcept { return static_cast<std::size_t>(cursor - buffer.get()); }

  /**
   * Bytes remaining before the buffer is exhausted.
   * @warning This does not account for alignment padding or destructor metadata that may be needed next.
   */
  [[nodiscard]] std::size_t remaining() const noexcept { return static_cast<std::size_t>(end - cursor); }

private:
  struct DestructorNode {
    DestructorNode* previous;
    void* object;
    std::size_t count;
    void (*destroy)(void*, std::size_t) noexcept;
  };

  struct ReservedStorage {
    std::byte* object = nullptr;
    DestructorNode* node = nullptr;
  };

  template <std::size_t Alignment, bool NeedsNode>
  [[nodiscard]] ReservedStorage try_carve(const std::size_t size) noexcept {
    static_assert(Alignment != 0);
    static_assert((Alignment & (Alignment - 1)) == 0);

    if (cursor == nullptr) [[unlikely]]
      return {};

    auto* object = reinterpret_cast<std::byte*>(
        (reinterpret_cast<std::uintptr_t>(cursor) + (Alignment - 1)) & ~std::uintptr_t{Alignment - 1});
    std::byte* next = object + size;
    if (next > end) [[unlikely]]
      return {};

    if constexpr (!NeedsNode) {
      cursor = next;
      return {object, nullptr};
    }

    auto* node = reinterpret_cast<DestructorNode*>(
        (reinterpret_cast<std::uintptr_t>(next) + (alignof(DestructorNode) - 1)) &
        ~std::uintptr_t{alignof(DestructorNode) - 1});
    auto* new_cursor = reinterpret_cast<std::byte*>(node) + sizeof(DestructorNode);
    if (new_cursor > end) [[unlikely]]
      return {};

    cursor = new_cursor;
    return {object, node};
  }

  template <std::size_t Alignment>
  [[nodiscard]] std::byte* allocate(const std::size_t size) noexcept {
    return try_carve<Alignment, false>(size).object;
  }

  void restore_cursor_only(const FixedArenaMarker m) noexcept { cursor = m.cursor; }

  [[nodiscard]] bool is_valid_mark(const FixedArenaMarker m) const noexcept {
    const auto begin_addr = reinterpret_cast<std::uintptr_t>(buffer.get());
    const auto current_addr = reinterpret_cast<std::uintptr_t>(cursor);

    if (const auto mark_addr = reinterpret_cast<std::uintptr_t>(m.cursor);
        mark_addr < begin_addr || mark_addr > current_addr) [[unlikely]]
      return false;

    auto* target = reinterpret_cast<DestructorNode*>(m.destructor_head);
    if (target == nullptr)
      return true;

    if (const auto target_addr = reinterpret_cast<std::uintptr_t>(target);
        target_addr < begin_addr || target_addr >= current_addr) [[unlikely]]
      return false;

    for (const DestructorNode* node = destructor_head; node != nullptr; node = node->previous) {
      if (node == target)
        return true;
    }

    return false;
  }

  void destroy_tracked_until(DestructorNode* target) noexcept {
    while (destructor_head != target) {
      DestructorNode* node = destructor_head;
      node->destroy(node->object, node->count);
      destructor_head = node->previous;
    }
  }

  void link_node(DestructorNode* node, void* object, const std::size_t count,
                 void (*destroy)(void*, std::size_t) noexcept) noexcept {
    ::new (node) DestructorNode{destructor_head, object, count, destroy};
    destructor_head = node;
  }

  template <typename T>
  static void destroy_range(void* object, const std::size_t count) noexcept {
    T* typed = static_cast<T*>(object);
    for (std::size_t i = count; i > 0; --i)
      typed[i - 1].~T();
  }

  template <typename T>
  static void destroy_constructed_prefix(T* objects, const std::size_t count) noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      for (std::size_t i = count; i > 0; --i)
        objects[i - 1].~T();
    }
  }

  template <typename T>
  friend class FixedArenaAllocator;

  std::unique_ptr<std::byte[]> buffer;
  std::byte* cursor;
  std::byte* end;
  DestructorNode* destructor_head = nullptr;
};

inline FixedArenaCheckpoint::~FixedArenaCheckpoint() noexcept { rollback(); }

inline void FixedArenaCheckpoint::rollback() noexcept {
  if (!active())
    return;

  arena->restore_unsafe(mark);
  release();
}

} // namespace baa
