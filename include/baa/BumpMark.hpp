#pragma once

#include <cstddef>

namespace baa {

/// Cursor snapshot for use with Bump::mark() and Bump::restore().
struct BumpMark {
  std::byte* cursor = nullptr;
};

} // namespace baa
