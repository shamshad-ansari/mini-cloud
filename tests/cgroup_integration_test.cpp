#include "mini_cloud/cgroup.hpp"
#include "mini_cloud/process_supervisor.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int failures = 0;
void expect(bool condition, const char* text) { if (!condition) { ++failures; std::cerr << "FAILED: " << text << '\n'; } }
std::string read(const std::filesystem::path& path) { std::ifstream input(path); std::ostringstream value; value << input.rdbuf(); return value.str(); }
std::uint64_t counter(const std::filesystem::path& path, std::string_view name) {
  std::ifstream input(path); std::string key; std::uint64_t value;
  while (input >> key >> value) if (key == name) return value;
  return 0;
}
}

int main(int argc, char* argv[]) {
  if (argc != 2) return EXIT_FAILURE;
#ifndef __linux__
  std::cout << "SKIP: actual cgroup enforcement requires Linux.\n"; return 77;
#else
  const char* delegated = std::getenv("MINI_CLOUD_CGROUP_ROOT");
  if (!delegated && geteuid() != 0) {
    std::cout << "SKIP: run with sudo or set MINI_CLOUD_CGROUP_ROOT to a writable delegated cpu/memory parent.\n";
    return 77;
  }
  const std::filesystem::path base = delegated ? delegated : "/sys/fs/cgroup";
  auto& io = mini_cloud::system_cgroup_io();
  std::string error;
  if (!io.validate_root(base, error)) { std::cerr << error << '\n'; return EXIT_FAILURE; }
  const auto parent = base / ("mini-cloud-test-" + std::to_string(getpid()));
  if (!io.create(parent, error)) { std::cerr << error << '\n'; return EXIT_FAILURE; }
  if (!io.write(parent / "cgroup.subtree_control", "+cpu +memory", error)) {
    std::cerr << error << '\n'; static_cast<void>(io.remove(parent, error)); return EXIT_FAILURE;
  }
  auto run_scope = [&](const std::string& name, const std::string& fixture, std::uint64_t cpu) {
    auto scope = std::make_unique<mini_cloud::CgroupScope>(parent, name);
    const auto path = scope->path();
    if (!scope->configure(cpu, 32)) { std::cerr << scope->error() << '\n'; expect(false, "kernel limit configuration succeeds"); return; }
    expect(read(path / "cpu.max") == std::to_string(cpu * 1000) + " 1000000\n", "actual cpu.max equals admitted quota and period");
    expect(read(path / "memory.max") == "33554432\n", "actual memory.max equals 32 MiB");
    expect(read(path / "memory.swap.max") == "0\n", "actual swap ceiling is zero");
    mini_cloud::ProcessSupervisor process(name);
    if (!process.launch(argv[1], {fixture}, std::move(scope))) { std::cerr << process.error() << '\n'; expect(false, "limited process launches"); return; }
    expect(read(path / "cgroup.procs").find(std::to_string(process.pid()) + "\n") != std::string::npos || fixture == "immediate-exit", "kernel reports replica PID in dedicated scope");
    if (fixture == "cpu-burn") {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (counter(path / "cpu.stat", "nr_throttled") == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      expect(counter(path / "cpu.stat", "nr_throttled") > 0, "CPU-abuse fixture is throttled by kernel");
      const auto terminal = process.stop();
      expect(terminal && terminal->cleanup_complete && terminal->state == mini_cloud::ReplicaState::stopped, "explicit stop confirms cleanup");
    } else {
      // Observe exit without reaping so memory.events can be inspected before runtime cleanup.
      siginfo_t info{};
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (info.si_pid == 0 && std::chrono::steady_clock::now() < deadline) {
        if (waitid(P_PID, process.pid(), &info, WEXITED | WNOHANG | WNOWAIT) < 0) break;
        if (info.si_pid == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      expect(info.si_pid == process.pid(), "fixture terminates before deadline");
      if (fixture == "memory-abuse") {
        expect(info.si_code == CLD_KILLED && info.si_status == SIGKILL, "memory abuse is terminated by SIGKILL");
        expect(counter(path / "memory.events", "oom_kill") > 0, "kernel attributes termination to cgroup OOM");
      }
      const auto terminal = process.poll();
      expect(terminal && terminal->cleanup_complete && terminal->state == mini_cloud::ReplicaState::exited_unexpectedly, "unexpected exit confirms cleanup");
    }
    expect(!std::filesystem::exists(path), "terminal replica scope is removed");
  };
  run_scope("cpu", "cpu-burn", 250);
  run_scope("memory", "memory-abuse", 1000);
  run_scope("normal-exit", "immediate-exit", 500);
  expect(io.remove(parent, error), "test parent is removed");
  if (!error.empty()) std::cerr << error << '\n';
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}
