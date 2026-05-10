#pragma once

#include <cstddef>

namespace baa {

namespace detail {
struct ArenaPage;
}

/// Snapshot for use with Arena::mark(), restore(), and restore_unsafe().
struct ArenaMarker {
  detail::ArenaPage* page = nullptr;
  std::byte* cursor = nullptr;
  void* destructor_head = nullptr;
};

} // namespace baa
