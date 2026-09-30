#include "mini_cloud/process_supervisor.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const char* description) {
  if (!condition) {
    std::cerr << "FAILED: " << description << '\n';
    ++failures;
  }
}

void expect_reaped(const pid_t pid, const char* description) {
  errno = 0;
  expect(waitpid(pid, nullptr, WNOHANG) == -1 && errno == ECHILD, description);
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc != 2) return EXIT_FAILURE;
  const std::string fixture = argv[1];

  mini_cloud::ProcessSupervisor long_running{"replica-stop"};
  expect(long_running.launch(fixture, {"long-running"}), "long-running child launches");
  const pid_t long_running_pid = long_running.pid();
  expect(long_running_pid > 0 && long_running.state() == mini_cloud::ReplicaState::running,
         "launch reports a running state and PID");
  const auto stopped = long_running.stop();
  expect(stopped && stopped->state == mini_cloud::ReplicaState::stopped,
         "explicit stop reports an intentional terminal state");
  expect(stopped && stopped->terminated_by_signal, "explicit stop terminates the managed child");
  expect_reaped(long_running_pid, "explicitly stopped child is reaped");

  mini_cloud::ProcessSupervisor immediate_exit{"replica-unexpected"};
  expect(immediate_exit.launch(fixture, {"immediate-exit"}), "immediate-exit child launches");
  const pid_t immediate_exit_pid = immediate_exit.pid();
  std::optional<mini_cloud::ProcessTermination> unexpected;
  for (int attempt = 0; attempt < 100 && !unexpected; ++attempt) {
    unexpected = immediate_exit.poll();
    if (!unexpected) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  expect(unexpected && unexpected->state == mini_cloud::ReplicaState::exited_unexpectedly,
         "normal child exit is reported as unexpected");
  expect(unexpected && unexpected->exited_normally && unexpected->exit_code == 0,
         "unexpected exit preserves code zero");
  expect_reaped(immediate_exit_pid, "unexpected child exit is reaped");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
