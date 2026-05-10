#pragma once

#include <baa/BumpMarker.hpp>

namespace baa {

class Bump;

/**
 * @brief One-shot scope guard that restores a `Bump` to a saved mark unless released.
 *
 * Constructed by `Bump::checkpoint()`.
 */
class BumpCheckpoint {
public:
  BumpCheckpoint() noexcept = default;
  ~BumpCheckpoint() noexcept;

  BumpCheckpoint(const BumpCheckpoint&) = delete;
  BumpCheckpoint& operator=(const BumpCheckpoint&) = delete;

  BumpCheckpoint(BumpCheckpoint&& other) noexcept : bump(other.bump), mark(other.mark) { other.release(); }

  BumpCheckpoint& operator=(BumpCheckpoint&& other) noexcept {
    if (this != &other) {
      rollback();
      bump = other.bump;
      mark = other.mark;
      other.release();
    }
    return *this;
  }

  /// Restore immediately when still active, then deactivate the guard.
  void rollback() noexcept;

  /// Deactivate the guard without changing arena state.
  void release() noexcept {
    bump = nullptr;
    mark = {};
  }

  /// @return `true` when this guard still owns rollback responsibility.
  [[nodiscard]] bool active() const noexcept { return bump != nullptr; }

private:
  BumpCheckpoint(Bump* owner, const BumpMarker saved) noexcept : bump(owner), mark(saved) {}

  friend class Bump;

  Bump* bump = nullptr;
  BumpMarker mark{};
};

} // namespace baa
