#include "mini_cloud/process_supervisor.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

std::optional<std::uint64_t> parse_unsigned(const std::string_view value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
  return parsed;
}

bool is_valid_replica_id(const std::string_view id) {
  if (id.empty()) return false;
  for (const char character : id) {
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') || character == '-' ||
                         character == '_' || character == '.';
    if (!allowed) return false;
  }
  return true;
}

std::string_view state_name(const mini_cloud::ReplicaState state) {
  switch (state) {
    case mini_cloud::ReplicaState::running: return "RUNNING";
    case mini_cloud::ReplicaState::stopped: return "STOPPED";
    case mini_cloud::ReplicaState::exited_unexpectedly: return "EXITED_UNEXPECTEDLY";
  }
  return "UNKNOWN";
}

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " <replica-id> <stop-after-milliseconds> -- <executable> [argument ...]\n";
}

void emit_termination(const std::string& replica_id, const pid_t pid,
                      const mini_cloud::ProcessTermination& termination) {
  std::cout << "{\"event\":\"replica_terminated\",\"replica_id\":\"" << replica_id
            << "\",\"pid\":" << pid << ",\"state\":\"" << state_name(termination.state)
            << "\",\"intentional\":"
            << (termination.state == mini_cloud::ReplicaState::stopped ? "true" : "false");
  if (termination.exited_normally) std::cout << ",\"exit_code\":" << termination.exit_code;
  if (termination.terminated_by_signal) std::cout << ",\"signal\":" << termination.signal_number;
  std::cout << "}\n";
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc < 6 || std::string_view(argv[3]) != "--") {
    usage(argv[0]);
    return 2;
  }
  const std::string replica_id = argv[1];
  const auto stop_after_milliseconds = parse_unsigned(argv[2]);
  if (!is_valid_replica_id(replica_id) || !stop_after_milliseconds ||
      *stop_after_milliseconds > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count())) {
    std::cerr << "Replica ID must use letters, digits, '.', '_' or '-', and delay must be valid.\n";
    return 2;
  }

  std::vector<std::string> arguments;
  for (int index = 5; index < argc; ++index) arguments.emplace_back(argv[index]);

  mini_cloud::ProcessSupervisor supervisor(replica_id);
  if (!supervisor.launch(argv[4], arguments)) {
    std::cerr << "Failed to launch replica " << replica_id << '\n';
    return 1;
  }
  const pid_t child_pid = supervisor.pid();
  std::cout << "{\"event\":\"replica_started\",\"replica_id\":\"" << supervisor.replica_id()
            << "\",\"pid\":" << child_pid << ",\"state\":\"RUNNING\"}\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(*stop_after_milliseconds));
  if (const auto termination = supervisor.poll()) {
    emit_termination(supervisor.replica_id(), child_pid, *termination);
    return 1;
  }

  std::cout << "{\"event\":\"replica_stopping\",\"replica_id\":\"" << supervisor.replica_id()
            << "\",\"pid\":" << child_pid << ",\"state\":\"STOPPING\"}\n";
  const auto termination = supervisor.stop();
  if (!termination) {
    std::cerr << "Failed to stop replica " << supervisor.replica_id() << '\n';
    return 1;
  }
  emit_termination(supervisor.replica_id(), child_pid, *termination);
  return 0;
}
