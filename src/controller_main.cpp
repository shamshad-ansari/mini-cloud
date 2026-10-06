#include "mini_cloud/resources.hpp"
#include "mini_cloud/scheduler.hpp"
#include "mini_cloud/worker_protocol.hpp"

#include <algorithm>
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
struct WorkerRecord {
  int fd = -1;
  std::string buffer, id;
  mini_cloud::WorkerResources resources{};
  std::chrono::steady_clock::time_point heartbeat;
  bool dead = false;
};
struct Replica {
  std::string id, worker_id;
  enum class State { pending, launching, running, stopping, stopped, lost, exited, failed } state = State::pending;
  bool pending_reported = false;
  std::uint64_t attempt = 0;
  std::string cgroup_status = "NOT_APPLIED";
  std::string cgroup_path;
};
struct Workload {
  std::string id;
  mini_cloud::Resources request;
  mini_cloud::SchedulingPolicy policy;
  std::string executable;
  std::vector<std::string> arguments;
  std::vector<Replica> replicas;
  bool desired = true;
};

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
bool healthy(const WorkerRecord& worker, std::uint64_t timeout_ms) {
  return !worker.dead && !worker.id.empty() && worker.fd >= 0 &&
         static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - worker.heartbeat).count()) < timeout_ms;
}
void event(std::string_view name, std::string_view fields = {}) { std::cout << "{\"timestamp_ms\":" << mini_cloud::timestamp_milliseconds() << ",\"event\":\"" << name << "\"" << fields << "}" << std::endl; }
void usage(const char* app) { std::cerr << "Usage: " << app << " <localhost-port> [heartbeat-timeout-ms]\nCommands: status, stop <workload-id>, submit <policy> <cpu-millicores> <memory-mib> <replicas> -- <executable> [arguments...], quit\n"; }
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2 && argc != 3) { usage(argv[0]); return 2; }
  const auto port = number(argv[1]);
  if (!port || *port == 0 || *port > 65535) { std::cerr << "Port must be an integer from 1 through 65535.\n"; return 2; }
  const auto timeout_ms = argc == 3 ? number(argv[2]) : std::optional<std::uint64_t>{2000};
  if (!timeout_ms || *timeout_ms == 0) {
    std::cerr << "Heartbeat timeout must be a positive integer in milliseconds.\n";
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);
  const int listener = listen_local(static_cast<std::uint16_t>(*port));
  if (listener < 0) { std::cerr << "Unable to listen on localhost.\n"; return 1; }
  bool stdin_open = true, keep_running = true;
  std::string command_buffer;
  std::vector<WorkerRecord> workers;
  std::vector<Workload> workloads;
  std::uint64_t workload_number = 1;
  auto fields = [](const Workload& w, const Replica& r) {
    return ",\"workload_id\":\"" + w.id + "\",\"replica_id\":\"" + r.id + "\"";
  };
  auto mark_dead = [&](WorkerRecord& worker, std::string_view reason) {
    if (worker.dead) return;
    worker.dead = true;
    if (worker.fd >= 0) close(worker.fd);
    worker.fd = -1;
    if (worker.id.empty()) return;
    event("worker_dead", ",\"worker_id\":\"" + worker.id + "\",\"reason\":\"" + std::string(reason) +
          "\",\"heartbeat_timeout_ms\":" + std::to_string(*timeout_ms));
    for (auto& w : workloads) for (auto& r : w.replicas) {
      r.pending_reported = false;
      if (r.worker_id != worker.id || r.state == Replica::State::stopped ||
          r.state == Replica::State::pending || r.state == Replica::State::lost ||
          r.state == Replica::State::exited || r.state == Replica::State::failed) continue;
      r.state = Replica::State::lost;
      r.cgroup_status = "UNKNOWN";
      ++r.attempt;
      event("replica_lost", fields(w, r) + ",\"worker_id\":\"" + worker.id +
            "\",\"attempt\":" + std::to_string(r.attempt) + ",\"replacement_desired\":" + (w.desired ? "true" : "false"));
    }
    // Dead capacity is unavailable. Old processes may still execute; this is not fencing.
    worker.resources.reserved = {0, 0};
  };
  auto reconcile = [&] {
    // The event loop serializes reservations and launches, one replica at a time.
    for (auto& w : workloads) {
      if (!w.desired) continue;
      for (auto& r : w.replicas) {
        if (r.state != Replica::State::pending && r.state != Replica::State::lost) continue;
        std::vector<mini_cloud::Worker> candidates;
        for (const auto& worker : workers) if (healthy(worker, *timeout_ms)) candidates.push_back({worker.id, worker.resources});
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        const auto selection = mini_cloud::choose_worker(w.policy, candidates, w.request);
        if (!selection) {
          if (!r.pending_reported) {
            if (candidates.empty()) event("replica_pending", fields(w, r) + ",\"cpu_deficit_millicores\":" + std::to_string(w.request.cpu_millicores) + ",\"memory_deficit_mib\":" + std::to_string(w.request.memory_mib));
            for (const auto& candidate : candidates) {
              const auto deficit = mini_cloud::capacity_deficit(candidate.resources, w.request);
              event("replica_pending", fields(w, r) + ",\"worker_id\":\"" + candidate.id + "\",\"cpu_deficit_millicores\":" + std::to_string(deficit.cpu_millicores) + ",\"memory_deficit_mib\":" + std::to_string(deficit.memory_mib));
            }
            r.pending_reported = true;
          }
          continue;
        }
        auto& worker = *std::find_if(workers.begin(), workers.end(), [&](const auto& worker) { return worker.id == candidates[*selection].id; });
        worker.resources.reserved = *mini_cloud::checked_add(worker.resources.reserved, w.request);
        r.worker_id = worker.id;
        r.state = Replica::State::launching;
        r.cgroup_status = "NOT_APPLIED"; r.cgroup_path.clear();
        event("scheduling_decision", fields(w, r) + ",\"policy\":\"" + std::string(mini_cloud::scheduling_policy_name(w.policy)) + "\",\"worker_id\":\"" + worker.id + "\"");
        const auto replacement_fields = fields(w, r) + ",\"worker_id\":\"" + worker.id + "\",\"attempt\":" + std::to_string(r.attempt);
        if (r.attempt > 0) event("replacement_scheduled", replacement_fields);
        if (!send_line(worker.fd, mini_cloud::launch_message(w.id, r.id, w.executable, w.arguments, w.request.cpu_millicores, w.request.memory_mib))) {
          mark_dead(worker, "launch_send_failure");
          continue;
        }
        if (r.attempt > 0) event("replacement_launch_issued", replacement_fields);
      }
    }
  };
  while (keep_running) {
    std::vector<pollfd> fds{{listener, POLLIN, 0}};
    const auto worker_count = workers.size();
    for (const auto& worker : workers) fds.push_back({worker.fd, POLLIN, 0});
    if (stdin_open) fds.push_back({STDIN_FILENO, POLLIN, 0});
    if (poll(fds.data(), fds.size(), command_buffer.find('\n') == std::string::npos ? 100 : 0) < 0) continue;
    for (auto& worker : workers) {
      if (!worker.dead && !worker.id.empty() && !healthy(worker, *timeout_ms)) mark_dead(worker, "heartbeat_timeout");
    }
    if (fds[0].revents & POLLIN) {
      int accepted = accept(listener, nullptr, nullptr);
      if (accepted >= 0) { WorkerRecord worker; worker.fd = accepted; workers.push_back(std::move(worker)); }
    }
    for (std::size_t i = 0; i < worker_count; ++i) {
      auto& worker = workers[i];
      if (worker.fd < 0) continue;
      if (!(fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      char chunk[2048]; const auto received = recv(worker.fd, chunk, sizeof(chunk), 0);
      if (received <= 0) { event("worker_disconnected", ",\"worker_id\":\"" + worker.id + "\""); mark_dead(worker, "tcp_loss"); continue; }
      worker.buffer.append(chunk, static_cast<std::size_t>(received));
      while (worker.fd >= 0) {
        const auto newline = worker.buffer.find('\n'); if (newline == std::string::npos) break;
        const auto message = mini_cloud::parse_worker_message(worker.buffer.substr(0, newline));
        worker.buffer.erase(0, newline + 1);
        bool valid = message.has_value();
        if (valid && message->type == mini_cloud::WorkerMessageType::registration) {
          valid = worker.id.empty() && std::none_of(workers.begin(), workers.end(), [&](const auto& other) { return other.id == message->worker_id; });
        } else if (valid) valid = !worker.id.empty() && worker.id == message->worker_id;
        if (!valid) { event("protocol_error"); mark_dead(worker, "protocol_error"); break; }
        if (message->type == mini_cloud::WorkerMessageType::registration) {
          worker.id = message->worker_id;
          worker.resources = {{message->cpu_millicores, message->memory_mib}, {0, 0}};
          worker.heartbeat = std::chrono::steady_clock::now();
          event("worker_registered", ",\"worker_id\":\"" + worker.id + "\"");
          for (auto& w : workloads) for (auto& r : w.replicas) r.pending_reported = false;
        } else if (message->type == mini_cloud::WorkerMessageType::heartbeat) {
          worker.heartbeat = std::chrono::steady_clock::now();
          event("worker_heartbeat", ",\"worker_id\":\"" + worker.id + "\"");
        } else {
          for (auto& w : workloads) if (w.id == message->workload_id) for (auto& r : w.replicas) {
            if (r.id != message->replica_id || r.worker_id != worker.id) continue;
            if (message->type == mini_cloud::WorkerMessageType::replica_stopped ||
                message->type == mini_cloud::WorkerMessageType::replica_exited ||
                message->type == mini_cloud::WorkerMessageType::launch_failed) {
              // Only a live reservation can be released; repeated terminal reports
              // must never subtract it twice or affect another replica.
              if (r.state != Replica::State::launching && r.state != Replica::State::running && r.state != Replica::State::stopping) continue;
              if (message->type == mini_cloud::WorkerMessageType::replica_stopped && r.state != Replica::State::stopping) continue;
              if (message->type == mini_cloud::WorkerMessageType::launch_failed && r.state == Replica::State::running) continue;
              r.state = !w.desired ? Replica::State::stopped :
                  message->type == mini_cloud::WorkerMessageType::launch_failed ? Replica::State::failed : Replica::State::exited;
              r.cgroup_status = message->type == mini_cloud::WorkerMessageType::launch_failed ? "NOT_APPLIED" : "RELEASED";
              worker.resources.reserved.cpu_millicores -= w.request.cpu_millicores;
              worker.resources.reserved.memory_mib -= w.request.memory_mib;
              const auto name = message->type == mini_cloud::WorkerMessageType::replica_stopped ? "replica_stopped" :
                  message->type == mini_cloud::WorkerMessageType::replica_exited ? "replica_exited" : "replica_launch_failed";
              std::string terminal_fields = fields(w, r) + ",\"worker_id\":\"" + worker.id + "\",\"reservation_released\":true";
              if (message->type != mini_cloud::WorkerMessageType::launch_failed) terminal_fields += ",\"cleanup_complete\":true";
              if (message->type == mini_cloud::WorkerMessageType::replica_exited)
                terminal_fields += ",\"exit_code\":" + std::to_string(message->exit_code) + ",\"signal\":" + std::to_string(message->signal);
              if (message->type == mini_cloud::WorkerMessageType::launch_failed)
                terminal_fields += ",\"reason\":\"" + message->reason + "\"";
              event(name, terminal_fields);
              for (auto& pending_w : workloads) for (auto& pending_r : pending_w.replicas) pending_r.pending_reported = false;
            } else if (w.desired && (r.state == Replica::State::launching || r.state == Replica::State::running)) {
              const bool first_running = message->type == mini_cloud::WorkerMessageType::replica_running && r.state == Replica::State::launching;
              if (message->type == mini_cloud::WorkerMessageType::replica_running) {
                r.state = Replica::State::running;
                r.cgroup_status = message->cgroup_enforced ? "ENFORCED" : "DISABLED";
                r.cgroup_path = message->cgroup_path;
                event("replica_enforcement", fields(w, r) + ",\"worker_id\":\"" + worker.id + "\",\"cgroup_enforced\":" + (message->cgroup_enforced ? "true" : "false"));
              }
              event(message->type == mini_cloud::WorkerMessageType::launch_accepted ? "launch_accepted" : "replica_running", fields(w, r) + ",\"worker_id\":\"" + worker.id + "\"");
              if (r.attempt > 0) {
                const auto replacement_fields = fields(w, r) + ",\"worker_id\":\"" + worker.id + "\",\"attempt\":" + std::to_string(r.attempt);
                if (message->type == mini_cloud::WorkerMessageType::launch_accepted) event("replacement_launch_accepted", replacement_fields);
                if (first_running) event("replacement_running", replacement_fields);
              }
            }
          }
        }
      }
    }
    if (stdin_open && fds.back().revents & (POLLIN | POLLHUP | POLLERR)) {
      char chunk[2048];
      const auto received = read(STDIN_FILENO, chunk, sizeof(chunk));
      if (received <= 0) {
        stdin_open = false;
        if (!command_buffer.empty() && command_buffer.back() != '\n') command_buffer += '\n';
      } else command_buffer.append(chunk, static_cast<std::size_t>(received));
    }
    const auto command_end = command_buffer.find('\n');
    if (command_end != std::string::npos) {
      const auto command = command_buffer.substr(0, command_end);
      command_buffer.erase(0, command_end + 1);
      if (command == "quit") keep_running = false;
      else if (command == "status") {
        std::cout << "RECOVERY heartbeat_timeout_ms=" << *timeout_ms
                  << " partition_safe=false exactly_once=false duplicate_execution_possible=true\n";
        std::size_t desired = 0, pending = 0, running = 0, exited = 0, failed = 0;
        for (const auto& w : workloads) for (const auto& r : w.replicas) {
          desired += w.desired; pending += w.desired && (r.state == Replica::State::pending || r.state == Replica::State::lost); running += r.state == Replica::State::running;
          exited += r.state == Replica::State::exited; failed += r.state == Replica::State::failed;
        }
        bool registered = false;
        for (const auto& worker : workers) if (!worker.id.empty()) {
          registered = true;
          std::cout << "STATUS worker=" << worker.id << " health=" << (worker.dead ? "DEAD" : healthy(worker, *timeout_ms) ? "HEALTHY" : "UNHEALTHY") << " cpu=" << worker.resources.capacity.cpu_millicores << "m memory=" << worker.resources.capacity.memory_mib << "MiB reserved_cpu=" << worker.resources.reserved.cpu_millicores << "m reserved_memory=" << worker.resources.reserved.memory_mib << "MiB desired=" << desired << " pending=" << pending << " running=" << running << " exited=" << exited << " failed=" << failed << '\n';
        }
        if (!registered) std::cout << "STATUS worker=none health=UNHEALTHY desired=" << desired << " pending=" << pending << " running=" << running << " exited=" << exited << " failed=" << failed << '\n';
        for (const auto& w : workloads) {
          std::cout << "WORKLOAD id=" << w.id << " desired=" << (w.desired ? w.replicas.size() : 0) << " state=" << (w.desired ? "ACTIVE" : std::all_of(w.replicas.begin(), w.replicas.end(), [](const auto& r) { return r.state == Replica::State::stopped; }) ? "STOPPED" : "STOPPING") << '\n';
          for (const auto& r : w.replicas) {
            const char* names[] = {"PENDING", "LAUNCHING", "RUNNING", "STOPPING", "STOPPED", "LOST", "EXITED", "FAILED"};
            std::cout << "REPLICA id=" << r.id << " worker=" << (r.worker_id.empty() ? "none" : r.worker_id) << " state=" << names[static_cast<int>(r.state)] << " cgroup=" << r.cgroup_status << " cgroup_path=" << (r.cgroup_path.empty() ? "none" : r.cgroup_path) << '\n';
          }
        }
        std::cout.flush();
      } else {
        std::istringstream stream(command); std::string verb; stream >> verb;
        if (verb == "stop") {
          std::string id, extra; stream >> id;
          auto it = std::find_if(workloads.begin(), workloads.end(), [&](const auto& w) { return w.id == id; });
          if (it == workloads.end() || stream >> extra) { std::cerr << "Invalid stop command or unknown workload.\n"; continue; }
          auto& w = *it;
          w.desired = false;
          for (auto& r : w.replicas) {
            if (r.state == Replica::State::pending || r.state == Replica::State::exited || r.state == Replica::State::failed) r.state = Replica::State::stopped;
            else if (r.state != Replica::State::stopped && r.state != Replica::State::lost) {
              r.state = Replica::State::stopping;
              for (const auto& worker : workers) if (worker.id == r.worker_id && worker.fd >= 0) static_cast<void>(send_line(worker.fd, mini_cloud::stop_message(w.id, r.id)));
            }
          }
          event("workload_stopped", ",\"workload_id\":\"" + w.id + "\"");
        } else {
          std::string policy_text, cpu_text, memory_text, count_text, delimiter, executable;
          stream >> policy_text >> cpu_text >> memory_text >> count_text >> delimiter >> executable;
          const auto policy = mini_cloud::parse_scheduling_policy(policy_text); const auto cpu = number(cpu_text); const auto memory = number(memory_text); const auto count = number(count_text);
          if (verb != "submit" || !policy || !cpu || !memory || !count || *cpu == 0 || *memory == 0 || *count == 0 || delimiter != "--" || executable.empty()) { std::cerr << "Invalid submit command.\n"; continue; }
          Workload w{"workload-" + std::to_string(workload_number++), {*cpu, *memory}, *policy, executable, {}, {}, true};
          for (std::string argument; stream >> argument;) w.arguments.push_back(argument);
          for (std::uint64_t n = 0; n < *count; ++n) w.replicas.push_back({w.id + "-replica-" + std::to_string(n + 1), {}});
          event("workload_submitted", ",\"workload_id\":\"" + w.id + "\",\"desired_replicas\":" + std::to_string(*count));
          workloads.push_back(std::move(w));
        }
      }
    }
    reconcile();
  }
  for (const auto& worker : workers) if (worker.fd >= 0) close(worker.fd);
  close(listener);
  return 0;
}
