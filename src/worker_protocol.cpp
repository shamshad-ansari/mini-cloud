#include "mini_cloud/worker_protocol.hpp"

#include <charconv>
#include <chrono>
#include <system_error>
#include <vector>

namespace mini_cloud {
namespace {

std::optional<std::string> quoted_string(std::string_view line, std::size_t& cursor) {
  if (cursor >= line.size() || line[cursor++] != '"') return std::nullopt;
  std::string value;
  while (cursor < line.size()) {
    char character = line[cursor++];
    if (character == '"') return value;
    if (character == '\\') {
      if (cursor >= line.size()) return std::nullopt;
      character = line[cursor++];
      switch (character) {
        case 'n': character = '\n'; break;
        case 'r': character = '\r'; break;
        case 't': character = '\t'; break;
        case '"': case '\\': case '/': break;
        default: return std::nullopt;
      }
    } else if (static_cast<unsigned char>(character) < 32) return std::nullopt;
    value += character;
  }
  return std::nullopt;
}
std::optional<std::string> string_field(const std::string_view line, const std::string_view name) {
  const std::string marker = "\"" + std::string(name) + "\":\"";
  const auto start = line.find(marker);
  if (start == std::string_view::npos) return std::nullopt;
  auto cursor = start + marker.size() - 1;
  return quoted_string(line, cursor);
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
    const auto value = quoted_string(line, cursor);
    if (!value) return std::nullopt;
    values.push_back(*value);
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
    if (character == '\n') { result += "\\n"; continue; }
    if (character == '\r') { result += "\\r"; continue; }
    if (character == '\t') { result += "\\t"; continue; }
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
  if (*type == "heartbeat") return WorkerMessage{WorkerMessageType::heartbeat, std::string(*worker_id), 0, 0, {}, {}, false, {}, {}, 0, 0};
  if (*type == "launch_accepted" || *type == "replica_running" || *type == "replica_stopped" ||
      *type == "replica_exited" || *type == "launch_failed") {
    const auto workload_id = string_field(line, "workload_id");
    const auto replica_id = string_field(line, "replica_id");
    if (!workload_id || !replica_id || !is_valid_worker_id(*workload_id) || !is_valid_worker_id(*replica_id)) return std::nullopt;
    WorkerMessageType kind = WorkerMessageType::launch_accepted;
    if (*type == "replica_running") kind = WorkerMessageType::replica_running;
    else if (*type == "replica_stopped") kind = WorkerMessageType::replica_stopped;
    else if (*type == "replica_exited") kind = WorkerMessageType::replica_exited;
    else if (*type == "launch_failed") kind = WorkerMessageType::launch_failed;
    WorkerMessage result{kind, *worker_id, 0, 0, *workload_id, *replica_id, false, {}, {}, 0, 0};
    if (kind == WorkerMessageType::replica_running) {
      const auto enforced = unsigned_field(line, "cgroup_enforced");
      if (enforced && *enforced > 1) return std::nullopt;
      result.cgroup_enforced = enforced.value_or(0) == 1;
      result.cgroup_path = string_field(line, "cgroup_path").value_or("");
      if (result.cgroup_enforced && result.cgroup_path.empty()) return std::nullopt;
    } else if (kind == WorkerMessageType::replica_exited) {
      const auto code = unsigned_field(line, "exit_code"); const auto signal = unsigned_field(line, "signal");
      if (!code || !signal) return std::nullopt;
      result.exit_code = *code; result.signal = *signal;
    } else if (kind == WorkerMessageType::launch_failed) {
      const auto reason = string_field(line, "reason");
      if (!reason || !is_valid_worker_id(*reason)) return std::nullopt;
      result.reason = *reason;
    }
    return result;
  }
  if (*type != "register") return std::nullopt;

  const auto cpu_millicores = unsigned_field(line, "cpu_millicores");
  const auto memory_mib = unsigned_field(line, "memory_mib");
  if (!cpu_millicores || !memory_mib || *cpu_millicores == 0 || *memory_mib == 0) return std::nullopt;
  return WorkerMessage{WorkerMessageType::registration, std::string(*worker_id), *cpu_millicores, *memory_mib, {}, {}, false, {}, {}, 0, 0};
}

std::optional<ControllerMessage> parse_controller_message(const std::string_view line) {
  if (line.empty() || line.front() != '{' || line.back() != '}') return std::nullopt;
  const auto version = unsigned_field(line, "version");
  const auto type = string_field(line, "type");
  if (!version || *version != kProtocolVersion || !type || (*type != "launch" && *type != "stop")) return std::nullopt;
  const auto workload_id = string_field(line, "workload_id");
  const auto replica_id = string_field(line, "replica_id");
  if (*type == "stop") {
    if (!workload_id || !replica_id || !is_valid_worker_id(*workload_id) || !is_valid_worker_id(*replica_id)) return std::nullopt;
    return ControllerMessage{ControllerMessageType::stop, std::string(*workload_id), std::string(*replica_id), {}, {}, 0, 0};
  }
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
                                    const std::string_view replica_id, const std::uint64_t pid, bool cgroup_enforced, std::string_view cgroup_path) {
  return "{\"version\":1,\"type\":\"replica_running\",\"worker_id\":\"" + json_string(worker_id) +
         "\",\"workload_id\":\"" + json_string(workload_id) + "\",\"replica_id\":\"" + json_string(replica_id) +
         "\",\"pid\":" + std::to_string(pid) + ",\"cgroup_enforced\":" + (cgroup_enforced ? "1" : "0") +
         ",\"cgroup_path\":\"" + json_string(cgroup_path) + "\"}";
}

std::string stop_message(std::string_view workload_id, std::string_view replica_id) {
  return "{\"version\":1,\"type\":\"stop\",\"workload_id\":\"" + json_string(workload_id) +
         "\",\"replica_id\":\"" + json_string(replica_id) + "\"}";
}

std::string replica_stopped_message(std::string_view worker_id, std::string_view workload_id, std::string_view replica_id) {
  return "{\"version\":1,\"type\":\"replica_stopped\",\"worker_id\":\"" + json_string(worker_id) +
         "\",\"workload_id\":\"" + json_string(workload_id) + "\",\"replica_id\":\"" + json_string(replica_id) + "\"}";
}

std::string replica_exited_message(std::string_view worker_id, std::string_view workload_id,
                                   std::string_view replica_id, std::uint64_t exit_code, std::uint64_t signal) {
  return "{\"version\":1,\"type\":\"replica_exited\",\"worker_id\":\"" + json_string(worker_id) +
         "\",\"workload_id\":\"" + json_string(workload_id) + "\",\"replica_id\":\"" + json_string(replica_id) +
         "\",\"exit_code\":" + std::to_string(exit_code) + ",\"signal\":" + std::to_string(signal) + "}";
}
std::string replica_launch_failed_message(std::string_view worker_id, std::string_view workload_id,
                                          std::string_view replica_id, std::string_view reason) {
  return "{\"version\":1,\"type\":\"launch_failed\",\"worker_id\":\"" + json_string(worker_id) +
         "\",\"workload_id\":\"" + json_string(workload_id) + "\",\"replica_id\":\"" + json_string(replica_id) +
         "\",\"reason\":\"" + json_string(reason) + "\"}";
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
