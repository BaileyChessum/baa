find_package(benchmark REQUIRED)

add_executable(baa_benchmarks
  bench_bump_allocator.cpp
)

target_link_libraries(baa_benchmarks PRIVATE baa::baa benchmark::benchmark_main)
