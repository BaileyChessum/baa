#include <baa/FixedArena.hpp>
#include <baa/FixedArenaAllocator.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace baa {

namespace {

struct LifetimeTracker {
  inline static int alive = 0;
  inline static int destroyed = 0;
  inline static std::array<int, 16> order{};
  inline static std::size_t order_size = 0;

  int id;

  explicit LifetimeTracker(int value = 0) : id(value) { ++alive; }

  ~LifetimeTracker() {
    order[order_size++] = id;
    ++destroyed;
    --alive;
  }

  static void reset() {
    alive = 0;
    destroyed = 0;
    order.fill(-1);
    order_size = 0;
  }
};

struct ThrowsOnConstruct {
  inline static int attempts = 0;

  ThrowsOnConstruct() {
    ++attempts;
    throw std::runtime_error("boom");
  }
};

struct ThrowsOnThirdDefault {
  inline static int attempts = 0;
  inline static int destroyed = 0;

  ThrowsOnThirdDefault() {
    ++attempts;
    if (attempts == 3)
      throw std::runtime_error("boom");
  }

  ~ThrowsOnThirdDefault() { ++destroyed; }
};

static_assert(!std::is_trivially_destructible_v<LifetimeTracker>);
static_assert(!std::is_trivially_destructible_v<ThrowsOnThirdDefault>);

} // namespace

TEST(FixedArena, InitialState) {
  FixedArena arena(256);
  EXPECT_EQ(arena.capacity(), 256u);
  EXPECT_EQ(arena.used(), 0u);
  EXPECT_EQ(arena.remaining(), 256u);
}

TEST(FixedArena, ResetDestroysOwnedObjects) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  (void)arena.emplace<LifetimeTracker>(1);
  (void)arena.emplace<LifetimeTracker>(2);

  EXPECT_EQ(LifetimeTracker::alive, 2);
  arena.reset();
  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 2);
  EXPECT_EQ(LifetimeTracker::order[0], 2);
  EXPECT_EQ(LifetimeTracker::order[1], 1);
}

TEST(FixedArena, RestoreDestroysOwnedObjectsAfterMark) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  (void)arena.emplace<LifetimeTracker>(1);
  FixedArenaMarker mark = arena.mark();
  (void)arena.emplace<LifetimeTracker>(2);
  (void)arena.emplace<LifetimeTracker>(3);

  EXPECT_TRUE(arena.restore(mark));
  EXPECT_EQ(LifetimeTracker::alive, 1);
  EXPECT_EQ(LifetimeTracker::destroyed, 2);
  EXPECT_EQ(LifetimeTracker::order[0], 3);
  EXPECT_EQ(LifetimeTracker::order[1], 2);
}

TEST(FixedArena, RestoreUnsafeDestroysOwnedObjectsAfterMark) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  (void)arena.emplace<LifetimeTracker>(1);
  FixedArenaMarker mark = arena.mark();
  (void)arena.emplace<LifetimeTracker>(2);

  arena.restore_unsafe(mark);
  EXPECT_EQ(LifetimeTracker::alive, 1);
  EXPECT_EQ(LifetimeTracker::destroyed, 1);
  EXPECT_EQ(LifetimeTracker::order[0], 2);
}

TEST(FixedArena, ResetInvalidatesTrackedMarks) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  (void)arena.emplace<LifetimeTracker>(1);
  FixedArenaMarker mark = arena.mark();
  (void)arena.emplace<LifetimeTracker>(2);

  arena.reset();
  EXPECT_FALSE(arena.restore(mark));
  EXPECT_EQ(LifetimeTracker::alive, 0);
}

TEST(FixedArena, RejectsForeignMarks) {
  FixedArena a(256);
  FixedArena b(256);
  FixedArenaMarker mark = a.mark();

  EXPECT_FALSE(b.restore(mark));
  EXPECT_EQ(b.used(), 0u);
  EXPECT_EQ(b.used() + b.remaining(), b.capacity());
}

TEST(FixedArena, CheckpointDestructorRollsBackOnScopeExit) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  {
    auto checkpoint = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(1);
    (void)arena.emplace<LifetimeTracker>(2);
    EXPECT_EQ(LifetimeTracker::alive, 2);
  }

  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 2);
  EXPECT_EQ(LifetimeTracker::order[0], 2);
  EXPECT_EQ(LifetimeTracker::order[1], 1);
}

TEST(FixedArena, CheckpointDestructorRollsBackOnExceptionExit) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  try {
    auto checkpoint = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(1);
    (void)arena.emplace<LifetimeTracker>(2);
    throw std::runtime_error("boom");
  }
  catch (const std::runtime_error&) {
  }

  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 2);
}

TEST(FixedArena, CheckpointReleaseKeepsAllocationsAndOwnedObjects) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  {
    auto checkpoint = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(1);
    (void)arena.emplace<LifetimeTracker>(2);
    checkpoint.release();
    EXPECT_FALSE(checkpoint.active());
  }

  EXPECT_EQ(LifetimeTracker::alive, 2);
  EXPECT_EQ(LifetimeTracker::destroyed, 0);
  arena.reset();
}

TEST(FixedArena, CheckpointRollbackRestoresImmediatelyAndDestroysOwnedObjects) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  auto checkpoint = arena.checkpoint();
  (void)arena.emplace<LifetimeTracker>(1);
  (void)arena.emplace<LifetimeTracker>(2);

  checkpoint.rollback();
  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 2);
}

