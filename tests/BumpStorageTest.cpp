#include <baa/Bump.hpp>
#include <baa/BumpAllocator.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <utility>

// Note: Bump::allocate is private — allocation behaviour is tested via BumpAllocator<T>.
// These tests cover construction, capacity accounting, mark/restore, reset, and move semantics.

namespace baa {

namespace {

struct ThrowsOnDefaultConstruct {
  inline static int attempts = 0;

  ThrowsOnDefaultConstruct() {
    ++attempts;
    throw std::runtime_error("boom");
  }
};

struct ThrowsOnValueConstruct {
  inline static int attempts = 0;

  explicit ThrowsOnValueConstruct(int) {
    ++attempts;
    throw std::runtime_error("boom");
  }
};

static_assert(std::is_trivially_destructible_v<ThrowsOnDefaultConstruct>);
static_assert(std::is_trivially_destructible_v<ThrowsOnValueConstruct>);

} // namespace

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
  BumpMarker m = s.mark();
  EXPECT_TRUE(s.restore(m));
  EXPECT_EQ(s.used(), 0u);
  EXPECT_EQ(s.remaining(), s.capacity());
}

TEST(Bump, RestoreUnsafeOnFreshStorage) {
  Bump s(256);
  BumpMarker m = s.mark();
  s.restore_unsafe(m);
  EXPECT_EQ(s.used(), 0u);
  EXPECT_EQ(s.remaining(), s.capacity());
}

TEST(Bump, ResetInvalidatesMarks) {
  Bump s(256);
  BumpAllocator<int> alloc(s);
  (void)alloc.allocate(4);
  BumpMarker m = s.mark();
  s.reset();
  EXPECT_TRUE(s.restore(m));
  EXPECT_EQ(s.used(), 4u * sizeof(int));
  EXPECT_EQ(s.used() + s.remaining(), s.capacity());
}

TEST(Bump, RejectsForeignMarks) {
  Bump a(256);
  Bump b(256);
  BumpMarker m = a.mark();
  EXPECT_FALSE(b.restore(m));
  EXPECT_EQ(b.used() + b.remaining(), b.capacity());
}

TEST(Bump, CheckpointDestructorRollsBackOnScopeExit) {
  Bump s(256);
  BumpAllocator<int> alloc(s);

  {
    auto checkpoint = s.checkpoint();
    (void)alloc.allocate(4);
    EXPECT_TRUE(checkpoint.active());
    EXPECT_GT(s.used(), 0u);
  }

  EXPECT_EQ(s.used(), 0u);
}

TEST(Bump, CheckpointDestructorRollsBackOnExceptionExit) {
  Bump s(256);
  BumpAllocator<int> alloc(s);

  try
  {
    auto checkpoint = s.checkpoint();
    (void)alloc.allocate(4);
    throw std::runtime_error("boom");
  }
  catch (const std::runtime_error&)
  {}

  EXPECT_EQ(s.used(), 0u);
}

TEST(Bump, CheckpointReleaseKeepsAllocations) {
  Bump s(256);
  BumpAllocator<int> alloc(s);

  {
    auto checkpoint = s.checkpoint();
    (void)alloc.allocate(4);
    checkpoint.release();
    EXPECT_FALSE(checkpoint.active());
  }

  EXPECT_EQ(s.used(), 4u * sizeof(int));
}

TEST(Bump, CheckpointRollbackRestoresImmediately) {
  Bump s(256);
  BumpAllocator<int> alloc(s);

  auto checkpoint = s.checkpoint();
  (void)alloc.allocate(4);
  EXPECT_GT(s.used(), 0u);

  checkpoint.rollback();
  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(s.used(), 0u);
}

TEST(Bump, CheckpointReleaseIsIdempotent) {
  Bump s(256);
  BumpAllocator<int> alloc(s);

  auto checkpoint = s.checkpoint();
  (void)alloc.allocate(4);
  checkpoint.release();
  checkpoint.release();

  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(s.used(), 4u * sizeof(int));
}

TEST(Bump, CheckpointRollbackIsIdempotent) {
  Bump s(256);
  BumpAllocator<int> alloc(s);

  auto checkpoint = s.checkpoint();
  (void)alloc.allocate(4);
  checkpoint.rollback();
  checkpoint.rollback();

  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(s.used(), 0u);
}

TEST(Bump, CheckpointMoveTransfersResponsibility) {
  Bump s(256);
  BumpAllocator<int> alloc(s);

  {
    auto original = s.checkpoint();
    (void)alloc.allocate(4);
    auto moved = std::move(original);
    EXPECT_FALSE(original.active());
    EXPECT_TRUE(moved.active());
  }

  EXPECT_EQ(s.used(), 0u);
}

