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

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " <host> <port> <worker-id> <cpu-millicores> <memory-mib> <heartbeat-ms> [heartbeat-count]\n";
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc != 7 && argc != 8) {
    usage(argv[0]);
    return 2;
  }
  const auto port = parse_unsigned(argv[2]);
  const auto cpu_millicores = parse_unsigned(argv[4]);
  const auto memory_mib = parse_unsigned(argv[5]);
  const auto heartbeat_milliseconds = parse_unsigned(argv[6]);
  const auto heartbeat_count = argc == 8 ? parse_unsigned(argv[7]) : std::optional<std::uint64_t>{0};
  if (!port || *port == 0 || *port > 65535 || !mini_cloud::is_valid_worker_id(argv[3]) ||
      !cpu_millicores || *cpu_millicores == 0 || !memory_mib || *memory_mib == 0 ||
      !heartbeat_milliseconds || *heartbeat_milliseconds == 0 || !heartbeat_count) {
    std::cerr << "Invalid worker configuration.\n";
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);
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

  std::vector<std::unique_ptr<mini_cloud::ProcessSupervisor>> supervisors;
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
          bool stopped = true;
          for (auto it = supervisors.begin(); it != supervisors.end(); ++it) {
            if ((*it)->replica_id() == command->replica_id) {
              static_cast<void>((*it)->stop());
              stopped = (*it)->pid() <= 0;
              if (stopped) supervisors.erase(it);
              break;
            }
          }
          if (stopped && !send_line(socket_fd, mini_cloud::replica_stopped_message(worker_id, command->workload_id, command->replica_id))) connected = false;
          continue;
        }
        auto supervisor = std::make_unique<mini_cloud::ProcessSupervisor>(command->replica_id);
        if (!supervisor->launch(command->executable, command->arguments)) { connected = false; break; }
        if (!send_line(socket_fd, mini_cloud::launch_accepted_message(worker_id, command->workload_id, command->replica_id)) ||
            !send_line(socket_fd, mini_cloud::replica_running_message(worker_id, command->workload_id, command->replica_id,
                                                                      static_cast<std::uint64_t>(supervisor->pid())))) {
          connected = false;
          break;
        }
        std::cout << mini_cloud::lifecycle_event("replica_running", worker_id) << std::endl;
        supervisors.push_back(std::move(supervisor));
      }
    }
    if (std::chrono::steady_clock::now() >= next_heartbeat) {
      if (!send_line(socket_fd, mini_cloud::heartbeat_message(worker_id))) break;
      ++sent;
      std::cout << mini_cloud::lifecycle_event("worker_heartbeat_sent", worker_id) << std::endl;
      next_heartbeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(*heartbeat_milliseconds);
    }
    for (auto& supervisor : supervisors) static_cast<void>(supervisor->poll());
  }
  close(socket_fd);
  return 0;
}
