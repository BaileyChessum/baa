#include <baa/Bump.hpp>
#include <baa/BumpAllocator.hpp>

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
  EXPECT_TRUE(s.restore(m));
  EXPECT_EQ(s.used(), 0u);
  EXPECT_EQ(s.remaining(), s.capacity());
}

TEST(Bump, RestoreUnsafeOnFreshStorage) {
  Bump s(256);
  BumpMark m = s.mark();
  s.restore_unsafe(m);
  EXPECT_EQ(s.used(), 0u);
  EXPECT_EQ(s.remaining(), s.capacity());
}

TEST(Bump, ResetInvalidatesMarks) {
  Bump s(256);
  BumpAllocator<int> alloc(s);
  (void)alloc.allocate(4);
  BumpMark m = s.mark();
  s.reset();
  EXPECT_TRUE(s.restore(m));
  EXPECT_EQ(s.used(), 4u * sizeof(int));
  EXPECT_EQ(s.used() + s.remaining(), s.capacity());
}

TEST(Bump, RejectsForeignMarks) {
  Bump a(256);
  Bump b(256);
  BumpMark m = a.mark();
  EXPECT_FALSE(b.restore(m));
  EXPECT_EQ(b.used() + b.remaining(), b.capacity());
}

// --- Move semantics ---------------------------------------------------------

TEST(Bump, MoveConstructor) {
  Bump a(128);
  Bump b(std::move(a));
  EXPECT_EQ(b.capacity(), 128u);
  EXPECT_EQ(b.used(), 0u);
  EXPECT_EQ(b.remaining(), 128u);
}

TEST(Bump, MarkSurvivesMoveConstruction) {
  Bump a(128);
  BumpAllocator<int> alloc(a);
  (void)alloc.allocate(4);
  BumpMark m = a.mark();
  (void)alloc.allocate(4);

  Bump b(std::move(a));
  EXPECT_TRUE(b.restore(m));
  EXPECT_EQ(b.used() + b.remaining(), b.capacity());
}

TEST(Bump, RestoreUnsafeSurvivesMoveConstruction) {
  Bump a(128);
  BumpAllocator<int> alloc(a);
  (void)alloc.allocate(4);
  BumpMark m = a.mark();
  (void)alloc.allocate(4);

  Bump b(std::move(a));
  b.restore_unsafe(m);
  EXPECT_EQ(b.used() + b.remaining(), b.capacity());
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
