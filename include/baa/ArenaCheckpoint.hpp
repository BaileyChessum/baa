#pragma once

#include <baa/ArenaMarker.hpp>

namespace baa {

class Arena;

/**
 * @brief One-shot scope guard that restores an `Arena` to a saved mark unless released.
 *
 * Constructed by `Arena::checkpoint()`.
 */
class ArenaCheckpoint {
public:
  ArenaCheckpoint() noexcept = default;
  ~ArenaCheckpoint() noexcept;

  ArenaCheckpoint(const ArenaCheckpoint&) = delete;
  ArenaCheckpoint& operator=(const ArenaCheckpoint&) = delete;

  ArenaCheckpoint(ArenaCheckpoint&& other) noexcept
      : arena(other.arena), marker(other.marker) {
    other.release();
  }

  ArenaCheckpoint& operator=(ArenaCheckpoint&& other) noexcept {
    if (this != &other)
    {
      rollback();
      arena = other.arena;
      marker = other.marker;
      other.release();
    }
    return *this;
  }

  /// Restore immediately when still active, then deactivate the guard.
  void rollback() noexcept;

  /// Deactivate the guard without changing arena state.
  void release() noexcept {
    arena = nullptr;
    marker = {};
  }

  /// @return `true` when this guard still owns rollback responsibility.
  [[nodiscard]] bool active() const noexcept { return arena != nullptr; }

private:
  ArenaCheckpoint(Arena* owner, const ArenaMarker saved) noexcept : arena(owner), marker(saved) {}

  friend class Arena;

  Arena* arena = nullptr;
  ArenaMarker marker{};
};

} // namespace baa
