#pragma once

#include <cstddef>

namespace baa {

/// Snapshot for use with FixedArena::mark(), restore(), and restore_unsafe().
struct FixedArenaMarker {
  std::byte* cursor = nullptr;
  std::byte* destructor_head = nullptr;
};

} // namespace baa
