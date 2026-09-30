#include "mini_cloud/worker_protocol.hpp"

#include <charconv>
#include <chrono>
#include <system_error>
#include <vector>

namespace mini_cloud {
namespace {

std::optional<std::string_view> string_field(const std::string_view line, const std::string_view name) {
  const std::string marker = "\"" + std::string(name) + "\":\"";
  const auto start = line.find(marker);
  if (start == std::string_view::npos) return std::nullopt;
  const auto value_start = start + marker.size();
  const auto value_end = line.find('"', value_start);
  if (value_end == std::string_view::npos) return std::nullopt;
  return line.substr(value_start, value_end - value_start);
}

std::optional<std::uint64_t> unsigned_field(const std::string_view line, const std::string_view name) {
  const std::string marker = "\"" + std::string(name) + "\":";
  const auto start = line.find(marker);
  if (start == std::string_view::npos) return std::nullopt;
  const auto value_start = start + marker.size();
  const auto value_end = line.find_first_not_of("0123456789", value_start);
  const auto value = line.substr(value_start, value_end - value_start);
  if (value.empty()) return std::nullopt;
  std::uint64_t parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
  return parsed;
}

std::optional<std::vector<std::string>> string_array_field(const std::string_view line, const std::string_view name) {
  const std::string marker = "\"" + std::string(name) + "\":[";
  const auto start = line.find(marker);
  if (start == std::string_view::npos) return std::nullopt;
  auto cursor = start + marker.size();
  std::vector<std::string> values;
  if (cursor < line.size() && line[cursor] == ']') return values;
  while (cursor < line.size() && line[cursor] == '"') {
    const auto end = line.find('"', cursor + 1);
    if (end == std::string_view::npos) return std::nullopt;
    values.emplace_back(line.substr(cursor + 1, end - cursor - 1));
    cursor = end + 1;
    if (cursor < line.size() && line[cursor] == ']') return values;
    if (cursor >= line.size() || line[cursor] != ',') return std::nullopt;
    ++cursor;
  }
  return std::nullopt;
}

std::string json_string(const std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    if (character == '"' || character == '\\') result.push_back('\\');
    result.push_back(character);
  }
  return result;
}

}  // namespace

bool is_valid_worker_id(const std::string_view worker_id) {
  if (worker_id.empty()) return false;
  for (const char character : worker_id) {
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') || character == '-' ||
                         character == '_' || character == '.';
    if (!allowed) return false;
  }
  return true;
}

std::optional<WorkerMessage> parse_worker_message(const std::string_view line) {
  if (line.empty() || line.front() != '{' || line.back() != '}') return std::nullopt;
  const auto version = unsigned_field(line, "version");
  const auto type = string_field(line, "type");
  const auto worker_id = string_field(line, "worker_id");
  if (!version || *version != kProtocolVersion || !type || !worker_id || !is_valid_worker_id(*worker_id)) {
    return std::nullopt;
  }
  if (*type == "heartbeat") return WorkerMessage{WorkerMessageType::heartbeat, std::string(*worker_id), 0, 0, {}, {}};
  if (*type == "launch_accepted" || *type == "replica_running") {
    const auto workload_id = string_field(line, "workload_id");
    const auto replica_id = string_field(line, "replica_id");
    if (!workload_id || !replica_id) return std::nullopt;
    return WorkerMessage{*type == "launch_accepted" ? WorkerMessageType::launch_accepted
                                                       : WorkerMessageType::replica_running,
                         std::string(*worker_id), 0, 0, std::string(*workload_id), std::string(*replica_id)};
  }
  if (*type != "register") return std::nullopt;

  const auto cpu_millicores = unsigned_field(line, "cpu_millicores");
  const auto memory_mib = unsigned_field(line, "memory_mib");
  if (!cpu_millicores || !memory_mib || *cpu_millicores == 0 || *memory_mib == 0) return std::nullopt;
  return WorkerMessage{WorkerMessageType::registration, std::string(*worker_id), *cpu_millicores, *memory_mib, {}, {}};
}

std::optional<ControllerMessage> parse_controller_message(const std::string_view line) {
  if (line.empty() || line.front() != '{' || line.back() != '}') return std::nullopt;
  const auto version = unsigned_field(line, "version");
  const auto type = string_field(line, "type");
  if (!version || *version != kProtocolVersion || !type || *type != "launch") return std::nullopt;
  const auto workload_id = string_field(line, "workload_id");
  const auto replica_id = string_field(line, "replica_id");
  const auto executable = string_field(line, "executable");
  const auto arguments = string_array_field(line, "arguments");
  const auto cpu = unsigned_field(line, "cpu_millicores");
  const auto memory = unsigned_field(line, "memory_mib");
  if (!workload_id || !replica_id || !executable || executable->empty() || !arguments || !cpu || !memory ||
      *cpu == 0 || *memory == 0) return std::nullopt;
  return ControllerMessage{ControllerMessageType::launch, std::string(*workload_id), std::string(*replica_id),
                           std::string(*executable), *arguments, *cpu, *memory};
}

std::string registration_message(const std::string_view worker_id, const std::uint64_t cpu_millicores,
                                 const std::uint64_t memory_mib) {
  return "{\"version\":1,\"type\":\"register\",\"worker_id\":\"" + std::string(worker_id) +
         "\",\"cpu_millicores\":" + std::to_string(cpu_millicores) + ",\"memory_mib\":" +
         std::to_string(memory_mib) + "}";
}

std::string heartbeat_message(const std::string_view worker_id) {
  return "{\"version\":1,\"type\":\"heartbeat\",\"worker_id\":\"" + std::string(worker_id) + "\"}";
}

std::string launch_message(const std::string_view workload_id, const std::string_view replica_id,
                           const std::string_view executable, const std::vector<std::string>& arguments,
                           const std::uint64_t cpu_millicores, const std::uint64_t memory_mib) {
  std::string result = "{\"version\":1,\"type\":\"launch\",\"workload_id\":\"" + json_string(workload_id) +
      "\",\"replica_id\":\"" + json_string(replica_id) + "\",\"executable\":\"" + json_string(executable) +
      "\",\"arguments\":[";
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index != 0) result += ',';
    result += "\"" + json_string(arguments[index]) + "\"";
  }
  return result + "],\"cpu_millicores\":" + std::to_string(cpu_millicores) +
         ",\"memory_mib\":" + std::to_string(memory_mib) + "}";
}

std::string launch_accepted_message(const std::string_view worker_id, const std::string_view workload_id,
                                    const std::string_view replica_id) {
  return "{\"version\":1,\"type\":\"launch_accepted\",\"worker_id\":\"" + json_string(worker_id) +
         "\",\"workload_id\":\"" + json_string(workload_id) + "\",\"replica_id\":\"" + json_string(replica_id) + "\"}";
}

std::string replica_running_message(const std::string_view worker_id, const std::string_view workload_id,
                                    const std::string_view replica_id, const std::uint64_t pid) {
  return "{\"version\":1,\"type\":\"replica_running\",\"worker_id\":\"" + json_string(worker_id) +
         "\",\"workload_id\":\"" + json_string(workload_id) + "\",\"replica_id\":\"" + json_string(replica_id) +
         "\",\"pid\":" + std::to_string(pid) + "}";
}

std::uint64_t timestamp_milliseconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string lifecycle_event(const std::string_view event, const std::string_view worker_id) {
  return "{\"timestamp_ms\":" + std::to_string(timestamp_milliseconds()) + ",\"event\":\"" +
         std::string(event) + "\",\"worker_id\":\"" + std::string(worker_id) + "\"}";
}

}  // namespace mini_cloud
