#include <baa/Bump.hpp>
#include <baa/BumpAllocator.hpp>

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

template <typename T> void fillVector(std::vector<T, BumpAllocator<T>> &values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i)
    values.push_back(T{});
}

template <> void fillVector(std::vector<int, BumpAllocator<int>> &values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i)
    values.push_back(static_cast<int>(i));
}

template <> void fillVector(std::vector<SmallRecord, BumpAllocator<SmallRecord>> &values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i)
  {
    values.push_back(SmallRecord{static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i ^ 0x55u), i});
  }
}

template <> void fillVector(std::vector<MediumRecord, BumpAllocator<MediumRecord>> &values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i)
    values.push_back(MediumRecord{i, i + 1u, i + 2u, i + 3u});
}

std::size_t parserStep(Bump &bump, unsigned sizeClass) {
  switch (sizeClass)
  {
  case 8: {
    BumpAllocator<Node8> alloc(bump);
    return reinterpret_cast<std::uintptr_t>(alloc.allocate(1));
  }
  case 16: {
    BumpAllocator<Node16> alloc(bump);
    return reinterpret_cast<std::uintptr_t>(alloc.allocate(1));
  }
  case 24: {
    BumpAllocator<Node24> alloc(bump);
    return reinterpret_cast<std::uintptr_t>(alloc.allocate(1));
  }
  case 32: {
    BumpAllocator<Node32> alloc(bump);
    return reinterpret_cast<std::uintptr_t>(alloc.allocate(1));
  }
  case 48: {
    BumpAllocator<Node48> alloc(bump);
    return reinterpret_cast<std::uintptr_t>(alloc.allocate(1));
  }
  case 64: {
    BumpAllocator<Node64> alloc(bump);
    return reinterpret_cast<std::uintptr_t>(alloc.allocate(1));
  }
  default:
    return 0;
  }
}

static void BM_ParserStyleScratch(benchmark::State &state) {
  const bool useUnsafeRestore = state.range(0) != 0;
  constexpr std::size_t kDepth = 32;
  Bump bump(parserWorkingSetBytes(kDepth) + 4096);

  for (auto _ : state)
  {
    benchmark::DoNotOptimize(bump.remaining());
    for (std::size_t outer = 0; outer < kDepth; ++outer)
    {
      BumpMark mark = bump.mark();
      std::size_t checksum = outer;
      for (unsigned sizeClass : kParserPattern)
      {
        checksum += parserStep(bump, sizeClass);
      }
      benchmark::DoNotOptimize(checksum);
      benchmark::ClobberMemory();
      if (useUnsafeRestore)
      {
        bump.restore_unsafe(mark);
        benchmark::DoNotOptimize(bump.used());
      }
      else
      {
        benchmark::DoNotOptimize(bump.restore(mark));
      }
    }
    bump.reset();
    benchmark::ClobberMemory();
  }

  state.SetLabel(useUnsafeRestore ? "restore_unsafe" : "restore_checked");
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kDepth * kParserPattern.size()));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(parserWorkingSetBytes(kDepth)));
}

template <typename T> static void BM_VectorGrowth(benchmark::State &state) {
  constexpr std::size_t kVectorCount = 48;
  const std::size_t elementCount = static_cast<std::size_t>(state.range(0));
  const std::size_t bytesPerVector = sizeof(T) * elementCount;
  Bump bump(kVectorCount * bytesPerVector * 2 + 4096);

  for (auto _ : state)
  {
    BumpAllocator<T> alloc(bump);
    std::size_t checksum = 0;

    for (std::size_t i = 0; i < kVectorCount; ++i)
    {
      std::vector<T, BumpAllocator<T>> values(alloc);
      values.reserve(elementCount);
      fillVector(values, elementCount);
      checksum += values.size();
      benchmark::DoNotOptimize(values.data());
      benchmark::ClobberMemory();
    }

    benchmark::DoNotOptimize(checksum);
    bump.reset();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kVectorCount * elementCount));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(kVectorCount * elementCount * sizeof(T)));
}

BENCHMARK(BM_ParserStyleScratch)->Arg(0)->Arg(1);
BENCHMARK_TEMPLATE(BM_VectorGrowth, int)->Arg(64)->Arg(256)->Arg(1024);
BENCHMARK_TEMPLATE(BM_VectorGrowth, SmallRecord)->Arg(64)->Arg(256)->Arg(1024);
BENCHMARK_TEMPLATE(BM_VectorGrowth, MediumRecord)->Arg(64)->Arg(256)->Arg(1024);

} // namespace
} // namespace baa
