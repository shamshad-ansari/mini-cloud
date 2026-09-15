#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mini_cloud {

struct Resources {
  std::uint64_t cpu_millicores;
  std::uint64_t memory_mib;
};

struct WorkerResources {
  Resources capacity;
  Resources reserved;
};

enum class Admission {
  accepted,
  insufficient_cpu,
  insufficient_memory,
  insufficient_cpu_and_memory,
};

[[nodiscard]] bool is_valid_capacity(Resources value);
[[nodiscard]] bool is_valid_reservation(WorkerResources worker);
[[nodiscard]] std::optional<Resources> checked_add(Resources left, Resources right);
[[nodiscard]] Admission evaluate_admission(WorkerResources worker, Resources request);
[[nodiscard]] std::string_view admission_name(Admission admission);

}  // namespace mini_cloud
