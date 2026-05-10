#pragma once

#include <baa/ArenaCheckpoint.hpp>
#include <baa/ArenaMarker.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace baa {

template <typename T>
class ArenaAllocator;

namespace detail {

struct ArenaPage {
  struct DestructorNode {
    DestructorNode* previous;
    void* object;
    std::size_t count;
    void (*destroy)(void*, std::size_t) noexcept;
  };

  static constexpr std::size_t kAlignment = alignof(std::max_align_t);

  [[nodiscard]] std::byte* begin() const noexcept;

  ArenaPage* previous = nullptr;
};

inline std::byte* ArenaPage::begin() const noexcept {
  constexpr std::size_t header_bytes = (sizeof(ArenaPage) + kAlignment - 1) & ~(kAlignment - 1);
  return reinterpret_cast<std::byte*>(const_cast<ArenaPage*>(this)) + header_bytes;
}

} // namespace detail

/**
 * @brief Growable arena with page-chained allocation and optional destructor tracking.
 *
 * `Arena` mirrors `FixedArena` where practical, but grows by allocating additional pages when
 * the current page cannot satisfy a request.
 *
 * `ArenaAllocator<T>` allocates raw storage only. Objects allocated through the allocator
 * are not tracked for destruction by the arena itself.
 *
 * @warning `Arena` is non-copyable. Move it if ownership must transfer.
 */
class Arena {
public:
  /// Allocate the first page with `initial_capacity` usable bytes.
  explicit Arena(const std::size_t initial_capacity) {
    current_page = allocate_page(initial_capacity, nullptr);
    cursor = current_page->begin();
    root_end = end = cursor + static_cast<std::ptrdiff_t>(initial_capacity);
  }

  ~Arena() { release_all_pages(); }

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  Arena(Arena&& other) noexcept
      : current_page(other.current_page), cursor(other.cursor), end(other.end), destructor_head(other.destructor_head),
        root_end(other.root_end) {
    other.current_page = nullptr;
    other.cursor = nullptr;
    other.end = nullptr;
    other.destructor_head = nullptr;
    other.root_end = nullptr;
  }

  Arena& operator=(Arena&& other) noexcept {
    if (this != &other) {
      release_all_pages();

      current_page = other.current_page;
      cursor = other.cursor;
      end = other.end;
      destructor_head = other.destructor_head;
      root_end = other.root_end;

      other.current_page = nullptr;
      other.cursor = nullptr;
      other.end = nullptr;
      other.destructor_head = nullptr;
      other.root_end = nullptr;
    }
    return *this;
  }

