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
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>

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

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 4 && argc != 5) return EXIT_FAILURE;
  const bool enforced = argc == 5 && std::string(argv[4]) == "--enforced";
  const char* delegated = std::getenv("MINI_CLOUD_CGROUP_ROOT");
  if (enforced && !delegated && geteuid() != 0) {
    std::cout << "SKIP: run with sudo or set MINI_CLOUD_CGROUP_ROOT to a delegated cpu/memory parent.\n";
    return 77;
  }
  const auto port = available_port();
  if (!port) return EXIT_FAILURE;
  const auto port_text = std::to_string(port);
  const std::string root = delegated ? delegated : "/sys/fs/cgroup";
  int input[2], output[2];
  if (pipe(input) < 0 || pipe(output) < 0) return EXIT_FAILURE;
  const auto controller = fork();
  if (controller == 0) {
    dup2(input[0], STDIN_FILENO); dup2(output[1], STDOUT_FILENO);
    close(input[0]); close(input[1]); close(output[0]); close(output[1]);
    execl(argv[1], argv[1], port_text.c_str(), nullptr); _exit(127);
  }
  close(input[0]); close(output[1]);
  const int ready = connect_localhost(port); if (ready >= 0) close(ready);
  expect(ready >= 0, "controller starts");
  const auto worker = fork();
  if (worker == 0) {
    close(input[1]); close(output[0]);
    const int quiet = open("/dev/null", O_WRONLY); dup2(quiet, STDOUT_FILENO); close(quiet);
    if (enforced) execl(argv[2], argv[2], "127.0.0.1", port_text.c_str(), "accounting", "500", "32", "30", "--cgroup-root", root.c_str(), nullptr);
    else execl(argv[2], argv[2], "127.0.0.1", port_text.c_str(), "accounting", "500", "32", "30", "--no-cgroups", nullptr);
    _exit(127);
  }
  std::signal(SIGPIPE, SIG_IGN);
  std::string events;
  auto pump = [&](int timeout) {
    pollfd fd{output[0], POLLIN, 0};
    if (poll(&fd, 1, timeout) > 0 && fd.revents & POLLIN) {
      char buffer[4096]; const auto n = read(output[0], buffer, sizeof(buffer));
      if (n > 0) events.append(buffer, static_cast<std::size_t>(n));
    }
  };
  auto wait_for = [&](const std::string& text, std::size_t offset = 0) {
    if (failures) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while ((events.find(text, offset) == std::string::npos || events.find('\n', events.find(text, offset)) == std::string::npos) && std::chrono::steady_clock::now() < deadline) pump(50);
    const bool found = events.find(text, offset) != std::string::npos;
    expect(found, text.c_str()); return found;
  };
  auto command = [&](const std::string& text) { const auto line = text + "\n"; expect(write(input[1], line.data(), line.size()) == static_cast<ssize_t>(line.size()), "command written"); };
  auto event = [](const std::string& name, int workload) { return "\"event\":\"" + name + "\",\"workload_id\":\"workload-" + std::to_string(workload) + "\""; };
  if (wait_for("\"event\":\"worker_registered\",\"worker_id\":\"accounting\"")) {
    command("submit first-fit 250 32 1 -- /bin/sleep 30");
    wait_for(event("replica_running", 1)); command("status");
    const std::string status = "REPLICA id=workload-1-replica-1 worker=accounting state=RUNNING cgroup=" + std::string(enforced ? "ENFORCED" : "DISABLED") + " cgroup_path=";
    wait_for(status);
    std::filesystem::path path;
    if (enforced && events.find(status) != std::string::npos) {
      const auto start = events.find(status) + status.size();
      path = events.substr(start, events.find('\n', start) - start);
      std::ifstream cpu(path / "cpu.max"), memory(path / "memory.max");
      std::string cpu_value; std::getline(cpu, cpu_value); std::string memory_value; std::getline(memory, memory_value);
      expect(cpu_value == "250000 1000000" && memory_value == "33554432", "worker applies exact admitted limits on VM");
    }
    // Full memory capacity prevents workload 2 from starting until stop cleans up.
    command("submit first-fit 250 32 1 -- " + std::string(argv[3]) + " immediate-exit");
    wait_for(event("replica_pending", 2)); command("stop workload-1");
    wait_for(event("replica_stopped", 1)); wait_for(event("replica_running", 2)); wait_for(event("replica_exited", 2));
    if (enforced) expect(!std::filesystem::exists(path), "explicit stop removes worker's actual cgroup");
    std::size_t offset = events.size(); command("status");
    wait_for("reserved_cpu=0m reserved_memory=0MiB", offset);
    wait_for("REPLICA id=workload-2-replica-1 worker=accounting state=EXITED cgroup=RELEASED", offset);
    if (enforced) {
      command("submit first-fit 250 32 1 -- " + std::string(argv[3]) + " memory-abuse");
      wait_for(event("replica_running", 3)); wait_for(event("replica_exited", 3));
      const auto start = events.find(event("replica_exited", 3));
      expect(start != std::string::npos && events.substr(start, events.find('\n', start) - start).find("\"signal\":9") != std::string::npos,
             "worker reports memory OOM termination");
    } else {
      command("submit first-fit 250 32 1 -- " + std::string(argv[3]) + " immediate-exit");
      wait_for(event("replica_exited", 3));
    }
    command("submit first-fit 250 32 1 -- /does/not/exist"); wait_for(event("replica_launch_failed", 4));
    offset = events.size(); command("status"); wait_for("reserved_cpu=0m reserved_memory=0MiB", offset);
    wait_for("REPLICA id=workload-4-replica-1 worker=accounting state=FAILED", offset);
    command("submit first-fit 250 32 1 -- /bin/sleep 30"); wait_for(event("replica_running", 5));
    command("stop workload-5\nstop workload-5"); wait_for(event("replica_stopped", 5));
    offset = events.size(); command("status"); wait_for("reserved_cpu=0m reserved_memory=0MiB", offset);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < deadline) pump(20);
    expect(events.find("\"event\":\"worker_dead\"") == std::string::npos, "child exits and rejected launches do not fail worker");
  }
  command("quit"); close(input[1]);
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (waitpid(controller, &status, WNOHANG) == 0) {
    pump(20);
    if (std::chrono::steady_clock::now() >= deadline) { kill(controller, SIGKILL); waitpid(controller, &status, 0); break; }
  }
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "controller exits cleanly");
  close(output[0]); waitpid(worker, &status, 0);
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "worker exits cleanly");
  if (failures) std::cerr << events;
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
