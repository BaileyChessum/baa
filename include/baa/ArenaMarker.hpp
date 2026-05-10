#pragma once

#include <cstddef>

namespace baa {

struct ArenaPage;

/// Snapshot for use with Arena::mark(), restore(), and restore_unsafe().
struct ArenaMarker {
  ArenaPage* page = nullptr;
  std::byte* cursor = nullptr;
  std::byte* destructor_head = nullptr;
};

} // namespace baa
