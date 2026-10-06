#pragma once

#include "mini_cloud/cgroup.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace mini_cloud {

enum class ReplicaState { running, stopped, exited_unexpectedly };

struct ProcessTermination {
  ReplicaState state;
  bool exited_normally;
  int exit_code;
  bool terminated_by_signal;
  int signal_number;
  bool cleanup_complete = true;
};

class ProcessSupervisor {
 public:
  explicit ProcessSupervisor(std::string replica_id);
  ~ProcessSupervisor();

  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;
  ProcessSupervisor(ProcessSupervisor&&) = delete;
  ProcessSupervisor& operator=(ProcessSupervisor&&) = delete;

  [[nodiscard]] bool launch(const std::string& executable, const std::vector<std::string>& arguments,
                            std::unique_ptr<CgroupScope> cgroup = nullptr);
  [[nodiscard]] std::optional<ProcessTermination> poll();
  [[nodiscard]] std::optional<ProcessTermination> stop();

  [[nodiscard]] const std::string& replica_id() const noexcept;
  [[nodiscard]] pid_t pid() const noexcept;
  [[nodiscard]] ReplicaState state() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept;

 private:
  [[nodiscard]] std::optional<ProcessTermination> reap(bool intentional_stop);

  std::string replica_id_;
  pid_t pid_ = -1;
  ReplicaState state_ = ReplicaState::stopped;
  std::unique_ptr<CgroupScope> cgroup_;
  std::optional<ProcessTermination> pending_termination_;
  std::string error_;
};

}  // namespace mini_cloud
