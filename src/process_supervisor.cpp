#include "mini_cloud/process_supervisor.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>

namespace mini_cloud {

ProcessSupervisor::ProcessSupervisor(std::string replica_id) : replica_id_(std::move(replica_id)) {}

ProcessSupervisor::~ProcessSupervisor() {
  if (pid_ > 0) {
    static_cast<void>(stop());
  }
}

bool ProcessSupervisor::launch(const std::string& executable, const std::vector<std::string>& arguments) {
  if (pid_ > 0 || executable.empty()) return false;

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(const_cast<char*>(executable.c_str()));
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);

  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    execvp(executable.c_str(), argv.data());
    _exit(127);
  }

  pid_ = child;
  state_ = ReplicaState::running;
  return true;
}

std::optional<ProcessTermination> ProcessSupervisor::reap(const bool intentional_stop) {
  if (pid_ <= 0) return std::nullopt;

  int wait_status = 0;
  pid_t result = 0;
  do {
    result = waitpid(pid_, &wait_status, intentional_stop ? 0 : WNOHANG);
  } while (result < 0 && errno == EINTR);

  if (result == 0) return std::nullopt;
  if (result < 0) {
    pid_ = -1;
    state_ = ReplicaState::exited_unexpectedly;
    return ProcessTermination{state_, false, -1, false, 0};
  }

  pid_ = -1;
  state_ = intentional_stop ? ReplicaState::stopped : ReplicaState::exited_unexpectedly;
  return ProcessTermination{state_, WIFEXITED(wait_status),
                            WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1,
                            WIFSIGNALED(wait_status),
                            WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0};
}

std::optional<ProcessTermination> ProcessSupervisor::poll() {
  return reap(false);
}

std::optional<ProcessTermination> ProcessSupervisor::stop() {
  if (pid_ <= 0) return std::nullopt;
  if (kill(pid_, SIGTERM) < 0 && errno != ESRCH) return std::nullopt;
  return reap(true);
}

const std::string& ProcessSupervisor::replica_id() const noexcept { return replica_id_; }

pid_t ProcessSupervisor::pid() const noexcept { return pid_; }

ReplicaState ProcessSupervisor::state() const noexcept { return state_; }

}  // namespace mini_cloud
