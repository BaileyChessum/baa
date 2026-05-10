#include <baa/Arena.hpp>
#include <baa/ArenaAllocator.hpp>
#include <baa/FixedArena.hpp>
#include <baa/FixedArenaAllocator.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace baa {
namespace {

struct SmallRecord {
  std::uint32_t id;
  std::uint32_t tag;
  std::uint64_t payload;
};

struct MediumRecord {
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t c;
  std::uint64_t d;
};

struct Node8 {
  std::uint64_t value;
};

struct Node16 {
  std::uint64_t a;
  std::uint64_t b;
};

struct Node24 {
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t c;
};

struct Node32 {
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t c;
  std::uint64_t d;
};

struct Node48 {
  std::uint64_t data[6];
};

struct Node64 {
  std::uint64_t data[8];
};

// Trivially destructible — emplace takes the fast path (no destructor node).
struct TrivialRecord {
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t c;
};

// Non-trivially destructible — emplace allocates an inline DestructorNode.
struct NonTrivialRecord {
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t c;
  ~NonTrivialRecord() noexcept {}
};

constexpr std::array<unsigned, 64> kParserPattern = {
    8,  16, 24, 32, 48, 64, 16, 24, 8,  32, 48, 24, 16, 64, 8,  32, 24, 48, 16, 8,  64, 32,
    24, 16, 48, 8,  32, 64, 16, 24, 48, 8,  8,  24, 48, 16, 32, 64, 24, 8,  16, 48, 32, 64,
    24, 8,  16, 32, 48, 64, 24, 16, 8,  32, 48, 24, 16, 64, 8,  32, 24, 48, 16, 8,
};

constexpr std::size_t parserWorkingSetBytes(std::size_t depth) {
  std::size_t total = 0;
  for (unsigned size : kParserPattern)
    total += size;
  return total * depth;
}

// ============================================================
// Raw-allocation parser scratch (no construction, no destructor tracking)
// ============================================================

template <typename ArenaT, template <typename> class AllocT>
std::size_t parserStepRaw(ArenaT& arena, unsigned sizeClass) {
  switch (sizeClass) {
  case 8: {
    AllocT<Node8> a(arena);
    return reinterpret_cast<std::uintptr_t>(a.allocate(1));
  }
  case 16: {
    AllocT<Node16> a(arena);
    return reinterpret_cast<std::uintptr_t>(a.allocate(1));
  }
  case 24: {
    AllocT<Node24> a(arena);
    return reinterpret_cast<std::uintptr_t>(a.allocate(1));
  }
  case 32: {
    AllocT<Node32> a(arena);
    return reinterpret_cast<std::uintptr_t>(a.allocate(1));
  }
  case 48: {
    AllocT<Node48> a(arena);
    return reinterpret_cast<std::uintptr_t>(a.allocate(1));
  }
  case 64: {
    AllocT<Node64> a(arena);
    return reinterpret_cast<std::uintptr_t>(a.allocate(1));
  }
  default:
    return 0;
  }
}

template <typename ArenaT, template <typename> class AllocT>
static void BM_ParserScratch_RawAlloc_Impl(benchmark::State& state) {
  const bool useUnsafeRestore = state.range(0) != 0;
  constexpr std::size_t kDepth = 32;
  ArenaT arena(parserWorkingSetBytes(kDepth) + 4096);

  for (auto _ : state) {
    benchmark::DoNotOptimize(arena.remaining());
    for (std::size_t outer = 0; outer < kDepth; ++outer) {
      auto mark = arena.mark();
      std::size_t checksum = outer;
      for (unsigned sizeClass : kParserPattern)
        checksum += parserStepRaw<ArenaT, AllocT>(arena, sizeClass);
      benchmark::DoNotOptimize(checksum);
      benchmark::ClobberMemory();
      if (useUnsafeRestore) {
        arena.restore_unsafe(mark);
        benchmark::DoNotOptimize(arena.remaining());
      }
      else {
        benchmark::DoNotOptimize(arena.restore(mark));
      }
    }
    arena.reset();
    benchmark::ClobberMemory();
  }

  state.SetLabel(useUnsafeRestore ? "restore_unsafe" : "restore_checked");
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kDepth * kParserPattern.size()));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(parserWorkingSetBytes(kDepth)));
}

