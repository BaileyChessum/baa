#include <baa/Bump.hpp>

#include <gtest/gtest.h>

// Note: Bump::allocate is private — allocation behaviour is tested via BumpAllocator<T>.
// These tests cover construction, capacity accounting, mark/restore, reset, and move semantics.

namespace baa {

// --- Construction -----------------------------------------------------------

TEST(Bump, InitialState) {
  Bump s(256);
  EXPECT_EQ(s.capacity(), 256u);
  EXPECT_EQ(s.used(), 0u);
  EXPECT_EQ(s.remaining(), 256u);
}

TEST(Bump, ZeroCapacity) {
  Bump s(0);
  EXPECT_EQ(s.capacity(), 0u);
  EXPECT_EQ(s.used(), 0u);
  EXPECT_EQ(s.remaining(), 0u);
}

TEST(Bump, CapacityUsedRemainingInvariant) {
  Bump s(1024);
  EXPECT_EQ(s.used() + s.remaining(), s.capacity());
}

// --- Reset ------------------------------------------------------------------

TEST(Bump, ResetOnFreshStorage) {
  Bump s(256);
  s.reset();
  EXPECT_EQ(s.used(), 0u);
  EXPECT_EQ(s.remaining(), s.capacity());
}

// --- Mark / restore ---------------------------------------------------------

TEST(Bump, MarkRestoreOnFreshStorage) {
  Bump s(256);
  BumpMark m = s.mark();
  s.restore(m);
  EXPECT_EQ(s.used(), 0u);
  EXPECT_EQ(s.remaining(), s.capacity());
}

// --- Move semantics ---------------------------------------------------------

TEST(Bump, MoveConstructor) {
  Bump a(128);
  Bump b(std::move(a));
  EXPECT_EQ(b.capacity(), 128u);
  EXPECT_EQ(b.used(), 0u);
  EXPECT_EQ(b.remaining(), 128u);
}

TEST(Bump, MoveAssignment) {
  Bump a(128);
  Bump b(64);
  b = std::move(a);
  EXPECT_EQ(b.capacity(), 128u);
  EXPECT_EQ(b.used(), 0u);
  EXPECT_EQ(b.remaining(), 128u);
}

TEST(Bump, SelfMoveAssignment) {
  Bump s(256);
  auto* self = &s;
  *self = std::move(s); // must not crash or corrupt
}

} // namespace baa
