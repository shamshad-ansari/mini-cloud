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
#include <stdexcept>
#ifdef __linux__
#include <sys/prctl.h>
#endif

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

namespace {
class Cluster {
 public:
  Cluster(const char* controller_app, const char* worker_app) : worker_app_(worker_app) {
    const auto port = available_port();
    if (!port) throw std::runtime_error("localhost port unavailable");
    port_ = std::to_string(port);
    int input[2], output[2];
    if (pipe(input) < 0 || pipe(output) < 0) throw std::runtime_error("pipe failed");
    controller_ = fork();
    if (controller_ == 0) {
      dup2(input[0], STDIN_FILENO); dup2(output[1], STDOUT_FILENO);
      close(input[0]); close(input[1]); close(output[0]); close(output[1]);
      execl(controller_app, controller_app, port_.c_str(), "400", nullptr); _exit(127);
    }
    close(input[0]); close(output[1]); input_ = input[1]; output_ = output[0];
    if (controller_ < 0) throw std::runtime_error("controller fork failed");
    const int ready = connect_localhost(port);
    if (ready >= 0) close(ready);
    expect(ready >= 0, "controller starts before deadline");
  }
  ~Cluster() {
    command("quit"); close(input_);
    // Each agent and its fixtures have a separate group, including orphaned fixtures
    // left by SIGKILL. Do not accidentally leave these processes behind after a test.
    for (const auto pid : workers_) {
      kill(-pid, SIGTERM); kill(-pid, SIGCONT);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    while (waitpid(controller_, &status, WNOHANG) == 0) {
      pump(20);
      if (std::chrono::steady_clock::now() >= deadline) { kill(controller_, SIGKILL); waitpid(controller_, &status, 0); break; }
    }
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "controller exits cleanly");
    for (const auto pid : workers_) {
      kill(-pid, SIGKILL);
      // On Linux the test is a subreaper, so this also reaps orphaned fixtures.
      while (waitpid(-pid, &status, 0) > 0 || errno == EINTR) {}
    }
    close(output_);
  }
  pid_t worker(const std::string& id, bool full = false) {
    const auto pid = fork();
    if (pid == 0) {
      setpgid(0, 0);
      close(input_); close(output_);
      const int quiet = open("/dev/null", O_WRONLY); dup2(quiet, STDOUT_FILENO); close(quiet);
      execl(worker_app_, worker_app_, "127.0.0.1", port_.c_str(), id.c_str(),
            full ? "500" : "1000", full ? "64" : "128", "30", "--no-cgroups", nullptr); _exit(127);
    }
    if (pid <= 0) throw std::runtime_error("worker fork failed");
    setpgid(pid, pid); workers_.push_back(pid);
    require("\"event\":\"worker_registered\",\"worker_id\":\"" + id + "\"");
    return pid;
  }
  void command(const std::string& text) {
    const std::string line = text + "\n";
    expect(write(input_, line.data(), line.size()) == static_cast<ssize_t>(line.size()), "command written");
  }
  bool wait_for(const std::string& text, std::size_t offset = 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto complete = [&] {
      const auto position = events.find(text, offset);
      return position != std::string::npos && events.find('\n', position + text.size()) != std::string::npos;
    };
    while (!complete() && std::chrono::steady_clock::now() < deadline) pump(50);
    return complete();
  }
  void require(const std::string& text, std::size_t offset = 0) {
    if (!wait_for(text, offset)) throw std::runtime_error("deadline waiting for " + text + "\n" + events);
  }
  void observe(int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) pump(20);
  }
  std::string events;
 private:
  void pump(int timeout) {
    pollfd fd{output_, POLLIN, 0};
    if (poll(&fd, 1, timeout) > 0 && fd.revents & POLLIN) {
      char buffer[4096]; const auto n = read(output_, buffer, sizeof(buffer));
      if (n > 0) events.append(buffer, static_cast<std::size_t>(n));
    }
  }
  const char* worker_app_;
  std::string port_;
  int input_ = -1, output_ = -1;
  pid_t controller_ = -1;
  std::vector<pid_t> workers_;
};

std::string replica_event(const std::string& name, int replica, const std::string& worker = {}) {
  auto result = "\"event\":\"" + name + "\",\"workload_id\":\"workload-1\",\"replica_id\":\"workload-1-replica-" + std::to_string(replica) + "\"";
  if (!worker.empty()) result += ",\"worker_id\":\"" + worker + "\"";
  return result;
}

void once(const std::string& events, const std::string& text) {
  const auto first = events.find(text);
  expect(first != std::string::npos && events.find(text, first + text.size()) == std::string::npos,
         ("exactly one " + text).c_str());
}