  /**
   * @brief Construct a `T` in the arena and return a reference to it.
   * @tparam T Object type to construct.
   * @tparam Args Constructor argument types.
   * @param args Arguments forwarded to `T`'s constructor.
   * @return Reference to the constructed object.
   * @throws std::bad_alloc The request overflows or the arena cannot satisfy the allocation.
   *
   * Non-trivial objects created this way are owned by the arena and are destroyed by
   * `restore()`, `restore_unsafe()`, `reset()`, and the arena destructor as needed.
   */
  template <typename T, typename... Args>
    requires std::constructible_from<T, Args...>
  [[nodiscard]] T& emplace(Args&&... args) {
    const ArenaMarker saved = mark();

    ReservedStorage storage = reserve_storage<alignof(T), !std::is_trivially_destructible_v<T>>(sizeof(T));

    try {
      T* object = ::new (storage.object) T(std::forward<Args>(args)...);
      if constexpr (!std::is_trivially_destructible_v<T>)
        link_node(storage.node, object, 1, &destroy_range<T>);
      return *object;
    }
    catch (...) {
      restore_unsafe(saved);
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

    const ArenaMarker saved = mark();
    ReservedStorage storage = reserve_storage<alignof(T), !std::is_trivially_destructible_v<T>>(sizeof(T) * count);
    T* objects = reinterpret_cast<T*>(storage.object);

    std::size_t constructed = 0;
    try {
      for (; constructed < count; ++constructed)
        ::new (objects + constructed) T();
    }
    catch (...) {
      destroy_constructed_prefix(objects, constructed);
      restore_unsafe(saved);
      throw;
    }

    if constexpr (!std::is_trivially_destructible_v<T>)
      link_node(storage.node, objects, count, &destroy_range<T>);

    return std::span<T>(objects, count);
  }

  /**
   * @brief Save the current page, cursor, and owned-destructor state.
   * @note This is the low-level explicit checkpoint primitive. Prefer `checkpoint()` for
   * scope-bounded temporary allocation that should roll back automatically unless released.
   * @return A marker that can later be passed to `restore()` or `restore_unsafe()`.
   */
  [[nodiscard]] ArenaMarker mark() const noexcept {
    if (current_page == nullptr)
      return {};

    return {current_page, cursor, end, static_cast<void*>(destructor_head)};
  }

  /**
   * @brief Create a scope guard that restores this arena unless released.
   * @return Active checkpoint guard capturing the current page, cursor, and destructor state.
   *
   * `rollback()` restores to the saved point immediately and may destroy owned non-trivial
   * objects created since the checkpoint was taken. `release()` keeps current allocations.
   */
  [[nodiscard]] ArenaCheckpoint checkpoint() noexcept { return ArenaCheckpoint(this, mark()); }

  /**
   * @brief Restore the arena to a previously saved marker.
   * @param marker Marker to restore.
   * @return `true` when the marker is accepted; `false` when it does not describe the current arena state.
   *
   * Accepted markers must refer to a live page in the current chain, keep the cursor within
   * that page's currently valid range, and refer to the current destruction stack or one of its
   * ancestors.
   *
   * This is the low-level explicit checkpoint primitive. Prefer `checkpoint()` unless you need
   * manual marker management.
   */
  [[nodiscard]] bool restore(const ArenaMarker marker) noexcept {
    if (!is_valid_marker(marker)) [[unlikely]]
      return false;

    restore_unsafe(marker);
    return true;
  }

  /**
   * @brief Restore the arena to a previously saved marker without validation.
   * @param marker Marker to restore.
   * @warning The caller must ensure that `marker` is valid for this arena's current page chain.
   * @note This is intended for internal or otherwise guaranteed-valid markers such as those held by
   * `ArenaCheckpoint`.
   */
  void restore_unsafe(const ArenaMarker marker) noexcept {
    if (current_page == nullptr) [[unlikely]]
      return;

    auto* target_head = static_cast<detail::ArenaPage::DestructorNode*>(marker.destructor_head);
    destroy_until(target_head);

    while (current_page != marker.page) {
      detail::ArenaPage* doomed = current_page;
      current_page = doomed->previous;
      deallocate_page(doomed);
    }

    cursor = marker.cursor;
    end = marker.end;
    destructor_head = target_head;
  }

  /**
   * @brief Reset the arena to the beginning of its first page.
   * @warning Destroys all arena-owned non-trivial objects and invalidates outstanding storage,
   * including storage kept alive through `ArenaAllocator<T>`.
   */
  void reset() noexcept {
    if (current_page == nullptr) [[unlikely]]
      return;

    destroy_until(nullptr);

    while (current_page->previous != nullptr) {
      detail::ArenaPage* doomed = current_page;
      current_page = doomed->previous;
      deallocate_page(doomed);
    }

    cursor = current_page->begin();
    end = root_end;
    destructor_head = nullptr;
  }

  /// Bytes remaining in the current page before another page allocation would be needed.
  [[nodiscard]] std::size_t remaining() const noexcept {
    if (current_page == nullptr)
      return 0;

    return static_cast<std::size_t>(end - cursor);
  }

private:
  struct ReservedStorage {
    std::byte* object = nullptr;
    detail::ArenaPage::DestructorNode* node = nullptr;
  };

  static constexpr std::size_t kPageHeaderBytes =
      (sizeof(detail::ArenaPage) + detail::ArenaPage::kAlignment - 1) & ~(detail::ArenaPage::kAlignment - 1);

  static std::uintptr_t align_up(const std::uintptr_t value, const std::size_t alignment) noexcept {
    const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment - 1);
    return (value + mask) & ~mask;
  }

