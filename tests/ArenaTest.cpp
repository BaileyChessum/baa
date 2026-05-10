#include <baa/Arena.hpp>
#include <baa/ArenaAllocator.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
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
  inline static std::array<int, 32> order{};
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

TEST(Arena, InitialState) {
  const Arena arena(256);
  EXPECT_EQ(arena.remaining(), 256u);
}

TEST(Arena, ZeroInitialCapacity) {
  const Arena arena(0);
  EXPECT_EQ(arena.remaining(), 0u);
}

TEST(Arena, GrowsAcrossPages) {
  Arena arena(32);
  ArenaAllocator<int> alloc(arena);

  int* p = alloc.allocate(16);
  ASSERT_NE(p, nullptr);
}

TEST(Arena, OversizedSingleRequestAllocatesLargeEnoughPage) {
  Arena arena(16);
  ArenaAllocator<int> alloc(arena);

  int* values = alloc.allocate(64);
  ASSERT_NE(values, nullptr);
}

TEST(Arena, ResetDestroysOwnedObjectsAndDropsExtraPages) {
  LifetimeTracker::reset();

  Arena arena(32);
  (void)arena.emplace<LifetimeTracker>(1);
  (void)arena.emplace_array<LifetimeTracker>(4);
  arena.reset();
  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 5);
  EXPECT_EQ(arena.remaining(), 32u);
}

TEST(Arena, RestoreDestroysOwnedObjectsAcrossPages) {
  LifetimeTracker::reset();

  Arena arena(64);
  (void)arena.emplace<LifetimeTracker>(1);
  ArenaMarker marker = arena.mark();
  (void)arena.emplace_array<LifetimeTracker>(4);

  EXPECT_TRUE(arena.restore(marker));
  EXPECT_EQ(LifetimeTracker::alive, 1);
  EXPECT_EQ(LifetimeTracker::destroyed, 4);
}

TEST(Arena, RestoreUnsafeDestroysOwnedObjectsAcrossPages) {
  LifetimeTracker::reset();

  Arena arena(64);
  (void)arena.emplace<LifetimeTracker>(1);
  const ArenaMarker marker = arena.mark();
  (void)arena.emplace_array<LifetimeTracker>(4);

  arena.restore_unsafe(marker);
  EXPECT_EQ(LifetimeTracker::alive, 1);
  EXPECT_EQ(LifetimeTracker::destroyed, 4);
}

TEST(Arena, ResetInvalidatesMarkers) {
  LifetimeTracker::reset();

  Arena arena(64);
  (void)arena.emplace<LifetimeTracker>(1);
  const ArenaMarker marker = arena.mark();
  (void)arena.emplace_array<LifetimeTracker>(3);

  arena.reset();
  EXPECT_FALSE(arena.restore(marker));
  EXPECT_EQ(LifetimeTracker::alive, 0);
}

TEST(Arena, RejectsForeignMarkers) {
  Arena a(64);
  Arena b(64);
  ArenaMarker marker = a.mark();

  EXPECT_FALSE(b.restore(marker));
}

TEST(Arena, CheckpointDestructorRollsBackOnScopeExit) {
  LifetimeTracker::reset();

  Arena arena(64);
  {
    auto checkpoint = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(1);
    (void)arena.emplace_array<LifetimeTracker>(3);
    EXPECT_EQ(LifetimeTracker::alive, 4);
  }

  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 4);
}

TEST(Arena, CheckpointDestructorRollsBackOnExceptionExit) {
  LifetimeTracker::reset();

  Arena arena(64);
  try {
    auto checkpoint = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(1);
    (void)arena.emplace_array<LifetimeTracker>(3);
    throw std::runtime_error("boom");
  }
  catch (const std::runtime_error&) {
  }

  EXPECT_EQ(LifetimeTracker::alive, 0);
}

