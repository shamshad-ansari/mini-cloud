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
#include <poll.h>
#include <fcntl.h>
#include <vector>

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

namespace {
void scenario(const char* controller_app, const char* worker_app, const std::string& policy, bool reverse) {
  const auto port = available_port();
  expect(port != 0, "localhost port available");
  if (!port) return;
  const auto port_text = std::to_string(port);
  int input[2], output[2];
  if (pipe(input) < 0 || pipe(output) < 0) { expect(false, "pipes created"); return; }
  const auto controller = fork();
  if (controller == 0) {
    dup2(input[0], STDIN_FILENO); dup2(output[1], STDOUT_FILENO);
    close(input[0]); close(input[1]); close(output[0]); close(output[1]);
    execl(controller_app, controller_app, port_text.c_str(), nullptr); _exit(127);
  }
  close(input[0]); close(output[1]);
  const auto ready = connect_localhost(port);
  expect(ready >= 0, "controller ready");
  if (ready >= 0) close(ready);
  std::string events;
  auto wait_for = [&](const std::string& text) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (events.find(text) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
      pollfd fd{output[0], POLLIN, 0};
      if (poll(&fd, 1, 50) > 0 && fd.revents & POLLIN) {
        char buffer[4096]; const auto n = read(output[0], buffer, sizeof(buffer));
        if (n > 0) events.append(buffer, static_cast<std::size_t>(n));
      }
    }
    expect(events.find(text) != std::string::npos, text.c_str());
  };
  auto command = [&](const std::string& text) {
    const auto line = text + "\n";
    expect(write(input[1], line.data(), line.size()) == static_cast<ssize_t>(line.size()), "controller command written");
  };
  std::vector<pid_t> workers;
  for (int n = 0; n < 3; ++n) {
    const std::string id = "agent-" + std::to_string(reverse ? 3 - n : n + 1);
    const auto worker = fork();
    if (worker == 0) {
      close(input[1]); close(output[0]);
      const auto quiet = open("/dev/null", O_WRONLY); dup2(quiet, STDOUT_FILENO); close(quiet);
      execl(worker_app, worker_app, "127.0.0.1", port_text.c_str(), id.c_str(), "1000", "128", "100", nullptr); _exit(127);
    }
    workers.push_back(worker);
    wait_for("\"event\":\"worker_registered\",\"worker_id\":\"" + id + "\"");
  }
  command("submit " + policy + " 500 64 6 -- /bin/sleep 60");
  for (int n = 1; n <= 6; ++n) {
    const int worker = policy == "first-fit" ? (n - 1) / 2 + 1 : (n - 1) % 3 + 1;
    wait_for("\"event\":\"replica_running\",\"workload_id\":\"workload-1\",\"replica_id\":\"workload-1-replica-" + std::to_string(n) + "\",\"worker_id\":\"agent-" + std::to_string(worker) + "\"");
  }
  // Keep unrelated running replicas while the second workload cannot fit.
  command("submit " + policy + " 500 64 1 -- /bin/sleep 60");
  wait_for("\"event\":\"replica_pending\",\"workload_id\":\"workload-2\"");
  command("status"); wait_for("desired=7 pending=1 running=6");
  for (int n = 1; n <= 6; ++n) {
    const int worker = policy == "first-fit" ? (n - 1) / 2 + 1 : (n - 1) % 3 + 1;
    wait_for("REPLICA id=workload-1-replica-" + std::to_string(n) + " worker=agent-" + std::to_string(worker) + " state=RUNNING");
  }
  expect(events.find("\"cpu_deficit_millicores\":500,\"memory_deficit_mib\":64") != std::string::npos, "pending deficit reported");
  command("stop workload-1");
  for (int n = 1; n <= 6; ++n) wait_for("\"event\":\"replica_stopped\",\"workload_id\":\"workload-1\",\"replica_id\":\"workload-1-replica-" + std::to_string(n) + "\"");
  wait_for("\"event\":\"replica_running\",\"workload_id\":\"workload-2\"");
  command("stop workload-2");
  wait_for("\"event\":\"replica_stopped\",\"workload_id\":\"workload-2\"");
  command("stop workload-1\nstop workload-2\nstatus");
  wait_for("desired=0 pending=0 running=0");
  wait_for("WORKLOAD id=workload-1 desired=0 state=STOPPED");
  wait_for("WORKLOAD id=workload-2 desired=0 state=STOPPED");
  command("submit " + policy + " 500 64 7 -- /bin/sleep 60");
  for (int n = 1; n <= 6; ++n) wait_for("\"event\":\"replica_running\",\"workload_id\":\"workload-3\",\"replica_id\":\"workload-3-replica-" + std::to_string(n) + "\"");
  wait_for("\"event\":\"replica_pending\",\"workload_id\":\"workload-3\",\"replica_id\":\"workload-3-replica-7\"");
  command("status");
  wait_for("REPLICA id=workload-3-replica-7 worker=none state=PENDING");
  expect(events.find("desired=7 pending=1 running=6") != std::string::npos, "partially feasible workload keeps six replicas running");
  command("stop workload-3");
  for (int n = 1; n <= 6; ++n) wait_for("\"event\":\"replica_stopped\",\"workload_id\":\"workload-3\",\"replica_id\":\"workload-3-replica-" + std::to_string(n) + "\"");
  const auto final_stop_offset = events.size();
  command("stop workload-3\nstatus");
  wait_for("WORKLOAD id=workload-3 desired=0 state=STOPPED");
  wait_for("REPLICA id=workload-3-replica-7 worker=none state=STOPPED");
  // Allow several reconciliation cycles, then inspect fresh status.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  command("status\nquit"); close(input[1]);
  events += read_all(output[0]); close(output[0]);
  int status = 0; waitpid(controller, &status, 0);
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "controller exits cleanly");
  for (const auto worker : workers) if (worker > 0) { waitpid(worker, &status, 0); expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "worker exits cleanly"); }
  expect(events.substr(final_stop_offset).find("\"event\":\"scheduling_decision\"") == std::string::npos, "stopped replicas never replaced");
  expect(events.substr(final_stop_offset).find("reserved_cpu=0m reserved_memory=0MiB") != std::string::npos, "stop releases capacity");
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 3) return EXIT_FAILURE;
  for (const std::string policy : {"first-fit", "least-loaded-dominant-resource"}) {
    scenario(argv[1], argv[2], policy, false);
    scenario(argv[1], argv[2], policy, true);
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