  static std::size_t doubled_capacity(const std::size_t value) noexcept {
    if (value == 0)
      return 1;
    if (value > (std::numeric_limits<std::size_t>::max)() / 2)
      return (std::numeric_limits<std::size_t>::max)();
    return value * 2;
  }

  [[nodiscard]] static detail::ArenaPage* allocate_page(const std::size_t usable_capacity,
                                                        detail::ArenaPage* previous) {
    if (usable_capacity > (std::numeric_limits<std::size_t>::max)() - kPageHeaderBytes)
      throw std::bad_alloc{};

    void* raw = ::operator new(kPageHeaderBytes + usable_capacity);
    auto* page = ::new (raw) detail::ArenaPage{};
    page->previous = previous;
    return page;
  }

  static void deallocate_page(detail::ArenaPage* page) noexcept {
    page->~ArenaPage();
    ::operator delete(page);
  }

  void release_all_pages() noexcept {
    destroy_until(nullptr);

    while (current_page != nullptr) {
      detail::ArenaPage* doomed = current_page;
      current_page = doomed->previous;
      deallocate_page(doomed);
    }

    cursor = nullptr;
    end = nullptr;
    destructor_head = nullptr;
    root_end = nullptr;
  }

  template <std::size_t Alignment, bool NeedsNode>
  [[nodiscard]] ReservedStorage try_carve(const std::size_t object_size) noexcept {
    static_assert(Alignment != 0);
    static_assert((Alignment & (Alignment - 1)) == 0);

    if (cursor == nullptr) [[unlikely]]
      return {};

    auto* object = reinterpret_cast<std::byte*>(align_up(reinterpret_cast<std::uintptr_t>(cursor), Alignment));
    std::byte* next = object + static_cast<std::ptrdiff_t>(object_size);
    if (next > end) [[unlikely]]
      return {};

    if constexpr (!NeedsNode) {
      cursor = next;
      return {object, nullptr};
    }

    auto* node = reinterpret_cast<detail::ArenaPage::DestructorNode*>(
        align_up(reinterpret_cast<std::uintptr_t>(next), alignof(detail::ArenaPage::DestructorNode)));
    auto* new_cursor = reinterpret_cast<std::byte*>(node) + sizeof(detail::ArenaPage::DestructorNode);
    if (new_cursor > end) [[unlikely]]
      return {};

    cursor = new_cursor;
    return {object, node};
  }

  template <std::size_t Alignment, bool NeedsNode>
  [[nodiscard]] ReservedStorage reserve_storage(const std::size_t object_size) {
    if (auto storage = try_carve<Alignment, NeedsNode>(object_size); storage.object != nullptr) [[likely]]
      return storage;

    const std::size_t minimum = minimum_page_capacity<Alignment, NeedsNode>(object_size);
    const std::size_t cap = current_page ? static_cast<std::size_t>(end - current_page->begin()) : 0;
    const std::size_t new_capacity = std::max(doubled_capacity(cap), minimum);

    detail::ArenaPage* new_page = allocate_page(new_capacity, current_page);
    current_page = new_page;
    cursor = new_page->begin();
    end = cursor + static_cast<std::ptrdiff_t>(new_capacity);

    // New page is sized to fit — carve unconditionally
    auto* object = reinterpret_cast<std::byte*>(align_up(reinterpret_cast<std::uintptr_t>(cursor), Alignment));
    std::byte* next = object + static_cast<std::ptrdiff_t>(object_size);

    if constexpr (!NeedsNode) {
      cursor = next;
      return {object, nullptr};
    }

    auto* node = reinterpret_cast<detail::ArenaPage::DestructorNode*>(
        align_up(reinterpret_cast<std::uintptr_t>(next), alignof(detail::ArenaPage::DestructorNode)));
    cursor = reinterpret_cast<std::byte*>(node) + sizeof(detail::ArenaPage::DestructorNode);
    return {object, node};
  }

