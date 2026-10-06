#include "mini_cloud/worker_protocol.hpp"
#include "mini_cloud/process_supervisor.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <fcntl.h>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <sys/socket.h>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace {

std::optional<std::uint64_t> parse_unsigned(const std::string_view value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
  return parsed;
}

int connect_to_host(const std::string& host, const std::string& port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) return -1;
  int socket_fd = -1;
  for (auto* address = addresses; address; address = address->ai_next) {
    socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    // Replica processes must not keep the agent's control connection alive after a crash.
    if (socket_fd >= 0 && fcntl(socket_fd, F_SETFD, FD_CLOEXEC) == 0 && connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) break;
    if (socket_fd >= 0) close(socket_fd);
    socket_fd = -1;
  }
  freeaddrinfo(addresses);
  return socket_fd;
}

bool send_line(const int socket_fd, const std::string& line) {
  const std::string message = line + "\n";
  std::size_t offset = 0;
  while (offset < message.size()) {
    const auto sent = send(socket_fd, message.data() + offset, message.size() - offset, 0);
    if (sent <= 0) return false;
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

void log_replica_message(std::string message) {
  const auto type = message.find("\"type\"");
  if (type != std::string::npos) message.replace(type, 6, "\"event\"");
  message.insert(1, "\"timestamp_ms\":" + std::to_string(mini_cloud::timestamp_milliseconds()) + ",");
  std::cout << message << std::endl;
}

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " <host> <port> <worker-id> <cpu-millicores> <memory-mib> <heartbeat-ms> [heartbeat-count] [--cgroup-root <delegated-parent> | --no-cgroups]\n";
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc < 7) {
    usage(argv[0]);
    return 2;
  }
  const auto port = parse_unsigned(argv[2]);
  const auto cpu_millicores = parse_unsigned(argv[4]);
  const auto memory_mib = parse_unsigned(argv[5]);
  const auto heartbeat_milliseconds = parse_unsigned(argv[6]);
  int option = 7;
  std::optional<std::uint64_t> heartbeat_count{0};
  if (option < argc && std::string_view(argv[option]).starts_with("--") == false) heartbeat_count = parse_unsigned(argv[option++]);
  bool enforce_cgroups = true;
  std::filesystem::path cgroup_root = "/sys/fs/cgroup";
  if (option < argc) {
    const std::string_view flag = argv[option++];
    if (flag == "--no-cgroups") enforce_cgroups = false;
    else if (flag == "--cgroup-root" && option < argc) cgroup_root = argv[option++];
    else { usage(argv[0]); return 2; }
  }
  if (option != argc) { usage(argv[0]); return 2; }
  if (!port || *port == 0 || *port > 65535 || !mini_cloud::is_valid_worker_id(argv[3]) ||
      !cpu_millicores || *cpu_millicores == 0 || !memory_mib || *memory_mib == 0 ||
      !heartbeat_milliseconds || *heartbeat_milliseconds == 0 || !heartbeat_count) {
    std::cerr << "Invalid worker configuration.\n";
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);
  const std::string scope_prefix = "mini-cloud-" + std::string(argv[3]) + "-" + std::to_string(getpid()) + "-";
  if (enforce_cgroups) {
    mini_cloud::CgroupScope probe(cgroup_root, scope_prefix + "probe");
    if (!probe.configure(1, 1) || !probe.cleanup()) {
      std::cerr << "CGROUP_ERROR: " << probe.error() << '\n';
      return 1;
    }
  }
  const int socket_fd = connect_to_host(argv[1], argv[2]);
  if (socket_fd < 0) {
    std::cerr << "Unable to connect to controller.\n";
    return 1;
  }
  const std::string worker_id = argv[3];
  std::cout << mini_cloud::lifecycle_event("worker_connected", worker_id) << std::endl;
  if (!send_line(socket_fd, mini_cloud::registration_message(worker_id, *cpu_millicores, *memory_mib))) {
    close(socket_fd);
    return 1;
  }
  std::cout << mini_cloud::lifecycle_event("worker_registration_sent", worker_id) << std::endl;

  std::cout << "{\"timestamp_ms\":" << mini_cloud::timestamp_milliseconds()
            << ",\"event\":\"worker_enforcement\",\"worker_id\":\"" << worker_id
            << "\",\"cgroup_enforcement\":\"" << (enforce_cgroups ? "REQUIRED" : "DISABLED") << "\"}" << std::endl;
  struct ManagedReplica {
    std::string workload_id;
    std::unique_ptr<mini_cloud::ProcessSupervisor> supervisor;
    bool intentional_stop = false;
  };
  std::vector<ManagedReplica> supervisors;
  std::string input_buffer;
  std::uint64_t sent = 0;
  auto next_heartbeat = std::chrono::steady_clock::now();
  bool connected = true;
  while (connected && (*heartbeat_count == 0 || sent < *heartbeat_count)) {
    const auto now = std::chrono::steady_clock::now();
    const auto timeout = now >= next_heartbeat ? 0 : static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next_heartbeat - now).count());
    pollfd descriptor{socket_fd, POLLIN, 0};
    const int poll_result = poll(&descriptor, 1, timeout);
    if (poll_result < 0 && errno != EINTR) break;
    if (poll_result > 0 && descriptor.revents & (POLLIN | POLLHUP | POLLERR)) {
      char buffer[2048];
      const auto received = recv(socket_fd, buffer, sizeof(buffer), 0);
      if (received <= 0) break;
      input_buffer.append(buffer, static_cast<std::size_t>(received));
      while (true) {
        const auto newline = input_buffer.find('\n');
        if (newline == std::string::npos) break;
        const std::string line = input_buffer.substr(0, newline);
        input_buffer.erase(0, newline + 1);
        const auto command = mini_cloud::parse_controller_message(line);
        if (!command) { connected = false; break; }
        if (command->type == mini_cloud::ControllerMessageType::stop) {
          bool known = false;
          for (auto& managed : supervisors) {
            if (managed.supervisor && managed.workload_id == command->workload_id && managed.supervisor->replica_id() == command->replica_id) {
              managed.intentional_stop = true;
              const auto stopped = managed.supervisor->stop();
              // Terminal acknowledgements are emitted below only after cleanup.
              if (stopped && stopped->cleanup_complete) {
                if (!send_line(socket_fd, mini_cloud::replica_stopped_message(worker_id, command->workload_id, command->replica_id))) connected = false;
                managed.supervisor.reset();
              }
              known = true;
              break;
            }
          }
          if (!known && !send_line(socket_fd, mini_cloud::replica_stopped_message(worker_id, command->workload_id, command->replica_id))) connected = false;
          continue;
        }
        auto supervisor = std::make_unique<mini_cloud::ProcessSupervisor>(command->replica_id);
        std::unique_ptr<mini_cloud::CgroupScope> scope;
        if (enforce_cgroups) {
          scope = std::make_unique<mini_cloud::CgroupScope>(cgroup_root, scope_prefix + command->replica_id);
          if (!scope->configure(command->cpu_millicores, command->memory_mib)) {
            std::cerr << "CGROUP_ERROR replica=" << command->replica_id << ": " << scope->error() << '\n';
            if (!send_line(socket_fd, mini_cloud::replica_launch_failed_message(worker_id, command->workload_id, command->replica_id, "cgroup_setup_failed"))) connected = false;
            continue;
          }
        }
        const auto scope_path = scope ? scope->path().string() : std::string{};
        if (!supervisor->launch(command->executable, command->arguments, std::move(scope))) {
          std::cerr << "LAUNCH_ERROR replica=" << command->replica_id << ": " << supervisor->error() << '\n';
          if (!send_line(socket_fd, mini_cloud::replica_launch_failed_message(worker_id, command->workload_id, command->replica_id, "runtime_launch_failed"))) connected = false;
          continue;
        }
        if (!send_line(socket_fd, mini_cloud::launch_accepted_message(worker_id, command->workload_id, command->replica_id)) ||
            !send_line(socket_fd, mini_cloud::replica_running_message(worker_id, command->workload_id, command->replica_id,
                                                                      static_cast<std::uint64_t>(supervisor->pid()), enforce_cgroups, scope_path))) {
          connected = false;
          break;
        }
        // Reuse protocol serialization so paths and arguments are escaped correctly.
        log_replica_message(mini_cloud::replica_running_message(worker_id, command->workload_id, command->replica_id,
                          static_cast<std::uint64_t>(supervisor->pid()), enforce_cgroups, scope_path));
        supervisors.push_back({command->workload_id, std::move(supervisor), false});
      }
    }
    if (std::chrono::steady_clock::now() >= next_heartbeat) {
      if (!send_line(socket_fd, mini_cloud::heartbeat_message(worker_id))) break;
      ++sent;
      std::cout << mini_cloud::lifecycle_event("worker_heartbeat_sent", worker_id) << std::endl;
      next_heartbeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(*heartbeat_milliseconds);
    }
    for (auto it = supervisors.begin(); it != supervisors.end();) {
      if (!it->supervisor) { it = supervisors.erase(it); continue; }
      const auto termination = it->intentional_stop ? it->supervisor->stop() : it->supervisor->poll();
      if (!termination) { ++it; continue; }
      if (!termination->cleanup_complete) {
        std::cerr << "CGROUP_CLEANUP_ERROR replica=" << it->supervisor->replica_id() << ": " << it->supervisor->error() << '\n';
        ++it; continue;
      }
      const auto message = it->intentional_stop
          ? mini_cloud::replica_stopped_message(worker_id, it->workload_id, it->supervisor->replica_id())
          : mini_cloud::replica_exited_message(worker_id, it->workload_id, it->supervisor->replica_id(),
                termination->exited_normally ? static_cast<std::uint64_t>(termination->exit_code) : 0,
                termination->terminated_by_signal ? static_cast<std::uint64_t>(termination->signal_number) : 0);
      if (!send_line(socket_fd, message)) connected = false;
      log_replica_message(message);
      it = supervisors.erase(it);
    }
  }
  close(socket_fd);
  return 0;
}
