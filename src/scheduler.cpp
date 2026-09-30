#include "mini_cloud/scheduler.hpp"

#include <utility>

namespace mini_cloud {
namespace {

int compare_fractions(std::uint64_t left_numerator, std::uint64_t left_denominator,
                      std::uint64_t right_numerator, std::uint64_t right_denominator) {
  bool inverted = false;
  while (true) {
    const auto left_quotient = left_numerator / left_denominator;
    const auto right_quotient = right_numerator / right_denominator;
    if (left_quotient != right_quotient) {
      const int comparison = left_quotient < right_quotient ? -1 : 1;
      return inverted ? -comparison : comparison;
    }

    const auto left_remainder = left_numerator % left_denominator;
    const auto right_remainder = right_numerator % right_denominator;
    if (left_remainder == 0 || right_remainder == 0) {
      int comparison = 0;
      if (left_remainder != right_remainder) {
        comparison = left_remainder == 0 ? -1 : 1;
      }
      return inverted ? -comparison : comparison;
    }

    std::swap(left_numerator, left_denominator);
    std::swap(right_numerator, right_denominator);
    left_denominator = left_remainder;
    right_denominator = right_remainder;
    inverted = !inverted;
  }
}

int compare_projected_dominant_utilization(const Worker& left, const Worker& right,
                                           const Resources request) {
  const auto left_projected = *checked_add(left.resources.reserved, request);
  const auto right_projected = *checked_add(right.resources.reserved, request);
  const bool left_cpu_dominant =
      compare_fractions(left_projected.cpu_millicores, left.resources.capacity.cpu_millicores,
                        left_projected.memory_mib, left.resources.capacity.memory_mib) >= 0;
  const bool right_cpu_dominant =
      compare_fractions(right_projected.cpu_millicores, right.resources.capacity.cpu_millicores,
                        right_projected.memory_mib, right.resources.capacity.memory_mib) >= 0;

  const auto left_numerator = left_cpu_dominant ? left_projected.cpu_millicores
                                                 : left_projected.memory_mib;
  const auto left_denominator = left_cpu_dominant ? left.resources.capacity.cpu_millicores
                                                   : left.resources.capacity.memory_mib;
  const auto right_numerator = right_cpu_dominant ? right_projected.cpu_millicores
                                                   : right_projected.memory_mib;
  const auto right_denominator = right_cpu_dominant ? right.resources.capacity.cpu_millicores
                                                     : right.resources.capacity.memory_mib;
  return compare_fractions(left_numerator, left_denominator, right_numerator, right_denominator);
}

}  // namespace

std::optional<SchedulingPolicy> parse_scheduling_policy(const std::string_view name) {
  if (name == "first-fit") return SchedulingPolicy::first_fit;
  if (name == "least-loaded-dominant-resource") return SchedulingPolicy::least_loaded_dominant_resource;
  return std::nullopt;
}

std::string_view scheduling_policy_name(const SchedulingPolicy policy) {
  switch (policy) {
    case SchedulingPolicy::first_fit: return "first-fit";
    case SchedulingPolicy::least_loaded_dominant_resource: return "least-loaded-dominant-resource";
  }
  return "unknown";
}

CapacityDeficit capacity_deficit(const WorkerResources worker, const Resources request) {
  const auto cpu_available = worker.capacity.cpu_millicores - worker.reserved.cpu_millicores;
  const auto memory_available = worker.capacity.memory_mib - worker.reserved.memory_mib;
  return {request.cpu_millicores > cpu_available ? request.cpu_millicores - cpu_available : 0,
          request.memory_mib > memory_available ? request.memory_mib - memory_available : 0};
}

std::optional<std::size_t> choose_worker(const SchedulingPolicy policy,
                                         const std::vector<Worker>& workers,
                                         const Resources request) {
  std::optional<std::size_t> selection;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    const auto& candidate = workers[index];
    if (!is_valid_reservation(candidate.resources)) continue;
    if (evaluate_admission(candidate.resources, request) != Admission::accepted) continue;
    if (policy == SchedulingPolicy::first_fit) return index;
    const int comparison = selection
                               ? compare_projected_dominant_utilization(candidate, workers[*selection], request)
                               : -1;
    if (!selection || comparison < 0 ||
        (comparison == 0 && candidate.id < workers[*selection].id)) {
      selection = index;
    }
  }
  return selection;
}

}  // namespace mini_cloud
