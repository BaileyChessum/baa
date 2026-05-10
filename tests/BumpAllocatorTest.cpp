#include <baa/BumpAllocator.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <new>
#include <vector>

namespace baa {

// --- Basic allocation -------------------------------------------------------

TEST(BumpAllocator, AllocateSingle) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  int* p = alloc.allocate(1);
  ASSERT_NE(p, nullptr);
  *p = 42;
  EXPECT_EQ(*p, 42);
}

TEST(BumpAllocator, AllocateArray) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  int* p = alloc.allocate(8);
  for (int i = 0; i < 8; ++i)
    p[i] = i;
  for (int i = 0; i < 8; ++i)
    EXPECT_EQ(p[i], i);
}

TEST(BumpAllocator, SequentialAllocationsDoNotOverlap) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  int* a = alloc.allocate(1);
  int* b = alloc.allocate(1);
  EXPECT_NE(a, b);
  // Regions must not overlap: b is at least sizeof(int) past a.
  EXPECT_GE(reinterpret_cast<std::byte*>(b) - reinterpret_cast<std::byte*>(a),
            static_cast<std::ptrdiff_t>(sizeof(int)));
}

// --- Alignment --------------------------------------------------------------

TEST(BumpAllocator, RespectsAlignment) {
  Bump bump(512);

  // Interleave a char allocation to force potential misalignment, then check
  // that the double allocation is still properly aligned.
  BumpAllocator<char>   chars(bump);
  BumpAllocator<double> doubles(bump);

  (void)chars.allocate(1); // nudge cursor off double-alignment
  double* p = doubles.allocate(1);

  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(double), 0u);
}

// --- Exhaustion -------------------------------------------------------------

TEST(BumpAllocator, ExhaustionThrowsBadAlloc) {
  Bump bump(sizeof(int));
  BumpAllocator<int> alloc(bump);
  (void)alloc.allocate(1);
  EXPECT_THROW((void)alloc.allocate(1), std::bad_alloc);
}

TEST(BumpAllocator, OversizedAllocationThrowsBadAlloc) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  std::size_t oversized = std::numeric_limits<std::size_t>::max() / sizeof(int) + 1;
  EXPECT_THROW((void)alloc.allocate(oversized), std::bad_alloc);
}

// --- Bump accounting --------------------------------------------------------

TEST(BumpAllocator, UsedIncreasesAfterAllocate) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  EXPECT_EQ(bump.used(), 0u);
  (void)alloc.allocate(1);
  EXPECT_GE(bump.used(), sizeof(int));
}

TEST(BumpAllocator, FailedOversizedAllocationDoesNotAdvanceCursor) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  std::size_t usedBefore = bump.used();
  std::size_t oversized = std::numeric_limits<std::size_t>::max() / sizeof(int) + 1;
  EXPECT_THROW((void)alloc.allocate(oversized), std::bad_alloc);
  EXPECT_EQ(bump.used(), usedBefore);
  EXPECT_EQ(bump.used() + bump.remaining(), bump.capacity());
}

TEST(BumpAllocator, ResetReclaims) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  (void)alloc.allocate(4);
  bump.reset();
  EXPECT_EQ(bump.used(), 0u);
  EXPECT_EQ(bump.remaining(), bump.capacity());
}

TEST(BumpAllocator, MarkRestore) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  (void)alloc.allocate(4);
  std::size_t usedAtMark = bump.used();
  BumpMark m = bump.mark();
  (void)alloc.allocate(4);
  EXPECT_GT(bump.used(), usedAtMark);
  EXPECT_TRUE(bump.restore(m));
  EXPECT_EQ(bump.used(), usedAtMark);
}

// --- Deallocate is a no-op --------------------------------------------------

TEST(BumpAllocator, DeallocateDoesNothing) {
  Bump bump(256);
  BumpAllocator<int> alloc(bump);
  int* p = alloc.allocate(1);
  std::size_t usedBefore = bump.used();
  alloc.deallocate(p, 1);
  EXPECT_EQ(bump.used(), usedBefore); // cursor unchanged
}

// --- Equality ---------------------------------------------------------------

TEST(BumpAllocator, EqualWhenSameBump) {
  Bump bump(256);
  BumpAllocator<int>   a(bump);
  BumpAllocator<float> b(bump);
  EXPECT_TRUE(a == b);
}

TEST(BumpAllocator, NotEqualWhenDifferentBump) {
  Bump a(256);
  Bump b(256);
  BumpAllocator<int> x(a);
  BumpAllocator<int> y(b);
  EXPECT_FALSE(x == y);
}

// --- STL container ----------------------------------------------------------

TEST(BumpAllocator, StdVector) {
  Bump bump(4096);
  BumpAllocator<int> alloc(bump);
  std::vector<int, BumpAllocator<int>> v(alloc);
  v.reserve(64);
  for (int i = 0; i < 64; ++i)
    v.push_back(i);
  for (std::size_t i = 0; i < 64; ++i)
    EXPECT_EQ(v[i], static_cast<int>(i));
}

} // namespace baa
