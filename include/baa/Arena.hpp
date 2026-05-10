#pragma once

#include <baa/ArenaCheckpoint.hpp>
#include <baa/ArenaMarker.hpp>

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

struct ArenaPage {
  struct DestructorNode {
    DestructorNode* previous;
    void* object;
    std::size_t count;
    void (*destroy)(void*, std::size_t) noexcept;
  };

  ArenaPage* previous = nullptr;
  std::byte* begin = nullptr;
  std::byte* cursor = nullptr;
  std::byte* end = nullptr;
  DestructorNode* destructor_head = nullptr;
  std::size_t usable_capacity = 0;
};

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
  explicit Arena(const std::size_t initial_capacity)
      : next_geometric_capacity(initial_capacity) {
    first_page = allocate_page(initial_capacity, nullptr);
    current_page = first_page;
    total_capacity = initial_capacity;
  }

  ~Arena() { release_all_pages(); }

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  Arena(Arena&& other) noexcept
      : first_page(other.first_page)
      , current_page(other.current_page)
      , total_capacity(other.total_capacity)
      , next_geometric_capacity(other.next_geometric_capacity) {
    other.first_page = nullptr;
    other.current_page = nullptr;
    other.total_capacity = 0;
    other.next_geometric_capacity = 0;
  }

  Arena& operator=(Arena&& other) noexcept {
    if (this != &other)
    {
      release_all_pages();

      first_page = other.first_page;
      current_page = other.current_page;
      total_capacity = other.total_capacity;
      next_geometric_capacity = other.next_geometric_capacity;

      other.first_page = nullptr;
      other.current_page = nullptr;
      other.total_capacity = 0;
      other.next_geometric_capacity = 0;
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

    ReservedStorage storage = reserve_storage<alignof(T)>(sizeof(T), !std::is_trivially_destructible_v<T>);

    try
    {
      T* object = ::new (storage.object) T(std::forward<Args>(args)...);
      if constexpr (!std::is_trivially_destructible_v<T>)
        link_node(storage.page, storage.node, object, 1, &destroy_range<T>);
      return *object;
    }
    catch (...)
    {
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
    ReservedStorage storage = reserve_storage<alignof(T)>(sizeof(T) * count, !std::is_trivially_destructible_v<T>);
    T* objects = reinterpret_cast<T*>(storage.object);

    std::size_t constructed = 0;
    try
    {
      for (; constructed < count; ++constructed)
        ::new (objects + constructed) T();
    }
    catch (...)
    {
      destroy_constructed_prefix(objects, constructed);
      restore_unsafe(saved);
      throw;
    }

    if constexpr (!std::is_trivially_destructible_v<T>)
      link_node(storage.page, storage.node, objects, count, &destroy_range<T>);

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

    return {current_page, current_page->cursor, reinterpret_cast<std::byte*>(current_page->destructor_head)};
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
   * ancestors for the target page.
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

    auto* target_page = marker.page;
    auto* target_head = reinterpret_cast<ArenaPage::DestructorNode*>(marker.destructor_head);

    while (current_page != target_page)
    {
      ArenaPage* doomed = current_page;
      destroy_page_until(doomed, nullptr);
      current_page = doomed->previous;
      total_capacity -= doomed->usable_capacity;
      deallocate_page(doomed);
    }

    destroy_page_until(current_page, target_head);
    current_page->cursor = marker.cursor;
    current_page->destructor_head = target_head;
    next_geometric_capacity = doubled_capacity(current_page->usable_capacity);
  }

  /**
   * @brief Reset the arena to the beginning of its first page.
   * @warning Destroys all arena-owned non-trivial objects and invalidates outstanding storage,
   * including storage kept alive through `ArenaAllocator<T>`.
   */
  void reset() noexcept {
    if (first_page == nullptr) [[unlikely]]
      return;

    while (current_page != first_page)
    {
      ArenaPage* doomed = current_page;
      destroy_page_until(doomed, nullptr);
      current_page = doomed->previous;
      total_capacity -= doomed->usable_capacity;
      deallocate_page(doomed);
    }

    destroy_page_until(first_page, nullptr);
    first_page->cursor = first_page->begin;
    first_page->destructor_head = nullptr;
    current_page = first_page;
    total_capacity = first_page->usable_capacity;
    next_geometric_capacity = doubled_capacity(first_page->usable_capacity);
  }

  /// Total usable capacity across all currently live pages.
  [[nodiscard]] std::size_t capacity() const noexcept { return total_capacity; }

  /**
   * Bytes consumed so far.
   * @note Earlier live pages are treated as fully consumed because forward allocation never reuses
   * their tail slack without `restore()` or `reset()`.
   */
  [[nodiscard]] std::size_t used() const noexcept { return capacity() - remaining(); }

  /// Bytes remaining in the current page before another page allocation would be needed.
  [[nodiscard]] std::size_t remaining() const noexcept {
    if (current_page == nullptr)
      return 0;

    return static_cast<std::size_t>(current_page->end - current_page->cursor);
  }

private:
  struct ReservedStorage {
    ArenaPage* page = nullptr;
    std::byte* object = nullptr;
    ArenaPage::DestructorNode* node = nullptr;
  };

  static constexpr std::size_t page_alignment = alignof(std::max_align_t);

  static std::size_t align_up_size(const std::size_t value, const std::size_t alignment) noexcept {
    const std::size_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
  }

  static std::uintptr_t align_up_address(const std::uintptr_t value, const std::size_t alignment) noexcept {
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

  [[nodiscard]] static ArenaPage* allocate_page(const std::size_t usable_capacity, ArenaPage* previous) {
    const std::size_t header_bytes = sizeof(ArenaPage);
    if (header_bytes > (std::numeric_limits<std::size_t>::max)() - (page_alignment - 1) ||
        usable_capacity > (std::numeric_limits<std::size_t>::max)() - header_bytes - (page_alignment - 1))
      throw std::bad_alloc{};

    void* raw = ::operator new(header_bytes + (page_alignment - 1) + usable_capacity);
    auto* page = ::new (raw) ArenaPage{};
    page->previous = previous;

    const auto raw_addr = reinterpret_cast<std::uintptr_t>(reinterpret_cast<std::byte*>(raw) + header_bytes);
    page->begin = reinterpret_cast<std::byte*>(align_up_address(raw_addr, page_alignment));
    page->cursor = page->begin;
    page->end = page->begin + static_cast<std::ptrdiff_t>(usable_capacity);
    page->destructor_head = nullptr;
    page->usable_capacity = usable_capacity;
    return page;
  }

  static void deallocate_page(ArenaPage* page) noexcept {
    page->~ArenaPage();
    ::operator delete(page);
  }

  void release_all_pages() noexcept {
    while (current_page != nullptr)
    {
      ArenaPage* doomed = current_page;
      destroy_page_until(doomed, nullptr);
      current_page = doomed->previous;
      deallocate_page(doomed);
    }

    first_page = nullptr;
    total_capacity = 0;
    next_geometric_capacity = 0;
  }

  template <std::size_t Alignment>
  [[nodiscard]] static bool try_reserve_in_page(ArenaPage* page, const std::size_t object_size, const bool needs_node,
                                                ReservedStorage& storage) noexcept {
    static_assert(Alignment != 0);
    static_assert((Alignment & (Alignment - 1)) == 0);

    const auto cursor_addr = reinterpret_cast<std::uintptr_t>(page->cursor);
    const auto aligned_object_addr = align_up_address(cursor_addr, Alignment);
    std::byte* object = reinterpret_cast<std::byte*>(aligned_object_addr);
    if (object > page->end) [[unlikely]]
      return false;

    const std::size_t after_object_available = static_cast<std::size_t>(page->end - object);
    if (object_size > after_object_available) [[unlikely]]
      return false;

    std::byte* next_cursor = object + static_cast<std::ptrdiff_t>(object_size);
    ArenaPage::DestructorNode* node = nullptr;
    if (needs_node)
    {
      const auto node_addr = align_up_address(reinterpret_cast<std::uintptr_t>(next_cursor), alignof(ArenaPage::DestructorNode));
      std::byte* node_bytes = reinterpret_cast<std::byte*>(node_addr);
      if (node_bytes > page->end) [[unlikely]]
        return false;

      const std::size_t after_node_available = static_cast<std::size_t>(page->end - node_bytes);
      if (sizeof(ArenaPage::DestructorNode) > after_node_available) [[unlikely]]
        return false;

      node = reinterpret_cast<ArenaPage::DestructorNode*>(node_bytes);
      next_cursor = node_bytes + static_cast<std::ptrdiff_t>(sizeof(ArenaPage::DestructorNode));
    }

    page->cursor = next_cursor;
    storage.page = page;
    storage.object = object;
    storage.node = node;
    return true;
  }

  template <std::size_t Alignment>
  [[nodiscard]] ReservedStorage reserve_storage(const std::size_t object_size, const bool needs_node) {
    ReservedStorage storage{};
    if (current_page != nullptr && try_reserve_in_page<Alignment>(current_page, object_size, needs_node, storage))
      return storage;

    const std::size_t minimum_capacity = minimum_page_capacity<Alignment>(object_size, needs_node);
    const std::size_t new_capacity = next_geometric_capacity > minimum_capacity ? next_geometric_capacity : minimum_capacity;

    ArenaPage* new_page = allocate_page(new_capacity, current_page);
    current_page = new_page;
    total_capacity += new_capacity;
    next_geometric_capacity = doubled_capacity(new_capacity);

    const bool reserved = try_reserve_in_page<Alignment>(current_page, object_size, needs_node, storage);
    (void)reserved;
    return storage;
  }

  template <std::size_t Alignment>
  [[nodiscard]] static std::size_t minimum_page_capacity(const std::size_t object_size, const bool needs_node) {
    static_assert(Alignment != 0);
    static_assert((Alignment & (Alignment - 1)) == 0);

    const std::size_t object_offset = align_up_size(0, Alignment);
    if (object_offset > (std::numeric_limits<std::size_t>::max)() - object_size)
      throw std::bad_alloc{};

    std::size_t total = object_offset + object_size;
    if (!needs_node)
      return total;

    const std::size_t node_offset = align_up_size(total, alignof(ArenaPage::DestructorNode));
    if (node_offset > (std::numeric_limits<std::size_t>::max)() - sizeof(ArenaPage::DestructorNode))
      throw std::bad_alloc{};

    return node_offset + sizeof(ArenaPage::DestructorNode);
  }

  template <std::size_t Alignment>
  [[nodiscard]] std::byte* allocate(const std::size_t size) {
    return reserve_storage<Alignment>(size, false).object;
  }

  [[nodiscard]] bool is_valid_marker(const ArenaMarker marker) const noexcept {
    if (current_page == nullptr || marker.page == nullptr)
      return false;

    for (ArenaPage* page = current_page; page != nullptr; page = page->previous)
    {
      if (page != marker.page)
        continue;

      const auto begin_addr = reinterpret_cast<std::uintptr_t>(page->begin);
      const auto marker_addr = reinterpret_cast<std::uintptr_t>(marker.cursor);
      const auto end_addr = reinterpret_cast<std::uintptr_t>(page == current_page ? page->cursor : page->end);
      if (marker_addr < begin_addr || marker_addr > end_addr) [[unlikely]]
        return false;

      return is_valid_destructor_head(page, reinterpret_cast<ArenaPage::DestructorNode*>(marker.destructor_head));
    }

    return false;
  }

  [[nodiscard]] static bool is_valid_destructor_head(const ArenaPage* page, const ArenaPage::DestructorNode* target) noexcept {
    if (target == nullptr)
      return true;

    const auto begin_addr = reinterpret_cast<std::uintptr_t>(page->begin);
    const auto current_addr = reinterpret_cast<std::uintptr_t>(page->cursor);
    const auto target_addr = reinterpret_cast<std::uintptr_t>(target);
    if (target_addr < begin_addr || target_addr >= current_addr) [[unlikely]]
      return false;

    for (const ArenaPage::DestructorNode* node = page->destructor_head; node != nullptr; node = node->previous)
    {
      if (node == target)
        return true;
    }

    return false;
  }

  static void destroy_page_until(ArenaPage* page, ArenaPage::DestructorNode* target) noexcept {
    while (page->destructor_head != target)
    {
      ArenaPage::DestructorNode* node = page->destructor_head;
      node->destroy(node->object, node->count);
      page->destructor_head = node->previous;
    }
  }

  static void link_node(ArenaPage* page, ArenaPage::DestructorNode* node, void* object, const std::size_t count,
                        void (*destroy)(void*, std::size_t) noexcept) noexcept {
    ::new (node) ArenaPage::DestructorNode{page->destructor_head, object, count, destroy};
    page->destructor_head = node;
  }

  template <typename T>
  static void destroy_range(void* object, const std::size_t count) noexcept {
    T* typed = static_cast<T*>(object);
    for (std::size_t i = count; i > 0; --i)
      typed[i - 1].~T();
  }

  template <typename T>
  static void destroy_constructed_prefix(T* objects, const std::size_t count) noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>)
    {
      for (std::size_t i = count; i > 0; --i)
        objects[i - 1].~T();
    }
  }

  template <typename T>
  friend class ArenaAllocator;

  ArenaPage* first_page = nullptr;
  ArenaPage* current_page = nullptr;
  std::size_t total_capacity = 0;
  std::size_t next_geometric_capacity = 0;
};

inline ArenaCheckpoint::~ArenaCheckpoint() noexcept {
  rollback();
}

inline void ArenaCheckpoint::rollback() noexcept {
  if (!active())
    return;

  arena->restore_unsafe(marker);
  release();
}

} // namespace baa