TEST(Bump, MovedFromCheckpointIsInactive) {
  Bump s(256);
  auto original = s.checkpoint();
  auto moved = std::move(original);

  EXPECT_FALSE(original.active());
  EXPECT_TRUE(moved.active());
}

TEST(Bump, NestedCheckpointsUnwindInLifoOrder) {
  Bump s(256);
  BumpAllocator<int> alloc(s);

  auto outer = s.checkpoint();
  (void)alloc.allocate(2);
  const std::size_t after_outer = s.used();

  {
    auto inner = s.checkpoint();
    (void)alloc.allocate(3);
    EXPECT_GT(s.used(), after_outer);
  }

  EXPECT_EQ(s.used(), after_outer);
  outer.rollback();
  EXPECT_EQ(s.used(), 0u);
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
  BumpMarker m = a.mark();
  (void)alloc.allocate(4);

  Bump b(std::move(a));
  EXPECT_TRUE(b.restore(m));
  EXPECT_EQ(b.used() + b.remaining(), b.capacity());
}

TEST(Bump, RestoreUnsafeSurvivesMoveConstruction) {
  Bump a(128);
  BumpAllocator<int> alloc(a);
  (void)alloc.allocate(4);
  BumpMarker m = a.mark();
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

// --- emplace ----------------------------------------------------------------

TEST(Bump, EmplaceDefaultConstruct) {
  Bump s(256);
  int& x = s.emplace<int>();
  x = 42;
  EXPECT_EQ(x, 42);
  EXPECT_GT(s.used(), 0u);
}

TEST(Bump, EmplaceWithArgs) {
  struct Point {
    int x, y;
  };
  Bump s(256);
  Point& p = s.emplace<Point>(3, 7);
  EXPECT_EQ(p.x, 3);
  EXPECT_EQ(p.y, 7);
}

TEST(Bump, EmplaceAlignment) {
  Bump s(512);
  (void)s.emplace<char>(); // nudge cursor
  double& d = s.emplace<double>();
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&d) % alignof(double), 0u);
}

TEST(Bump, EmplaceExhaustionThrows) {
  Bump s(1);
  EXPECT_THROW((void)s.emplace<int>(), std::bad_alloc);
}

TEST(Bump, EmplaceConstructorFailureRollsBackCursor) {
  Bump s(256);
  const std::size_t usedBefore = s.used();
  ThrowsOnValueConstruct::attempts = 0;

  EXPECT_THROW((void)s.emplace<ThrowsOnValueConstruct>(7), std::runtime_error);
  EXPECT_EQ(ThrowsOnValueConstruct::attempts, 1);
  EXPECT_EQ(s.used(), usedBefore);
  EXPECT_EQ(s.used() + s.remaining(), s.capacity());
}

// --- emplace_array ----------------------------------------------------------

TEST(Bump, EmplaceArraySize) {
  Bump s(256);
  auto sp = s.emplace_array<int>(8);
  EXPECT_EQ(sp.size(), 8u);
}

TEST(Bump, EmplaceArrayZeroReturnsEmpty) {
  Bump s(256);
  auto sp = s.emplace_array<int>(0);
  EXPECT_TRUE(sp.empty());
  EXPECT_EQ(s.used(), 0u);
}

TEST(Bump, EmplaceArrayDefaultConstructed) {
  Bump s(256);
  auto sp = s.emplace_array<int>(4);
  for (int v : sp)
    EXPECT_EQ(v, 0);
}

TEST(Bump, EmplaceArrayAlignment) {
  Bump s(512);
  (void)s.emplace<char>(); // nudge cursor
  auto sp = s.emplace_array<double>(2);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(sp.data()) % alignof(double), 0u);
}

TEST(Bump, EmplaceArrayExhaustionThrows) {
  Bump s(4);
  EXPECT_THROW((void)s.emplace_array<int>(8), std::bad_alloc);
}

TEST(Bump, EmplaceArrayOverflowThrows) {
  Bump s(256);
  std::size_t oversized = std::numeric_limits<std::size_t>::max() / sizeof(int) + 1;
  EXPECT_THROW((void)s.emplace_array<int>(oversized), std::bad_alloc);
}

TEST(Bump, EmplaceArrayConstructorFailureRollsBackCursor) {
  Bump s(256);
  const std::size_t usedBefore = s.used();
  ThrowsOnDefaultConstruct::attempts = 0;

  EXPECT_THROW((void)s.emplace_array<ThrowsOnDefaultConstruct>(4), std::runtime_error);
  EXPECT_EQ(ThrowsOnDefaultConstruct::attempts, 1);
  EXPECT_EQ(s.used(), usedBefore);
  EXPECT_EQ(s.used() + s.remaining(), s.capacity());
}

} // namespace baa