  template <std::size_t Alignment, bool NeedsNode>
  [[nodiscard]] static std::size_t minimum_page_capacity(const std::size_t object_size) {
    static_assert(Alignment != 0);
    static_assert((Alignment & (Alignment - 1)) == 0);

    // begin() is already kAlignment-aligned; compute worst-case padding for Alignment
    constexpr std::size_t align_pad =
        Alignment > detail::ArenaPage::kAlignment ? Alignment - detail::ArenaPage::kAlignment : 0;

    if (align_pad > (std::numeric_limits<std::size_t>::max)() - object_size)
      throw std::bad_alloc{};

    const std::size_t total = align_pad + object_size;
    if constexpr (!NeedsNode)
      return total;

    const std::size_t aligned_total = static_cast<std::size_t>(
        align_up(static_cast<std::uintptr_t>(total), alignof(detail::ArenaPage::DestructorNode)));
    if (aligned_total > (std::numeric_limits<std::size_t>::max)() - sizeof(detail::ArenaPage::DestructorNode))
      throw std::bad_alloc{};

    return aligned_total + sizeof(detail::ArenaPage::DestructorNode);
  }

  template <std::size_t Alignment>
  [[nodiscard]] std::byte* allocate(const std::size_t size) {
    return reserve_storage<Alignment, false>(size).object;
  }

  [[nodiscard]] bool is_valid_marker(const ArenaMarker marker) const noexcept {
    if (current_page == nullptr || marker.page == nullptr)
      return false;

    for (const detail::ArenaPage* page = current_page; page != nullptr; page = page->previous) {
      if (page != marker.page)
        continue;

      const auto begin_addr = reinterpret_cast<std::uintptr_t>(page->begin());
      const auto marker_cursor_addr = reinterpret_cast<std::uintptr_t>(marker.cursor);

      if (page == current_page) {
        // The end of the current page is Arena::end; the marker's end must match
        if (marker.end != end) [[unlikely]]
          return false;
        const auto current_cursor_addr = reinterpret_cast<std::uintptr_t>(cursor);
        if (marker_cursor_addr < begin_addr || marker_cursor_addr > current_cursor_addr) [[unlikely]]
          return false;
      }
      else {
        // Finalized page: use marker.end for range validation
        const auto marker_end_addr = reinterpret_cast<std::uintptr_t>(marker.end);
        if (marker_end_addr < begin_addr) [[unlikely]]
          return false;
        if (marker_cursor_addr < begin_addr || marker_cursor_addr > marker_end_addr) [[unlikely]]
          return false;
      }

      auto* target_head = static_cast<detail::ArenaPage::DestructorNode*>(marker.destructor_head);
      return is_valid_destructor_head(target_head);
    }

    return false;
  }

  [[nodiscard]] bool is_valid_destructor_head(const detail::ArenaPage::DestructorNode* target) const noexcept {
    if (target == nullptr)
      return true;

    for (const detail::ArenaPage::DestructorNode* node = destructor_head; node != nullptr; node = node->previous) {
      if (node == target)
        return true;
    }

    return false;
  }

  void destroy_until(detail::ArenaPage::DestructorNode* target) noexcept {
    while (destructor_head != target) {
      detail::ArenaPage::DestructorNode* node = destructor_head;
      node->destroy(node->object, node->count);
      destructor_head = node->previous;
    }
  }

  void link_node(detail::ArenaPage::DestructorNode* node, void* object, const std::size_t count,
                 void (*destroy)(void*, std::size_t) noexcept) noexcept {
    ::new (node) detail::ArenaPage::DestructorNode{destructor_head, object, count, destroy};
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
  friend class ArenaAllocator;

  detail::ArenaPage* current_page = nullptr;
  std::byte* cursor = nullptr;
  std::byte* end = nullptr;
  detail::ArenaPage::DestructorNode* destructor_head = nullptr;
  std::byte* root_end = nullptr;
};

inline ArenaCheckpoint::~ArenaCheckpoint() noexcept { rollback(); }

inline void ArenaCheckpoint::rollback() noexcept {
  if (!active())
    return;

  arena->restore_unsafe(marker);
  release();
}

} // namespace baa
