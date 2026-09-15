#include "mini_cloud/resources.hpp"

#include <limits>

namespace mini_cloud {

bool is_valid_capacity(const Resources value) {
  return value.cpu_millicores > 0 && value.memory_mib > 0;
}

bool is_valid_reservation(const WorkerResources worker) {
  return is_valid_capacity(worker.capacity) &&
         worker.reserved.cpu_millicores <= worker.capacity.cpu_millicores &&
         worker.reserved.memory_mib <= worker.capacity.memory_mib;
}

std::optional<Resources> checked_add(const Resources left, const Resources right) {
  if (left.cpu_millicores > std::numeric_limits<std::uint64_t>::max() - right.cpu_millicores ||
      left.memory_mib > std::numeric_limits<std::uint64_t>::max() - right.memory_mib) {
    return std::nullopt;
  }
  return Resources{left.cpu_millicores + right.cpu_millicores,
                   left.memory_mib + right.memory_mib};
}

Admission evaluate_admission(const WorkerResources worker, const Resources request) {
  const auto projected = checked_add(worker.reserved, request);
  const bool cpu_fits = projected && projected->cpu_millicores <= worker.capacity.cpu_millicores;
  const bool memory_fits = projected && projected->memory_mib <= worker.capacity.memory_mib;

  if (cpu_fits && memory_fits) {
    return Admission::accepted;
  }
  if (!cpu_fits && !memory_fits) {
    return Admission::insufficient_cpu_and_memory;
  }
  return cpu_fits ? Admission::insufficient_memory : Admission::insufficient_cpu;
}

std::string_view admission_name(const Admission admission) {
  switch (admission) {
    case Admission::accepted:
      return "accepted";
    case Admission::insufficient_cpu:
      return "rejected: insufficient CPU";
    case Admission::insufficient_memory:
      return "rejected: insufficient memory";
    case Admission::insufficient_cpu_and_memory:
      return "rejected: insufficient CPU and memory";
  }
  return "rejected";
}

}  // namespace mini_cloud
