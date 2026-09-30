#pragma once

#include "mini_cloud/resources.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mini_cloud {

enum class SchedulingPolicy { first_fit, least_loaded_dominant_resource };

struct Worker {
  std::string id;
  WorkerResources resources;
};

struct CapacityDeficit {
  std::uint64_t cpu_millicores;
  std::uint64_t memory_mib;
};

[[nodiscard]] std::optional<SchedulingPolicy> parse_scheduling_policy(std::string_view name);
[[nodiscard]] std::string_view scheduling_policy_name(SchedulingPolicy policy);
[[nodiscard]] CapacityDeficit capacity_deficit(WorkerResources worker, Resources request);
[[nodiscard]] std::optional<std::size_t> choose_worker(
    SchedulingPolicy policy, const std::vector<Worker>& workers, Resources request);

}  // namespace mini_cloud
