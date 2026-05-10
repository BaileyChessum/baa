#pragma once

#include <cstddef>

namespace baa {

/// Opaque cursor snapshot for use with Bump::mark() and Bump::restore().
/// A mark is invalidated by any call to Bump::reset() or by restoring an earlier mark.
struct BumpMark {
  std::byte* cursor;
};

} // namespace baa
