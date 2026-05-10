#pragma once

#include <cstddef>

namespace baa {

/// Cursor snapshot for use with Bump::mark() and Bump::restore().
struct BumpMark {
  std::byte*  buffer = nullptr;
  std::size_t offset = 0;
};

} // namespace baa
