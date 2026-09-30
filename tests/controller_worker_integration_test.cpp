#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

int failures = 0;

void expect(const bool condition, const char* description) {
  if (!condition) {
    std::cerr << "FAILED: " << description << '\n';
    ++failures;
  }
}

std::uint16_t available_port() {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    std::cerr << "socket failed: " << std::strerror(errno) << '\n';
    return 0;
  }
  sockaddr_in address{};
#ifdef __APPLE__
  address.sin_len = sizeof(address);
#endif
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    std::cerr << "bind failed: " << std::strerror(errno) << '\n';
    close(socket_fd);
    return 0;
  }
  socklen_t length = sizeof(address);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    std::cerr << "getsockname failed: " << std::strerror(errno) << '\n';
    close(socket_fd);
    return 0;
  }
  const auto port = ntohs(address.sin_port);
  close(socket_fd);
  return port;
}

int connect_localhost(const std::uint16_t port) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
#ifdef __APPLE__
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (socket_fd >= 0 && connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
      return socket_fd;
    }
    if (socket_fd >= 0) close(socket_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

std::string read_all(const int file_descriptor) {
  std::string output;
  char buffer[1024];
  while (true) {
    const auto count = read(file_descriptor, buffer, sizeof(buffer));
    if (count <= 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  return output;
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc != 3) return EXIT_FAILURE;
  const auto port = available_port();
  expect(port != 0, "an ephemeral localhost port is available");
  if (port == 0) return EXIT_FAILURE;
  const std::string port_text = std::to_string(port);

  int controller_input[2];
  int controller_output[2];
  if (pipe(controller_input) < 0 || pipe(controller_output) < 0) return EXIT_FAILURE;
  const pid_t controller = fork();
  if (controller == 0) {
    dup2(controller_input[0], STDIN_FILENO);
    dup2(controller_output[1], STDOUT_FILENO);
    close(controller_input[0]);
    close(controller_input[1]);
    close(controller_output[0]);
    close(controller_output[1]);
    execl(argv[1], argv[1], port_text.c_str(), nullptr);
    _exit(127);
  }
  if (controller < 0) return EXIT_FAILURE;
  close(controller_input[0]);
  close(controller_output[1]);

  const int invalid_client = connect_localhost(port);
  expect(invalid_client >= 0, "controller accepts a localhost connection");
  if (invalid_client >= 0) {
    const std::string invalid =
        "{\"version\":2,\"type\":\"register\",\"worker_id\":\"bad\",\"cpu_millicores\":1,\"memory_mib\":1}\n";
    static_cast<void>(write(invalid_client, invalid.data(), invalid.size()));
    close(invalid_client);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const pid_t worker = fork();
  if (worker == 0) {
    execl(argv[2], argv[2], "127.0.0.1", port_text.c_str(), "agent-1", "2000", "4096", "30", "20", nullptr);
    _exit(127);
  }
  expect(worker > 0, "worker process starts");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const std::string commands =
      "submit first-fit 250 64 1 -- /bin/sleep 1\n"
      "submit least-loaded-dominant-resource 5000 64 1 -- /bin/sleep 1\n";
  static_cast<void>(write(controller_input[1], commands.data(), commands.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  static_cast<void>(write(controller_input[1], "status\nquit\n", 12));
  close(controller_input[1]);

  int controller_status = 0;
  static_cast<void>(waitpid(controller, &controller_status, 0));
  if (worker > 0) {
    int worker_status = 0;
    static_cast<void>(waitpid(worker, &worker_status, 0));
  }
  const std::string controller_events = read_all(controller_output[0]);
  close(controller_output[0]);

  expect(WIFEXITED(controller_status) && WEXITSTATUS(controller_status) == 0,
         "controller exits cleanly after quit command");
  expect(controller_events.find("\"event\":\"protocol_error\"") != std::string::npos,
         "unsupported-version input is rejected safely");
  expect(controller_events.find("\"event\":\"worker_registered\"") != std::string::npos,
         "valid worker registers after invalid input");
  expect(controller_events.find("\"event\":\"worker_heartbeat\"") != std::string::npos,
         "controller observes at least one heartbeat");
  expect(controller_events.find("STATUS worker=agent-1 health=HEALTHY cpu=2000m memory=4096MiB") !=
             std::string::npos,
         "status command reports the registered worker as healthy");
  expect(controller_events.find("\"event\":\"workload_submitted\"") != std::string::npos,
         "controller records workload submission");
  expect(controller_events.find("\"event\":\"scheduling_decision\"") != std::string::npos,
         "controller records a scheduler decision");
  expect(controller_events.find("\"event\":\"launch_accepted\"") != std::string::npos,
         "worker accepts the remote launch");
  expect(controller_events.find("\"event\":\"replica_running\"") != std::string::npos,
         "worker confirms the replica is running");
  expect(controller_events.find("\"event\":\"replica_pending\"") != std::string::npos &&
             controller_events.find("\"cpu_deficit_millicores\":") != std::string::npos,
         "infeasible submission remains pending with a capacity deficit");
  expect(controller_events.find("reserved_cpu=250m") != std::string::npos &&
             controller_events.find("desired=2 pending=1 running=1") != std::string::npos,
         "status includes reservations and desired pending running counts");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