TEST(FixedArena, CheckpointReleaseIsIdempotent) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  auto checkpoint = arena.checkpoint();
  (void)arena.emplace<LifetimeTracker>(1);
  checkpoint.release();
  checkpoint.release();

  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(LifetimeTracker::alive, 1);
  arena.reset();
}

TEST(FixedArena, CheckpointRollbackIsIdempotent) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  auto checkpoint = arena.checkpoint();
  (void)arena.emplace<LifetimeTracker>(1);
  checkpoint.rollback();
  checkpoint.rollback();

  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 1);
}

TEST(FixedArena, CheckpointMoveTransfersResponsibility) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  {
    auto original = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(1);
    auto moved = std::move(original);
    EXPECT_FALSE(original.active());
    EXPECT_TRUE(moved.active());
  }

  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 1);
}

TEST(FixedArena, MovedFromCheckpointIsInactive) {
  FixedArena arena(512);
  auto original = arena.checkpoint();
  auto moved = std::move(original);

  EXPECT_FALSE(original.active());
  EXPECT_TRUE(moved.active());
}

TEST(FixedArena, NestedCheckpointsUnwindInLifoOrder) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  auto outer = arena.checkpoint();
  (void)arena.emplace<LifetimeTracker>(1);

  {
    auto inner = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(2);
    (void)arena.emplace<LifetimeTracker>(3);
  }

  EXPECT_EQ(LifetimeTracker::alive, 1);
  EXPECT_EQ(LifetimeTracker::destroyed, 2);
  EXPECT_EQ(LifetimeTracker::order[0], 3);
  EXPECT_EQ(LifetimeTracker::order[1], 2);

  outer.rollback();
  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 3);
  EXPECT_EQ(LifetimeTracker::order[2], 1);
}

TEST(FixedArena, CheckpointRollbackOnExceptionDestroysNewlyOwnedObjects) {
  LifetimeTracker::reset();

  FixedArena arena(512);
  try {
    auto checkpoint = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(1);
    (void)arena.emplace<LifetimeTracker>(2);
    throw std::runtime_error("boom");
  }
  catch (const std::runtime_error&) {
  }

  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 2);
  EXPECT_EQ(LifetimeTracker::order[0], 2);
  EXPECT_EQ(LifetimeTracker::order[1], 1);
}

TEST(FixedArena, TrivialArraysDoNotNeedDestructorTracking) {
  FixedArena arena(256);
  auto values = arena.emplace_array<int>(8);
  ASSERT_EQ(values.size(), 8u);
  EXPECT_EQ(arena.used() + arena.remaining(), arena.capacity());
}

TEST(FixedArena, EmplaceConstructorFailureRollsBackCursor) {
  FixedArena arena(256);
  const std::size_t used_before = arena.used();
  ThrowsOnConstruct::attempts = 0;

  EXPECT_THROW((void)arena.emplace<ThrowsOnConstruct>(), std::runtime_error);
  EXPECT_EQ(ThrowsOnConstruct::attempts, 1);
  EXPECT_EQ(arena.used(), used_before);
  EXPECT_EQ(arena.used() + arena.remaining(), arena.capacity());
}

TEST(FixedArena, EmplaceArrayConstructorFailureRollsBackCursorAndDestroysConstructedPrefix) {
  FixedArena arena(512);
  const std::size_t used_before = arena.used();
  ThrowsOnThirdDefault::attempts = 0;
  ThrowsOnThirdDefault::destroyed = 0;

  EXPECT_THROW((void)arena.emplace_array<ThrowsOnThirdDefault>(4), std::runtime_error);
  EXPECT_EQ(ThrowsOnThirdDefault::attempts, 3);
  EXPECT_EQ(ThrowsOnThirdDefault::destroyed, 2);
  EXPECT_EQ(arena.used(), used_before);
  EXPECT_EQ(arena.used() + arena.remaining(), arena.capacity());
}

TEST(FixedArena, MarkSurvivesMoveConstruction) {
  LifetimeTracker::reset();

  FixedArena a(512);
  (void)a.emplace<LifetimeTracker>(1);
  FixedArenaMarker mark = a.mark();
  (void)a.emplace<LifetimeTracker>(2);

  FixedArena b(std::move(a));
  EXPECT_TRUE(b.restore(mark));
  EXPECT_EQ(LifetimeTracker::alive, 1);
}

TEST(FixedArena, AllocatorSupportsNonTrivialTypes) {
  FixedArena arena(4096);
  FixedArenaAllocator<std::string> alloc(arena);
  std::vector<std::string, FixedArenaAllocator<std::string>> values(alloc);

  values.reserve(8);
  values.emplace_back("alpha");
  values.emplace_back("beta");

  EXPECT_EQ(values[0], "alpha");
  EXPECT_EQ(values[1], "beta");
}

TEST(FixedArenaAllocator, OversizedAllocationThrowsBadAlloc) {
  FixedArena arena(256);
  FixedArenaAllocator<int> alloc(arena);
  const std::size_t oversized = std::numeric_limits<std::size_t>::max() / sizeof(int) + 1;
  EXPECT_THROW((void)alloc.allocate(oversized), std::bad_alloc);
}

TEST(FixedArenaAllocator, RespectsAlignment) {
  FixedArena arena(512);
  FixedArenaAllocator<char> chars(arena);
  FixedArenaAllocator<double> doubles(arena);

  (void)chars.allocate(1);
  double* value = doubles.allocate(1);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(value) % alignof(double), 0u);
}

} // namespace baa
