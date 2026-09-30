#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mini_cloud {

inline constexpr std::uint64_t kProtocolVersion = 1;

enum class WorkerMessageType { registration, heartbeat, launch_accepted, replica_running };

enum class ControllerMessageType { launch };

struct WorkerMessage {
  WorkerMessageType type;
  std::string worker_id;
  std::uint64_t cpu_millicores = 0;
  std::uint64_t memory_mib = 0;
  std::string workload_id;
  std::string replica_id;
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
                                                   std::string_view replica_id, std::uint64_t pid);
[[nodiscard]] std::uint64_t timestamp_milliseconds();
[[nodiscard]] std::string lifecycle_event(std::string_view event, std::string_view worker_id);

}  // namespace mini_cloud
