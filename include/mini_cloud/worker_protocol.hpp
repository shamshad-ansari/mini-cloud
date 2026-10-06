#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mini_cloud {

inline constexpr std::uint64_t kProtocolVersion = 1;

enum class WorkerMessageType { registration, heartbeat, launch_accepted, replica_running, replica_stopped, replica_exited, launch_failed };

enum class ControllerMessageType { launch, stop };

struct WorkerMessage {
  WorkerMessageType type;
  std::string worker_id;
  std::uint64_t cpu_millicores = 0;
  std::uint64_t memory_mib = 0;
  std::string workload_id;
  std::string replica_id;
  bool cgroup_enforced = false;
  std::string cgroup_path;
  std::string reason;
  std::uint64_t exit_code = 0;
  std::uint64_t signal = 0;
};

struct ControllerMessage {
  ControllerMessageType type;
  std::string workload_id;
  std::string replica_id;
  std::string executable;
  std::vector<std::string> arguments;
  std::uint64_t cpu_millicores = 0;
  std::uint64_t memory_mib = 0;
};

[[nodiscard]] bool is_valid_worker_id(std::string_view worker_id);
[[nodiscard]] std::optional<WorkerMessage> parse_worker_message(std::string_view line);
[[nodiscard]] std::optional<ControllerMessage> parse_controller_message(std::string_view line);
[[nodiscard]] std::string registration_message(std::string_view worker_id,
                                               std::uint64_t cpu_millicores,
                                               std::uint64_t memory_mib);
[[nodiscard]] std::string heartbeat_message(std::string_view worker_id);
[[nodiscard]] std::string launch_message(std::string_view workload_id, std::string_view replica_id,
                                         std::string_view executable, const std::vector<std::string>& arguments,
                                         std::uint64_t cpu_millicores, std::uint64_t memory_mib);
[[nodiscard]] std::string launch_accepted_message(std::string_view worker_id, std::string_view workload_id,
                                                  std::string_view replica_id);
[[nodiscard]] std::string replica_running_message(std::string_view worker_id, std::string_view workload_id,
                                                   std::string_view replica_id, std::uint64_t pid,
                                                   bool cgroup_enforced = false, std::string_view cgroup_path = {});
[[nodiscard]] std::string stop_message(std::string_view workload_id, std::string_view replica_id);
[[nodiscard]] std::string replica_stopped_message(std::string_view worker_id, std::string_view workload_id, std::string_view replica_id);
[[nodiscard]] std::string replica_exited_message(std::string_view worker_id, std::string_view workload_id,
                                                std::string_view replica_id, std::uint64_t exit_code, std::uint64_t signal);
[[nodiscard]] std::string replica_launch_failed_message(std::string_view worker_id, std::string_view workload_id,
                                                       std::string_view replica_id, std::string_view reason);
[[nodiscard]] std::uint64_t timestamp_milliseconds();
[[nodiscard]] std::string lifecycle_event(std::string_view event, std::string_view worker_id);

}  // namespace mini_cloud