static void BM_ParserScratch_RawAlloc_Arena(benchmark::State& state) {
  BM_ParserScratch_RawAlloc_Impl<Arena, ArenaAllocator>(state);
}
static void BM_ParserScratch_RawAlloc_FixedArena(benchmark::State& state) {
  BM_ParserScratch_RawAlloc_Impl<FixedArena, FixedArenaAllocator>(state);
}

BENCHMARK(BM_ParserScratch_RawAlloc_Arena)->Arg(0)->Arg(1);
BENCHMARK(BM_ParserScratch_RawAlloc_FixedArena)->Arg(0)->Arg(1);

// ============================================================
// Emplace parser scratch: trivial vs non-trivial
// Measures the overhead of allocating inline DestructorNodes and
// running destructor chains on restore.
// ============================================================

template <typename T, typename ArenaT>
static void BM_ParserScratch_Emplace_Impl(benchmark::State& state) {
  const bool useUnsafeRestore = state.range(0) != 0;
  constexpr std::size_t kDepth = 32;
  constexpr std::size_t kPerDepth = kParserPattern.size(); // 64
  // Budget sizeof(T) + 40 bytes per slot to cover DestructorNode (32 bytes) and alignment slack.
  // Since we restore between depth levels the arena only needs to hold one level at a time.
  constexpr std::size_t kBytesPerSlot = sizeof(T) + 40;
  ArenaT arena(kPerDepth * kBytesPerSlot + 4096);

  for (auto _ : state) {
    benchmark::DoNotOptimize(arena.remaining());
    for (std::size_t outer = 0; outer < kDepth; ++outer) {
      auto mark = arena.mark();
      std::size_t checksum = outer;
      for (std::size_t i = 0; i < kPerDepth; ++i)
        checksum += reinterpret_cast<std::uintptr_t>(&arena.template emplace<T>());
      benchmark::DoNotOptimize(checksum);
      benchmark::ClobberMemory();
      if (useUnsafeRestore) {
        arena.restore_unsafe(mark);
        benchmark::DoNotOptimize(arena.remaining());
      }
      else {
        benchmark::DoNotOptimize(arena.restore(mark));
      }
    }
    arena.reset();
    benchmark::ClobberMemory();
  }

  state.SetLabel(useUnsafeRestore ? "restore_unsafe" : "restore_checked");
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kDepth * kPerDepth));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kDepth * kPerDepth * sizeof(T)));
}

static void BM_EmplaceTrivial_Arena(benchmark::State& state) {
  BM_ParserScratch_Emplace_Impl<TrivialRecord, Arena>(state);
}
static void BM_EmplaceTrivial_FixedArena(benchmark::State& state) {
  BM_ParserScratch_Emplace_Impl<TrivialRecord, FixedArena>(state);
}
static void BM_EmplaceNonTrivial_Arena(benchmark::State& state) {
  BM_ParserScratch_Emplace_Impl<NonTrivialRecord, Arena>(state);
}
static void BM_EmplaceNonTrivial_FixedArena(benchmark::State& state) {
  BM_ParserScratch_Emplace_Impl<NonTrivialRecord, FixedArena>(state);
}

BENCHMARK(BM_EmplaceTrivial_Arena)->Arg(0)->Arg(1);
BENCHMARK(BM_EmplaceTrivial_FixedArena)->Arg(0)->Arg(1);
BENCHMARK(BM_EmplaceNonTrivial_Arena)->Arg(0)->Arg(1);
BENCHMARK(BM_EmplaceNonTrivial_FixedArena)->Arg(0)->Arg(1);

// ============================================================
// Vector growth via allocator adapter
// ============================================================

template <typename T>
T makeValue(std::size_t /*i*/) {
  return T{};
}

