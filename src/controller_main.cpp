#include "mini_cloud/resources.hpp"
#include "mini_cloud/scheduler.hpp"
#include "mini_cloud/worker_protocol.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
struct WorkerRecord { std::string id; mini_cloud::WorkerResources resources; std::chrono::steady_clock::time_point heartbeat; };
struct Replica { std::string id; enum class State { pending, launching, running } state = State::pending; };
struct Workload { std::string id; mini_cloud::Resources request; std::vector<Replica> replicas; };

std::optional<std::uint64_t> number(std::string_view text) {
  std::uint64_t value{}; const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}
int listen_local(std::uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
  int enabled = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
#ifdef __APPLE__
  address.sin_len = sizeof(address);
#endif
  address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(fd, 4) < 0) { close(fd); return -1; }
  return fd;
}
bool send_line(int fd, const std::string& line) {
  const std::string message = line + '\n'; std::size_t sent_total = 0;
  while (sent_total < message.size()) { const auto sent = send(fd, message.data() + sent_total, message.size() - sent_total, 0); if (sent <= 0) return false; sent_total += static_cast<std::size_t>(sent); }
  return true;
}
bool healthy(const std::optional<WorkerRecord>& worker, int fd) { return worker && fd >= 0 && std::chrono::steady_clock::now() - worker->heartbeat < std::chrono::seconds(2); }
void event(std::string_view name, std::string_view fields = {}) { std::cout << "{\"timestamp_ms\":" << mini_cloud::timestamp_milliseconds() << ",\"event\":\"" << name << "\"" << fields << "}" << std::endl; }
void usage(const char* app) { std::cerr << "Usage: " << app << " <localhost-port>\nCommands: status, submit <policy> <cpu-millicores> <memory-mib> <replicas> -- <executable> [arguments...], quit\n"; }
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) { usage(argv[0]); return 2; }
  const auto port = number(argv[1]);
  if (!port || *port == 0 || *port > 65535) { std::cerr << "Port must be an integer from 1 through 65535.\n"; return 2; }
  std::signal(SIGPIPE, SIG_IGN);
  const int listener = listen_local(static_cast<std::uint16_t>(*port));
  if (listener < 0) { std::cerr << "Unable to listen on localhost.\n"; return 1; }
  int client = -1; bool stdin_open = true; bool keep_running = true; std::string buffer;
  std::optional<WorkerRecord> worker; std::vector<Workload> workloads; std::uint64_t workload_number = 1;
  while (keep_running) {
    const int old_client = client; std::vector<pollfd> fds{{listener, POLLIN, 0}}; if (old_client >= 0) fds.push_back({old_client, POLLIN, 0}); if (stdin_open) fds.push_back({STDIN_FILENO, POLLIN, 0});
    if (poll(fds.data(), fds.size(), -1) < 0) continue;
    std::size_t index = 0;
    if (fds[index++].revents & POLLIN) { int accepted = accept(listener, nullptr, nullptr); if (accepted >= 0) { if (client >= 0) close(client); client = accepted; buffer.clear(); } }
    if (old_client >= 0) {
      const auto revents = fds[index++].revents;
      if (client == old_client && revents & (POLLIN | POLLHUP | POLLERR)) {
        char chunk[2048]; const auto received = recv(client, chunk, sizeof(chunk), 0);
        if (received <= 0) { if (worker) event("worker_disconnected", ",\"worker_id\":\"" + worker->id + "\""); close(client); client = -1; buffer.clear(); }
        else {
          buffer.append(chunk, static_cast<std::size_t>(received));
          while (client >= 0) {
            const auto newline = buffer.find('\n'); if (newline == std::string::npos) break;
            const auto message = mini_cloud::parse_worker_message(buffer.substr(0, newline)); buffer.erase(0, newline + 1);
            if (!message || (message->type != mini_cloud::WorkerMessageType::registration && (!worker || worker->id != message->worker_id))) { event("protocol_error", ",\"worker_id\":\"" + (worker ? worker->id : "unknown") + "\""); close(client); client = -1; break; }
            if (message->type == mini_cloud::WorkerMessageType::registration) { worker = WorkerRecord{message->worker_id, {{message->cpu_millicores, message->memory_mib}, {0, 0}}, std::chrono::steady_clock::now()}; event("worker_registered", ",\"worker_id\":\"" + worker->id + "\""); }
            else if (message->type == mini_cloud::WorkerMessageType::heartbeat) { worker->heartbeat = std::chrono::steady_clock::now(); event("worker_heartbeat", ",\"worker_id\":\"" + worker->id + "\""); }
            else {
              for (auto& workload : workloads) if (workload.id == message->workload_id) for (auto& replica : workload.replicas) if (replica.id == message->replica_id) {
                if (message->type == mini_cloud::WorkerMessageType::replica_running) replica.state = Replica::State::running;
                event(message->type == mini_cloud::WorkerMessageType::launch_accepted ? "launch_accepted" : "replica_running", ",\"worker_id\":\"" + worker->id + "\",\"workload_id\":\"" + workload.id + "\",\"replica_id\":\"" + replica.id + "\"");
              }
            }
          }
        }
      }
    }
    if (stdin_open) {
      const auto revents = fds[index].revents;
      if (revents & (POLLIN | POLLHUP | POLLERR)) {
        std::string command; if (!std::getline(std::cin, command)) { stdin_open = false; continue; }
        if (command == "quit") { keep_running = false; continue; }
        if (command == "status") {
          std::size_t desired = 0, pending = 0, running = 0; for (const auto& w : workloads) for (const auto& r : w.replicas) { ++desired; pending += r.state == Replica::State::pending; running += r.state == Replica::State::running; }
          if (!worker) std::cout << "STATUS worker=none health=UNHEALTHY desired=" << desired << " pending=" << pending << " running=" << running << '\n';
          else std::cout << "STATUS worker=" << worker->id << " health=" << (healthy(worker, client) ? "HEALTHY" : "UNHEALTHY") << " cpu=" << worker->resources.capacity.cpu_millicores << "m memory=" << worker->resources.capacity.memory_mib << "MiB reserved_cpu=" << worker->resources.reserved.cpu_millicores << "m reserved_memory=" << worker->resources.reserved.memory_mib << "MiB desired=" << desired << " pending=" << pending << " running=" << running << '\n';
          std::cout.flush(); continue;
        }
        std::istringstream stream(command); std::string verb, policy_text, cpu_text, memory_text, count_text, delimiter, executable;
        stream >> verb >> policy_text >> cpu_text >> memory_text >> count_text >> delimiter >> executable;
        const auto policy = mini_cloud::parse_scheduling_policy(policy_text); const auto cpu = number(cpu_text); const auto memory = number(memory_text); const auto count = number(count_text);
        if (verb != "submit" || !policy || !cpu || !memory || !count || *cpu == 0 || *memory == 0 || *count == 0 || delimiter != "--" || executable.empty()) { std::cerr << "Invalid submit command.\n"; continue; }
        std::vector<std::string> arguments; for (std::string argument; stream >> argument;) arguments.push_back(argument);
        Workload workload{"workload-" + std::to_string(workload_number++), {*cpu, *memory}, {}}; event("workload_submitted", ",\"workload_id\":\"" + workload.id + "\",\"desired_replicas\":" + std::to_string(*count));
        for (std::uint64_t n = 1; n <= *count; ++n) {
          Replica replica{workload.id + "-replica-" + std::to_string(n)};
          const std::vector<mini_cloud::Worker> candidates = worker ? std::vector<mini_cloud::Worker>{{worker->id, worker->resources}} : std::vector<mini_cloud::Worker>{};
          if (!healthy(worker, client) || !mini_cloud::choose_worker(*policy, candidates, workload.request)) {
            mini_cloud::CapacityDeficit deficit{*cpu, *memory}; if (worker) deficit = mini_cloud::capacity_deficit(worker->resources, workload.request);
            event("replica_pending", ",\"workload_id\":\"" + workload.id + "\",\"replica_id\":\"" + replica.id + "\",\"cpu_deficit_millicores\":" + std::to_string(deficit.cpu_millicores) + ",\"memory_deficit_mib\":" + std::to_string(deficit.memory_mib));
          } else {
            replica.state = Replica::State::launching; worker->resources.reserved = *mini_cloud::checked_add(worker->resources.reserved, workload.request);
            event("scheduling_decision", ",\"policy\":\"" + std::string(mini_cloud::scheduling_policy_name(*policy)) + "\",\"worker_id\":\"" + worker->id + "\",\"workload_id\":\"" + workload.id + "\",\"replica_id\":\"" + replica.id + "\"");
            if (!send_line(client, mini_cloud::launch_message(workload.id, replica.id, executable, arguments, *cpu, *memory))) replica.state = Replica::State::pending;
          }
          workload.replicas.push_back(std::move(replica));
        }
        workloads.push_back(std::move(workload));
      }
    }
  }
  if (client >= 0) close(client); close(listener); return 0;
}
