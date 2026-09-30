#include "mini_cloud/scheduler.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using mini_cloud::Resources;
using mini_cloud::Worker;
using mini_cloud::WorkerResources;

std::optional<std::uint64_t> parse_unsigned(const std::string_view value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
  return parsed;
}

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " <first-fit|least-loaded-dominant-resource>"
            << " <request-cpu-millicores> <request-memory-mib>"
            << " <worker-id> <capacity-cpu> <capacity-memory> <reserved-cpu> <reserved-memory>"
            << " [<worker-id> <capacity-cpu> <capacity-memory> <reserved-cpu> <reserved-memory> ...]\n";
}

bool contains_worker_id(const std::vector<Worker>& workers, const std::string_view id) {
  return std::any_of(workers.begin(), workers.end(), [id](const Worker& worker) {
    return worker.id == id;
  });
}

}  // namespace

int main(const int argc, char* argv[]) {
  constexpr int kFirstWorkerArgument = 4;
  constexpr int kWorkerArgumentCount = 5;
  if (argc < kFirstWorkerArgument + kWorkerArgumentCount ||
      (argc - kFirstWorkerArgument) % kWorkerArgumentCount != 0) {
    usage(argv[0]);
    return 2;
  }

  const auto policy = mini_cloud::parse_scheduling_policy(argv[1]);
  if (!policy) {
    std::cerr << "Unknown policy: " << argv[1] << '\n';
    usage(argv[0]);
    return 2;
  }
  const auto request_cpu = parse_unsigned(argv[2]);
  const auto request_memory = parse_unsigned(argv[3]);
  if (!request_cpu || !request_memory || !mini_cloud::is_valid_capacity({*request_cpu, *request_memory})) {
    std::cerr << "Invalid request: CPU millicores and memory MiB must both be positive unsigned integers.\n";
    return 2;
  }
  const Resources request{*request_cpu, *request_memory};

  std::vector<Worker> workers;
  workers.reserve(static_cast<std::size_t>((argc - kFirstWorkerArgument) / kWorkerArgumentCount));
  for (int index = kFirstWorkerArgument; index < argc; index += kWorkerArgumentCount) {
    const std::string_view id = argv[index];
    if (id.empty() || contains_worker_id(workers, id)) {
      std::cerr << "Worker IDs must be non-empty and unique: " << id << '\n';
      return 2;
    }
    std::optional<std::uint64_t> values[4];
    for (int offset = 0; offset < 4; ++offset) {
      values[offset] = parse_unsigned(argv[index + offset + 1]);
      if (!values[offset]) {
        std::cerr << "Invalid unsigned integer: " << argv[index + offset + 1] << '\n';
        return 2;
      }
    }
    Worker worker{std::string{id}, {{*values[0], *values[1]}, {*values[2], *values[3]}}};
    if (!mini_cloud::is_valid_reservation(worker.resources)) {
      std::cerr << "Invalid resources for worker " << worker.id
                << ": capacity must be positive and reservation must not exceed capacity.\n";
      return 2;
    }
    workers.push_back(std::move(worker));
  }

  const auto selected = mini_cloud::choose_worker(*policy, workers, request);
  if (selected) {
    std::cout << "selected worker=" << workers[*selected].id
              << " policy=" << mini_cloud::scheduling_policy_name(*policy) << '\n';
    return 0;
  }

  std::cout << "no placement policy=" << mini_cloud::scheduling_policy_name(*policy) << '\n';
  for (const auto& worker : workers) {
    const auto deficit = mini_cloud::capacity_deficit(worker.resources, request);
    std::cout << "capacity deficit worker=" << worker.id << " cpu=" << deficit.cpu_millicores
              << "m memory=" << deficit.memory_mib << "MiB\n";
  }
  return 1;
}
