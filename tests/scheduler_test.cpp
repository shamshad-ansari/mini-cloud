#include "mini_cloud/scheduler.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const char* description) {
  if (!condition) {
    std::cerr << "FAILED: " << description << '\n';
    ++failures;
  }
}

void expect_selection(const mini_cloud::SchedulingPolicy policy,
                      const std::vector<mini_cloud::Worker>& workers,
                      const mini_cloud::Resources request, const std::size_t expected,
                      const char* description) {
  const auto selected = mini_cloud::choose_worker(policy, workers, request);
  expect(selected && *selected == expected, description);
}

}  // namespace

int main() {
  using mini_cloud::Resources;
  using mini_cloud::SchedulingPolicy;
  using mini_cloud::Worker;

  expect(mini_cloud::parse_scheduling_policy("first-fit") == SchedulingPolicy::first_fit,
         "first-fit policy parses");
  expect(mini_cloud::parse_scheduling_policy("least-loaded-dominant-resource") ==
             SchedulingPolicy::least_loaded_dominant_resource,
         "dominant-resource policy parses");
  expect(!mini_cloud::parse_scheduling_policy("random"), "unknown policy is rejected");

  const std::vector<Worker> ordered_workers{
      {"first", {{2000, 4096}, {1000, 1024}}},
      {"second", {{2000, 4096}, {0, 0}}},
  };
  expect_selection(SchedulingPolicy::first_fit, ordered_workers, {500, 512}, 0,
                   "first-fit keeps configured worker order");

  const std::vector<Worker> cpu_heavy_workers{
      {"cpu-busy", {{4000, 4000}, {3000, 0}}},
      {"memory-busy", {{4000, 4000}, {0, 1000}}},
  };
  expect_selection(SchedulingPolicy::least_loaded_dominant_resource, cpu_heavy_workers,
                   {1000, 100}, 1, "CPU-heavy request prefers lower projected CPU load");

  const std::vector<Worker> memory_heavy_workers{
      {"cpu-busy", {{4000, 4000}, {1000, 0}}},
      {"memory-busy", {{4000, 4000}, {0, 3000}}},
  };
  expect_selection(SchedulingPolicy::least_loaded_dominant_resource, memory_heavy_workers,
                   {100, 1000}, 0, "memory-heavy request prefers lower projected memory load");

  const std::vector<Worker> balanced_workers{
      {"less-loaded", {{4000, 4000}, {1000, 1000}}},
      {"more-loaded", {{4000, 4000}, {2000, 2000}}},
  };
  expect_selection(SchedulingPolicy::least_loaded_dominant_resource, balanced_workers,
                   {1000, 1000}, 0, "balanced request uses projected dominant utilization");

  const std::vector<Worker> tied_workers{
      {"zeta", {{4000, 4000}, {1000, 1000}}},
      {"alpha", {{4000, 4000}, {1000, 1000}}},
  };
  expect_selection(SchedulingPolicy::least_loaded_dominant_resource, tied_workers,
                   {1000, 1000}, 1, "dominant-resource ties use worker ID rather than input order");

  const std::vector<Worker> infeasible_workers{
      {"cpu-short", {{1000, 4000}, {500, 0}}},
      {"memory-short", {{4000, 1000}, {0, 500}}},
  };
  expect(!mini_cloud::choose_worker(SchedulingPolicy::first_fit, infeasible_workers, {1000, 1000}),
         "no worker is selected unless CPU and memory both fit");
  const auto deficit = mini_cloud::capacity_deficit(infeasible_workers[0].resources, {1000, 1000});
  expect(deficit.cpu_millicores == 500 && deficit.memory_mib == 0,
         "infeasible worker reports its CPU capacity deficit");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
