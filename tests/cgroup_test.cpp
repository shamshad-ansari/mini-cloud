#include "mini_cloud/cgroup.hpp"
#include "mini_cloud/process_supervisor.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <unistd.h>

namespace {
int failures = 0;
void expect(bool ok, const char* description) { if (!ok) { ++failures; std::cerr << "FAILED: " << description << '\n'; } }
class FakeIo final : public mini_cloud::CgroupIo {
 public:
  bool valid = true, occupied = false, removed = false, deny_cleanup = false;
  std::string fail_file;
  std::map<std::string, std::string> controls;
  bool validate_root(const std::filesystem::path&, std::string& error) override { if (!valid) error = "delegation unavailable"; return valid; }
  bool create(const std::filesystem::path&, std::string& error) override { if (occupied) { error = "scope already exists"; return false; } occupied = true; return true; }
  bool write(const std::filesystem::path& path, std::string_view value, std::string& error) override {
    if (path.filename() == fail_file) { error = "permission denied: " + fail_file; return false; }
    controls[path.filename().string()] = value; return true;
  }
  bool remove(const std::filesystem::path&, std::string& error) override {
    if (deny_cleanup) { error = "cleanup denied"; return false; }
    occupied = false; removed = true; return true;
  }
};
}

int main(int argc, char* argv[]) {
  if (argc != 2) return EXIT_FAILURE;
  const auto tiny = mini_cloud::CgroupLimits::from_request(1, 1);
  expect(tiny && tiny->quota_microseconds == 1000 && tiny->memory_bytes == 1048576, "smallest admitted request maps exactly to supported units");
  expect(!mini_cloud::CgroupLimits::from_request(0, 1), "zero CPU rejected");
  expect(!mini_cloud::CgroupLimits::from_request(1, 0), "zero memory rejected");
  expect(!mini_cloud::CgroupLimits::from_request(std::numeric_limits<std::uint64_t>::max(), 1), "CPU conversion overflow rejected");
  expect(!mini_cloud::CgroupLimits::from_request(1, std::numeric_limits<std::uint64_t>::max()), "memory conversion overflow rejected");
  std::string root_error;
  expect(!mini_cloud::system_cgroup_io().validate_root(std::filesystem::temp_directory_path(), root_error) &&
         root_error.find("cgroup v2") != std::string::npos, "ordinary filesystems are rejected with an actionable error");
  FakeIo io;
  mini_cloud::CgroupScope scope("/delegated", "replica-1", io);
  expect(scope.configure(250, 64), "dedicated scope configures");
  expect(io.controls["cpu.max"] == "250000 1000000", "CPU quota matches 250 millicores");
  expect(io.controls["memory.max"] == "67108864", "memory ceiling matches 64 MiB");
  expect(io.controls["memory.swap.max"] == "0" && io.controls["memory.oom.group"] == "1", "swap is disabled and OOM applies to whole replica");
  expect(scope.attach(123), "replica PID attached");
  expect(io.controls["cgroup.procs"] == "123", "attachment writes child PID");
  expect(scope.cleanup() && io.removed && scope.cleanup(), "cleanup removes scope and is idempotent");
  FakeIo denied; denied.valid = false;
  mini_cloud::CgroupScope invalid("/delegated", "replica-2", denied);
  expect(!invalid.configure(250, 64) && !denied.occupied && !invalid.error().empty(), "missing delegation fails before creating scope");
  FakeIo partial; partial.fail_file = "memory.max";
  mini_cloud::CgroupScope failed("/delegated", "replica-3", partial);
  expect(!failed.configure(250, 64) && partial.removed && failed.error().find("memory.max") != std::string::npos, "partial configuration rolls back and preserves actionable failure");
  FakeIo collision; collision.occupied = true;
  { mini_cloud::CgroupScope existing("/delegated", "replica-4", collision); expect(!existing.configure(250, 64), "existing scopes are never reused"); }
  expect(!collision.removed, "failed creation does not remove another replica's scope");
  FakeIo unsafe;
  mini_cloud::CgroupScope traversal("/delegated", "../escape", unsafe);
  expect(!traversal.configure(250, 64) && !unsafe.occupied, "scope names cannot escape parent");

  FakeIo lifecycle;
  auto limits = std::make_unique<mini_cloud::CgroupScope>("/delegated", "replica-runtime", lifecycle);
  expect(limits->configure(100, 32), "runtime scope configures");
  mini_cloud::ProcessSupervisor process("replica-runtime");
  expect(process.launch(argv[1], {"long-running"}, std::move(limits)), "child launches through attachment gate");
  expect(lifecycle.controls["cgroup.procs"] == std::to_string(process.pid()), "runtime attaches child before launch returns");
  lifecycle.deny_cleanup = true;
  const auto blocked = process.stop();
  expect(blocked && !blocked->cleanup_complete, "terminal state cannot confirm failed cleanup");
  lifecycle.deny_cleanup = false;
  const auto retried = process.poll();
  expect(retried && retried->cleanup_complete && lifecycle.removed, "terminal cleanup can be retried after exit");

  FakeIo attach_failure; attach_failure.fail_file = "cgroup.procs";
  auto failed_limits = std::make_unique<mini_cloud::CgroupScope>("/delegated", "replica-gate", attach_failure);
  expect(failed_limits->configure(100, 32), "attachment failure scope configures");
  const auto marker = std::filesystem::temp_directory_path() / ("mini-cloud-gate-" + std::to_string(getpid()));
  mini_cloud::ProcessSupervisor gated("replica-gate");
  expect(!gated.launch(argv[1], {"touch-marker", marker.string()}, std::move(failed_limits)), "attachment failure rejects launch");
  expect(!std::filesystem::exists(marker) && attach_failure.removed && gated.pid() < 0, "failed attachment never executes workload and cleans up");
  mini_cloud::ProcessSupervisor missing("replica-missing");
  expect(!missing.launch("/does/not/exist", {}) && missing.pid() < 0 && !missing.error().empty(), "exec failure does not report RUNNING");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