template <>
int makeValue<int>(std::size_t i) {
  return static_cast<int>(i);
}

template <>
SmallRecord makeValue<SmallRecord>(std::size_t i) {
  return SmallRecord{static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i ^ 0x55u), i};
}

template <>
MediumRecord makeValue<MediumRecord>(std::size_t i) {
  return MediumRecord{i, i + 1u, i + 2u, i + 3u};
}

template <typename T, typename AllocT>
void fillVector(std::vector<T, AllocT>& values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i)
    values.push_back(makeValue<T>(i));
}

template <typename T, template <typename> class AllocT, typename ArenaT>
static void BM_VectorGrowth_Impl(benchmark::State& state) {
  constexpr std::size_t kVectorCount = 48;
  const std::size_t elementCount = static_cast<std::size_t>(state.range(0));
  const std::size_t bytesPerVector = sizeof(T) * elementCount;
  ArenaT arena(kVectorCount * bytesPerVector * 2 + 4096);

  for (auto _ : state) {
    AllocT<T> alloc(arena);
    std::size_t checksum = 0;

    for (std::size_t i = 0; i < kVectorCount; ++i) {
      std::vector<T, AllocT<T>> values(alloc);
      values.reserve(elementCount);
      fillVector(values, elementCount);
      checksum += values.size();
      benchmark::DoNotOptimize(values.data());
      benchmark::ClobberMemory();
    }

    benchmark::DoNotOptimize(checksum);
    arena.reset();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kVectorCount * elementCount));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kVectorCount * elementCount * sizeof(T)));
}

template <typename T>
static void BM_VectorGrowth_Arena(benchmark::State& state) {
  BM_VectorGrowth_Impl<T, ArenaAllocator, Arena>(state);
}

template <typename T>
static void BM_VectorGrowth_FixedArena(benchmark::State& state) {
  BM_VectorGrowth_Impl<T, FixedArenaAllocator, FixedArena>(state);
}

BENCHMARK_TEMPLATE(BM_VectorGrowth_Arena, int)->Arg(64)->Arg(256)->Arg(1024);
BENCHMARK_TEMPLATE(BM_VectorGrowth_Arena, SmallRecord)->Arg(64)->Arg(256)->Arg(1024);
BENCHMARK_TEMPLATE(BM_VectorGrowth_Arena, MediumRecord)->Arg(64)->Arg(256)->Arg(1024);
BENCHMARK_TEMPLATE(BM_VectorGrowth_FixedArena, int)->Arg(64)->Arg(256)->Arg(1024);
BENCHMARK_TEMPLATE(BM_VectorGrowth_FixedArena, SmallRecord)->Arg(64)->Arg(256)->Arg(1024);
BENCHMARK_TEMPLATE(BM_VectorGrowth_FixedArena, MediumRecord)->Arg(64)->Arg(256)->Arg(1024);

// ============================================================
// Arena page growth (Arena-only)
// Measures the cost of ::operator new + page-header setup as the arena
// grows geometrically from a tiny initial page.
// ============================================================

static void BM_ArenaPageGrowth(benchmark::State& state) {
  constexpr std::size_t kTotalBytes = 64 * 1024; // 64 KiB of Node32 allocations per iteration
  constexpr std::size_t kInitialPage = 256;      // tiny first page to force early growth
  Arena arena(kInitialPage);

  for (auto _ : state) {
    std::size_t checksum = 0;
    std::size_t allocated = 0;
    while (allocated + sizeof(Node32) <= kTotalBytes) {
      checksum += reinterpret_cast<std::uintptr_t>(&arena.emplace<Node32>());
      allocated += sizeof(Node32);
    }
    benchmark::DoNotOptimize(checksum);
    benchmark::ClobberMemory();
    arena.reset(); // deallocates all pages beyond the first, resets cursor
  }

  const std::size_t nodesPerIteration = kTotalBytes / sizeof(Node32);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(nodesPerIteration));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kTotalBytes));
}

BENCHMARK(BM_ArenaPageGrowth);

} // namespace
} // namespace baa
