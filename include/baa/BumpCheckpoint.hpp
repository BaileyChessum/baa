#pragma once

#include <baa/BumpMark.hpp>

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
  ~BumpCheckpoint() { rollback(); }

  BumpCheckpoint(const BumpCheckpoint&) = delete;
  BumpCheckpoint& operator=(const BumpCheckpoint&) = delete;

  BumpCheckpoint(BumpCheckpoint&& other) noexcept
      : bump(other.bump), mark(other.mark), restore_fn(other.restore_fn) {
    other.release();
  }

  BumpCheckpoint& operator=(BumpCheckpoint&& other) noexcept {
    if (this != &other)
    {
      rollback();
      bump = other.bump;
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

    restore_fn(bump, mark);
    release();
  }

  /// Deactivate the guard without changing arena state.
  void release() noexcept {
    bump = nullptr;
    mark = {};
    restore_fn = nullptr;
  }

  /// @return `true` when this guard still owns rollback responsibility.
  [[nodiscard]] bool active() const noexcept { return restore_fn != nullptr; }

private:
  using RestoreFn = void (*)(Bump*, BumpMark) noexcept;

  BumpCheckpoint(Bump* owner, const BumpMark saved, RestoreFn restore) noexcept
      : bump(owner), mark(saved), restore_fn(restore) {}

  friend class Bump;

  Bump* bump = nullptr;
  BumpMark mark{};
  RestoreFn restore_fn = nullptr;
};

} // namespace baa
