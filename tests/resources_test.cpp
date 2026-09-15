#include "mini_cloud/resources.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void expect(const bool condition, const char* description) {
  if (!condition) {
    std::cerr << "FAILED: " << description << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  using mini_cloud::Admission;
  using mini_cloud::Resources;
  using mini_cloud::WorkerResources;

  const WorkerResources worker{{2000, 4096}, {500, 1024}};
  expect(mini_cloud::is_valid_reservation(worker), "valid reservation is accepted");
  expect(mini_cloud::evaluate_admission(worker, {1500, 3000}) == Admission::accepted,
         "request exactly fitting remaining capacity is accepted");
  expect(mini_cloud::evaluate_admission(worker, {1501, 3000}) == Admission::insufficient_cpu,
         "CPU over-capacity is rejected");
  expect(mini_cloud::evaluate_admission(worker, {1500, 3073}) == Admission::insufficient_memory,
         "memory over-capacity is rejected");
  expect(mini_cloud::evaluate_admission(worker, {1501, 3073}) == Admission::insufficient_cpu_and_memory,
         "dual over-capacity is rejected");

  expect(!mini_cloud::is_valid_reservation({{2000, 4096}, {2001, 0}}),
         "CPU reservation cannot exceed capacity");
  expect(!mini_cloud::is_valid_reservation({{2000, 4096}, {0, 4097}}),
         "memory reservation cannot exceed capacity");
  expect(!mini_cloud::is_valid_reservation({{0, 4096}, {0, 0}}),
         "zero CPU capacity is invalid");
  expect(!mini_cloud::is_valid_reservation({{2000, 0}, {0, 0}}),
         "zero memory capacity is invalid");
  expect(!mini_cloud::checked_add({std::numeric_limits<std::uint64_t>::max(), 1}, {1, 0}),
         "CPU addition overflow is rejected");
  expect(!mini_cloud::checked_add({1, std::numeric_limits<std::uint64_t>::max()}, {0, 1}),
         "memory addition overflow is rejected");
  expect(mini_cloud::evaluate_admission({{100, 100}, {90, 90}},
                                         {std::numeric_limits<std::uint64_t>::max(), 1}) ==
             Admission::insufficient_cpu_and_memory,
         "overflowing projection is rejected without wrapping");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
