#pragma once

#include <baa/FixedArenaMarker.hpp>

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
  ~FixedArenaCheckpoint() noexcept;

  FixedArenaCheckpoint(const FixedArenaCheckpoint&) = delete;
  FixedArenaCheckpoint& operator=(const FixedArenaCheckpoint&) = delete;

  FixedArenaCheckpoint(FixedArenaCheckpoint&& other) noexcept : arena(other.arena), mark(other.mark) {
    other.release();
  }

  FixedArenaCheckpoint& operator=(FixedArenaCheckpoint&& other) noexcept {
    if (this != &other) {
      rollback();
      arena = other.arena;
      mark = other.mark;
      other.release();
    }
    return *this;
  }

  /// Restore immediately when still active, then deactivate the guard.
  void rollback() noexcept;

  /// Deactivate the guard without changing arena state.
  void release() noexcept {
    arena = nullptr;
    mark = {};
  }

  /// @return `true` when this guard still owns rollback responsibility.
  [[nodiscard]] bool active() const noexcept { return arena != nullptr; }

private:
  FixedArenaCheckpoint(FixedArena* owner, const FixedArenaMarker saved) noexcept : arena(owner), mark(saved) {}

  friend class FixedArena;

  FixedArena* arena = nullptr;
  FixedArenaMarker mark{};
};

} // namespace baa