TEST(Arena, CheckpointReleaseKeepsAllocationsAndOwnedObjects) {
  LifetimeTracker::reset();

  Arena arena(64);
  {
    auto checkpoint = arena.checkpoint();
    (void)arena.emplace<LifetimeTracker>(1);
    (void)arena.emplace_array<LifetimeTracker>(3);
    checkpoint.release();
    EXPECT_FALSE(checkpoint.active());
  }

  EXPECT_EQ(LifetimeTracker::alive, 4);
  EXPECT_EQ(LifetimeTracker::destroyed, 0);
  arena.reset();
}

TEST(Arena, CheckpointRollbackRestoresImmediatelyAndDestroysOwnedObjects) {
  LifetimeTracker::reset();

  Arena arena(64);
  auto checkpoint = arena.checkpoint();
  (void)arena.emplace<LifetimeTracker>(1);
  (void)arena.emplace_array<LifetimeTracker>(3);

  checkpoint.rollback();
  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 4);
}

TEST(Arena, CheckpointReleaseIsIdempotent) {
  LifetimeTracker::reset();

  Arena arena(64);
  auto checkpoint = arena.checkpoint();
  (void)arena.emplace<LifetimeTracker>(1);
  checkpoint.release();
  checkpoint.release();

  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(LifetimeTracker::alive, 1);
  arena.reset();
}

TEST(Arena, CheckpointRollbackIsIdempotent) {
  LifetimeTracker::reset();

  Arena arena(64);
  auto checkpoint = arena.checkpoint();
  (void)arena.emplace<LifetimeTracker>(1);
  checkpoint.rollback();
  checkpoint.rollback();

  EXPECT_FALSE(checkpoint.active());
  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 1);
}

TEST(Arena, CheckpointMoveTransfersResponsibility) {
  LifetimeTracker::reset();

  Arena arena(64);
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

TEST(Arena, MovedFromCheckpointIsInactive) {
  Arena arena(64);
  auto original = arena.checkpoint();
  auto moved = std::move(original);

  EXPECT_FALSE(original.active());
  EXPECT_TRUE(moved.active());
}

TEST(Arena, NestedCheckpointsUnwindInLifoOrderAcrossPages) {
  LifetimeTracker::reset();

  Arena arena(64);
  auto outer = arena.checkpoint();
  (void)arena.emplace<LifetimeTracker>(1);

  {
    auto inner = arena.checkpoint();
    (void)arena.emplace_array<LifetimeTracker>(4);
  }

  EXPECT_EQ(LifetimeTracker::alive, 1);
  EXPECT_EQ(LifetimeTracker::destroyed, 4);
  outer.rollback();
  EXPECT_EQ(LifetimeTracker::alive, 0);
  EXPECT_EQ(LifetimeTracker::destroyed, 5);
}

TEST(Arena, EmplaceConstructorFailureRollsBackState) {
  Arena arena(64);
  const std::size_t remaining_before = arena.remaining();
  ThrowsOnConstruct::attempts = 0;

  EXPECT_THROW((void)arena.emplace<ThrowsOnConstruct>(), std::runtime_error);
  EXPECT_EQ(ThrowsOnConstruct::attempts, 1);
  EXPECT_EQ(arena.remaining(), remaining_before);
}

TEST(Arena, EmplaceArrayConstructorFailureRollsBackStateAndDestroysPrefix) {
  Arena arena(128);
  const std::size_t remaining_before = arena.remaining();
  ThrowsOnThirdDefault::attempts = 0;
  ThrowsOnThirdDefault::destroyed = 0;

  EXPECT_THROW((void)arena.emplace_array<ThrowsOnThirdDefault>(4), std::runtime_error);
  EXPECT_EQ(ThrowsOnThirdDefault::attempts, 3);
  EXPECT_EQ(ThrowsOnThirdDefault::destroyed, 2);
  EXPECT_EQ(arena.remaining(), remaining_before);
}

TEST(Arena, MarkerSurvivesMoveConstruction) {
  LifetimeTracker::reset();

  Arena a(64);
  (void)a.emplace<LifetimeTracker>(1);
  ArenaMarker marker = a.mark();
  (void)a.emplace_array<LifetimeTracker>(4);

  Arena b(std::move(a));
  EXPECT_TRUE(b.restore(marker));
  EXPECT_EQ(LifetimeTracker::alive, 1);
}

TEST(Arena, AllocatorSupportsNonTrivialTypes) {
  Arena arena(128);
  ArenaAllocator<std::string> alloc(arena);
  std::vector<std::string, ArenaAllocator<std::string>> values(alloc);

  values.reserve(8);
  values.emplace_back("alpha");
  values.emplace_back("beta");

  EXPECT_EQ(values[0], "alpha");
  EXPECT_EQ(values[1], "beta");
}

TEST(ArenaAllocator, AllocateSingle) {
  Arena arena(64);
  ArenaAllocator<int> alloc(arena);
  int* p = alloc.allocate(1);
  ASSERT_NE(p, nullptr);
  *p = 42;
  EXPECT_EQ(*p, 42);
}

TEST(ArenaAllocator, AllocateArray) {
  Arena arena(64);
  ArenaAllocator<int> alloc(arena);
  int* p = alloc.allocate(8);
  for (int i = 0; i < 8; ++i)
    p[i] = i;
  for (int i = 0; i < 8; ++i)
    EXPECT_EQ(p[i], i);
}

TEST(ArenaAllocator, SequentialAllocationsDoNotOverlap) {
  Arena arena(64);
  ArenaAllocator<int> alloc(arena);
  int* a = alloc.allocate(1);
  int* b = alloc.allocate(1);

  EXPECT_NE(a, b);
  EXPECT_GE(reinterpret_cast<std::byte*>(b) - reinterpret_cast<std::byte*>(a),
            static_cast<std::ptrdiff_t>(sizeof(int)));
}

TEST(ArenaAllocator, RespectsAlignment) {
  Arena arena(64);
  ArenaAllocator<char> chars(arena);
  ArenaAllocator<double> doubles(arena);

  (void)chars.allocate(1);
  double* value = doubles.allocate(1);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(value) % alignof(double), 0u);
}

