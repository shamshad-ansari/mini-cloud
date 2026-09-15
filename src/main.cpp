#include "mini_cloud/resources.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <system_error>
#include <string_view>

namespace {

using mini_cloud::Resources;
using mini_cloud::WorkerResources;

std::optional<std::uint64_t> parse_unsigned(const std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " <capacity-cpu-millicores> <capacity-memory-mib>"
            << " <reserved-cpu-millicores> <reserved-memory-mib>"
            << " <request-cpu-millicores> <request-memory-mib>\n";
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc != 7) {
    usage(argv[0]);
    return 2;
  }

  std::optional<std::uint64_t> values[6];
  for (int index = 0; index < 6; ++index) {
    values[index] = parse_unsigned(argv[index + 1]);
    if (!values[index]) {
      std::cerr << "Invalid unsigned integer: " << argv[index + 1] << '\n';
      return 2;
    }
  }

  const WorkerResources worker{{*values[0], *values[1]}, {*values[2], *values[3]}};
  const Resources request{*values[4], *values[5]};
  if (!mini_cloud::is_valid_reservation(worker)) {
    std::cerr << "Invalid worker resources: capacity must be positive and reservation must not exceed capacity.\n";
    return 2;
  }
  if (!mini_cloud::is_valid_capacity(request)) {
    std::cerr << "Invalid request: CPU millicores and memory MiB must both be positive.\n";
    return 2;
  }

  const auto admission = mini_cloud::evaluate_admission(worker, request);
  std::cout << mini_cloud::admission_name(admission) << '\n';
  return admission == mini_cloud::Admission::accepted ? 0 : 1;
}
