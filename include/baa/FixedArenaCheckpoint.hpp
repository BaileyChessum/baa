#pragma once

#include <baa/FixedArenaMark.hpp>

namespace baa {

class FixedArena;

/**
 * @brief One-shot scope guard that restores a `FixedArena` to a saved mark unless released.
 *
 * Constructed by `FixedArena::checkpoint()`.
 */
class FixedArenaCheckpoint {
public:
  FixedArenaCheckpoint() noexcept = default;
  ~FixedArenaCheckpoint() { rollback(); }

  FixedArenaCheckpoint(const FixedArenaCheckpoint&) = delete;
  FixedArenaCheckpoint& operator=(const FixedArenaCheckpoint&) = delete;

  FixedArenaCheckpoint(FixedArenaCheckpoint&& other) noexcept
      : arena(other.arena), mark(other.mark), restore_fn(other.restore_fn) {
    other.release();
  }

  FixedArenaCheckpoint& operator=(FixedArenaCheckpoint&& other) noexcept {
    if (this != &other)
    {
      rollback();
      arena = other.arena;
      mark = other.mark;
      restore_fn = other.restore_fn;
      other.release();
    }
    return *this;
  }

  /// Restore immediately when still active, then deactivate the guard.
  void rollback() noexcept {
    if (!active())
      return;

    restore_fn(arena, mark);
    release();
  }

  /// Deactivate the guard without changing arena state.
  void release() noexcept {
    arena = nullptr;
    mark = {};
    restore_fn = nullptr;
  }

  /// @return `true` when this guard still owns rollback responsibility.
  [[nodiscard]] bool active() const noexcept { return restore_fn != nullptr; }

private:
  using RestoreFn = void (*)(FixedArena*, FixedArenaMark) noexcept;

  FixedArenaCheckpoint(FixedArena* owner, const FixedArenaMark saved, RestoreFn restore) noexcept
      : arena(owner), mark(saved), restore_fn(restore) {}

  friend class FixedArena;

  FixedArena* arena = nullptr;
  FixedArenaMark mark{};
  RestoreFn restore_fn = nullptr;
};

} // namespace baa