TEST(ArenaAllocator, GrowsInsteadOfExhausting) {
  Arena arena(sizeof(int));
  ArenaAllocator<int> alloc(arena);

  (void)alloc.allocate(1);
  EXPECT_NO_THROW((void)alloc.allocate(1));
}

TEST(ArenaAllocator, OversizedAllocationThrowsBadAlloc) {
  Arena arena(64);
  ArenaAllocator<int> alloc(arena);
  const std::size_t oversized = std::numeric_limits<std::size_t>::max() / sizeof(int) + 1;
  EXPECT_THROW((void)alloc.allocate(oversized), std::bad_alloc);
}

TEST(ArenaAllocator, DeallocateDoesNothing) {
  Arena arena(64);
  ArenaAllocator<int> alloc(arena);
  int* p = alloc.allocate(1);
  const std::size_t remaining_before = arena.remaining();
  alloc.deallocate(p, 1);
  EXPECT_EQ(arena.remaining(), remaining_before);
}

TEST(ArenaAllocator, EqualWhenSameArena) {
  Arena arena(64);
  ArenaAllocator<int> a(arena);
  ArenaAllocator<float> b(arena);
  EXPECT_TRUE(a == b);
}

TEST(ArenaAllocator, NotEqualWhenDifferentArena) {
  Arena a(64);
  Arena b(64);
  ArenaAllocator<int> x(a);
  ArenaAllocator<int> y(b);
  EXPECT_FALSE(x == y);
}

TEST(ArenaAllocator, StdVectorSpansMultiplePages) {
  Arena arena(64);
  ArenaAllocator<int> alloc(arena);
  std::vector<int, ArenaAllocator<int>> values(alloc);

  values.reserve(128);
  for (int i = 0; i < 128; ++i)
    values.push_back(i);

  for (std::size_t i = 0; i < values.size(); ++i)
    EXPECT_EQ(values[i], static_cast<int>(i));
}

} // namespace baa
