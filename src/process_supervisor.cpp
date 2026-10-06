#include "mini_cloud/process_supervisor.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <utility>
#include <sys/wait.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

namespace mini_cloud {

ProcessSupervisor::ProcessSupervisor(std::string replica_id) : replica_id_(std::move(replica_id)) {}

ProcessSupervisor::~ProcessSupervisor() {
  if (pid_ > 0) {
    static_cast<void>(stop());
  }
}

bool ProcessSupervisor::launch(const std::string& executable, const std::vector<std::string>& arguments,
                               std::unique_ptr<CgroupScope> cgroup) {
  if (pid_ > 0 || pending_termination_ || executable.empty()) return false;
  error_.clear();
  cgroup_ = std::move(cgroup);
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(const_cast<char*>(executable.c_str()));
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);

  int gate[2], exec_status[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, gate) < 0) { error_ = std::strerror(errno); return false; }
  if (pipe(exec_status) < 0) { error_ = std::strerror(errno); close(gate[0]); close(gate[1]); return false; }
  if (fcntl(exec_status[1], F_SETFD, FD_CLOEXEC) < 0) {
    error_ = std::strerror(errno);
    for (const auto fd : {gate[0], gate[1], exec_status[0], exec_status[1]}) close(fd);
    return false;
  }
#ifdef SO_NOSIGPIPE
  int enabled = 1; setsockopt(gate[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
  const pid_t child = fork();
  if (child == 0) {
    close(gate[0]); close(exec_status[0]);
    char ready = 0;
    ssize_t received;
    do { received = read(gate[1], &ready, 1); } while (received < 0 && errno == EINTR);
    close(gate[1]);
    if (received != 1 || ready != 1) _exit(126);
    execvp(executable.c_str(), argv.data());
    const int failure = errno;
    static_cast<void>(write(exec_status[1], &failure, sizeof(failure)));
    _exit(127);
  }
  close(gate[1]); close(exec_status[1]);
  bool ready = child > 0;
  if (!ready) error_ = std::strerror(errno);
  if (ready && cgroup_ && !cgroup_->attach(child)) { ready = false; error_ = cgroup_->error(); }
  if (ready) {
    const char release = 1;
#ifdef MSG_NOSIGNAL
    ready = send(gate[0], &release, 1, MSG_NOSIGNAL) == 1;
#else
    ready = send(gate[0], &release, 1, 0) == 1;
#endif
    if (!ready) error_ = "Cannot release child after cgroup attachment: " + std::string(std::strerror(errno));
  }
  close(gate[0]);
  int exec_error = 0;
  ssize_t received = 0;
  if (ready) {
    do { received = read(exec_status[0], &exec_error, sizeof(exec_error)); } while (received < 0 && errno == EINTR);
    if (received != 0) { ready = false; error_ = "Cannot execute " + executable + ": " + std::strerror(received > 0 ? exec_error : errno); }
  }
  close(exec_status[0]);
  if (!ready) {
    if (child > 0) {
      kill(child, SIGKILL);
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
    if (cgroup_ && !cgroup_->cleanup()) error_ += "; cleanup failed: " + cgroup_->error();
    cgroup_.reset();
    return false;
  }
  pid_ = child;
  state_ = ReplicaState::running;
  return true;
}

std::optional<ProcessTermination> ProcessSupervisor::reap(const bool intentional_stop) {
  if (pid_ <= 0) {
    if (!pending_termination_) return std::nullopt;
    if (cgroup_ && !cgroup_->cleanup()) { error_ = cgroup_->error(); return pending_termination_; }
    auto termination = *pending_termination_;
    termination.cleanup_complete = true;
    pending_termination_.reset(); cgroup_.reset(); error_.clear();
    return termination;
  }

  int wait_status = 0;
  pid_t result = 0;
  do {
    result = waitpid(pid_, &wait_status, intentional_stop ? 0 : WNOHANG);
  } while (result < 0 && errno == EINTR);

  if (result == 0) return std::nullopt;
  pid_ = -1;
  state_ = intentional_stop ? ReplicaState::stopped : ReplicaState::exited_unexpectedly;
  ProcessTermination termination{state_, result > 0 && WIFEXITED(wait_status),
                                result > 0 && WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1,
                                result > 0 && WIFSIGNALED(wait_status),
                                result > 0 && WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0};
  if (cgroup_ && !cgroup_->cleanup()) {
    error_ = cgroup_->error(); termination.cleanup_complete = false; pending_termination_ = termination;
  } else { cgroup_.reset(); error_.clear(); }
  return termination;
}

std::optional<ProcessTermination> ProcessSupervisor::poll() {
  return reap(false);
}

std::optional<ProcessTermination> ProcessSupervisor::stop() {
  if (pid_ <= 0) return reap(true);
  if (kill(pid_, SIGTERM) < 0 && errno != ESRCH) return std::nullopt;
  if (cgroup_ && !cgroup_->kill_all()) { error_ = cgroup_->error(); return std::nullopt; }
  return reap(true);
}

const std::string& ProcessSupervisor::replica_id() const noexcept { return replica_id_; }

pid_t ProcessSupervisor::pid() const noexcept { return pid_; }

ReplicaState ProcessSupervisor::state() const noexcept { return state_; }
const std::string& ProcessSupervisor::error() const noexcept { return error_; }

}  // namespace mini_cloud