void recovery(const char* controller_app, const char* worker_app, const std::string& policy, bool timeout) {
  Cluster cluster(controller_app, worker_app);
  const auto victim = cluster.worker("agent-1");
  cluster.worker("agent-2"); cluster.worker("agent-3");
  cluster.command("submit " + policy + " 500 64 3 -- /bin/sleep 60");
  const bool first_fit = policy == "first-fit";
  for (int n = 1; n <= 3; ++n) cluster.require(replica_event("replica_running", n,
      "agent-" + std::to_string(first_fit ? (n - 1) / 2 + 1 : n)));
  const auto failure_offset = cluster.events.size();
  const auto failed_at = std::chrono::steady_clock::now();
  expect(kill(victim, timeout ? SIGSTOP : SIGKILL) == 0, "hosting agent killed or heartbeat sender suspended");
  const auto death = "\"event\":\"worker_dead\",\"worker_id\":\"agent-1\",\"reason\":\"" +
                     std::string(timeout ? "heartbeat_timeout" : "tcp_loss") + "\"";
  cluster.require(death);
  if (timeout) expect(std::chrono::steady_clock::now() - failed_at >= std::chrono::milliseconds(300), "heartbeat detection waits for configured timeout");
  for (int n = 1; n <= (first_fit ? 2 : 1); ++n) {
    const std::string target = "agent-" + std::to_string(n + 1);
    const auto loss = replica_event("replica_lost", n, "agent-1");
    cluster.require(loss);
    std::size_t previous = cluster.events.find(death);
    std::uint64_t previous_timestamp = 0;
    for (const std::string name : {"replica_lost", "replacement_scheduled", "replacement_launch_issued", "replacement_launch_accepted", "replacement_running"}) {
      const auto text = name == "replica_lost" ? loss : replica_event(name, n, target);
      cluster.require(text);
      const auto position = cluster.events.find(text);
      expect(position > previous, "death, loss, scheduling, issuance, acceptance, and restoration occur in order");
      previous = position;
      const auto end = cluster.events.find('\n', position);
      const auto start = cluster.events.rfind('\n', position);
      const auto line = cluster.events.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1));
      expect(line.starts_with("{\"timestamp_ms\":") && line.find("\"attempt\":1") != std::string::npos,
             "replacement events have timestamps, stable replica IDs, and attempt IDs");
      const auto timestamp = std::stoull(line.substr(std::string("{\"timestamp_ms\":").size()));
      expect(timestamp > 0 && timestamp >= previous_timestamp, "replacement event timestamps preserve ordering");
      previous_timestamp = timestamp;
    }
  }
  cluster.command("status");
  cluster.require("desired=3 pending=0 running=3", failure_offset);
  cluster.require("STATUS worker=agent-1 health=DEAD", failure_offset);
  cluster.require("partition_safe=false exactly_once=false duplicate_execution_possible=true");
  // Resume a timed-out agent; its old connection cannot revive the dead worker.
  if (timeout) kill(victim, SIGCONT);
  cluster.observe(900);
  once(cluster.events, "\"event\":\"worker_dead\",\"worker_id\":\"agent-1\"");
  once(cluster.events, replica_event("replacement_running", 1));
  expect(cluster.events.substr(failure_offset).find(replica_event("replacement_running", 3)) == std::string::npos,
         "unaffected replicas keep their placement");
  const auto stop_offset = cluster.events.size();
  cluster.command("stop workload-1");
  for (int n = 1; n <= 3; ++n) cluster.require(replica_event("replica_stopped", n), stop_offset);
  cluster.command("stop workload-1\nstatus");
  cluster.require("desired=0 pending=0 running=0", stop_offset);
  cluster.observe(500);
  expect(cluster.events.substr(stop_offset).find("\"event\":\"replacement_scheduled\"") == std::string::npos,
         "stopping recovered replicas prevents further replacements");
}

void no_capacity(const char* controller_app, const char* worker_app) {
  Cluster cluster(controller_app, worker_app);
  const auto victim = cluster.worker("agent-1", true);
  cluster.worker("agent-2", true); cluster.worker("agent-3", true);
  cluster.command("submit first-fit 500 64 3 -- /bin/sleep 60");
  for (int n = 1; n <= 3; ++n) cluster.require(replica_event("replica_running", n, "agent-" + std::to_string(n)));
  const auto failure_offset = cluster.events.size();
  kill(victim, SIGKILL);
  cluster.require(replica_event("replica_lost", 1));
  cluster.require(replica_event("replica_pending", 1));
  cluster.command("status");
  cluster.require("desired=3 pending=1 running=2", failure_offset);
  cluster.require("REPLICA id=workload-1-replica-1 worker=agent-1 state=LOST");
  expect(cluster.events.find("\"cpu_deficit_millicores\":500,\"memory_deficit_mib\":64") != std::string::npos,
         "lost replica exposes healthy capacity deficit");
  const auto stop_offset = cluster.events.size();
  cluster.command("stop workload-1");
  cluster.require(replica_event("replica_stopped", 2), stop_offset);
  cluster.require(replica_event("replica_stopped", 3), stop_offset);
  cluster.worker("agent-4", true);
  cluster.command("status"); cluster.require("desired=0 pending=0 running=0", stop_offset);
  cluster.observe(900);
  expect(cluster.events.find("\"event\":\"replacement_scheduled\"") == std::string::npos,
         "lost replica is not recreated after explicit stop, even when new capacity arrives");
  once(cluster.events, "\"event\":\"worker_dead\",\"worker_id\":\"agent-1\"");
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 3) return EXIT_FAILURE;
  std::signal(SIGPIPE, SIG_IGN);
#ifdef __linux__
  // Reap orphaned fixture children from agent crash tests during cleanup.
  if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) return EXIT_FAILURE;
#endif
  try {
    recovery(argv[1], argv[2], "first-fit", false);
    recovery(argv[1], argv[2], "least-loaded-dominant-resource", false);
    recovery(argv[1], argv[2], "least-loaded-dominant-resource", true);
    no_capacity(argv[1], argv[2]);
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
