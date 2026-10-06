#include "mini_cloud/cgroup.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

namespace mini_cloud {
namespace {
bool fail(std::string& error, const std::filesystem::path& path, std::string_view action) {
  error = std::string(action) + " " + path.string() + ": " + std::strerror(errno) +
          ". Provide a writable cgroup v2 parent with cpu and memory delegated and enabled in cgroup.subtree_control; use --cgroup-root <parent>. Run the worker within the same delegation (for example, a delegated systemd service), or with the required root permissions.";
  return false;
}
bool contains(const std::filesystem::path& path, std::string_view word) {
  std::ifstream input(path);
  for (std::string token; input >> token;) if (token == word) return true;
  return false;
}
class SystemCgroupIo final : public CgroupIo {
 public:
  bool validate_root(const std::filesystem::path& root, std::string& error) override {
#ifdef __linux__
    struct statfs info{};
    if (statfs(root.c_str(), &info) < 0) return fail(error, root, "Cannot inspect");
    if (info.f_type != CGROUP2_SUPER_MAGIC) {
      error = root.string() + " is not a cgroup v2 filesystem. Mount cgroup v2 and provide --cgroup-root <delegated-parent>.";
      return false;
    }
    if (!contains(root / "cgroup.subtree_control", "cpu") || !contains(root / "cgroup.subtree_control", "memory")) {
      error = "Enable cpu and memory in " + (root / "cgroup.subtree_control").string() +
              " and delegate this empty parent; run the worker outside the parent or in a sibling leaf.";
      return false;
    }
    return true;
#else
    error = "cgroup v2 enforcement requires Linux and a delegated --cgroup-root <parent>.";
    static_cast<void>(root);
    return false;
#endif
  }
  bool create(const std::filesystem::path& path, std::string& error) override {
    std::error_code code;
    if (!std::filesystem::create_directory(path, code)) {
      errno = code ? code.value() : EEXIST;
      return fail(error, path, "Cannot create dedicated cgroup");
    }
    return true;
  }
  bool write(const std::filesystem::path& path, std::string_view value, std::string& error) override {
    // Never create files: missing controller interfaces must fail, not be simulated.
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return fail(error, path, "Cannot open cgroup control");
    ssize_t written;
    do { written = ::write(fd, value.data(), value.size()); } while (written < 0 && errno == EINTR);
    const int saved_errno = written < 0 ? errno : EIO;
    close(fd);
    if (written != static_cast<ssize_t>(value.size())) { errno = saved_errno; return fail(error, path, "Cannot write cgroup control"); }
    return true;
  }
  bool remove(const std::filesystem::path& path, std::string& error) override {
    if (rmdir(path.c_str()) == 0 || errno == ENOENT) return true;
    return fail(error, path, "Cannot remove cgroup");
  }
};
}

std::optional<CgroupLimits> CgroupLimits::from_request(std::uint64_t cpu, std::uint64_t memory) {
  const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (cpu == 0 || memory == 0 || cpu > maximum / 1000 || memory > maximum / (1024 * 1024)) return std::nullopt;
  return CgroupLimits{cpu * 1000, memory * 1024 * 1024};
}
CgroupIo& system_cgroup_io() { static SystemCgroupIo io; return io; }
CgroupScope::CgroupScope(std::filesystem::path root, std::string name, CgroupIo& io)
    : root_(std::move(root)), path_(root_ / name), io_(io) {
  if (name.empty() || name == "." || name == ".." || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos)
    error_ = "Invalid cgroup name: use a single safe path component.";
}
CgroupScope::~CgroupScope() { if (created_) static_cast<void>(cleanup()); }
bool CgroupScope::configure(std::uint64_t cpu, std::uint64_t memory) {
  if (!error_.empty() || created_) return false;
  const auto limits = CgroupLimits::from_request(cpu, memory);
  if (!limits) { error_ = "CPU/memory limits must be positive and fit signed 64-bit cgroup units."; return false; }
  if (!io_.validate_root(root_, error_) || !io_.create(path_, error_)) return false;
  created_ = true;
  if (!io_.write(path_ / "cpu.max", std::to_string(limits->quota_microseconds) + " " + std::to_string(CgroupLimits::period_microseconds), error_) ||
      !io_.write(path_ / "memory.max", std::to_string(limits->memory_bytes), error_) ||
      !io_.write(path_ / "memory.swap.max", "0", error_) ||
      !io_.write(path_ / "memory.oom.group", "1", error_) ||
      !io_.write(path_ / "cgroup.kill", "1", error_)) {
    const auto setup_error = error_;
    static_cast<void>(cleanup());
    error_ = setup_error;
    return false;
  }
  return true;
}
bool CgroupScope::attach(pid_t pid) {
  if (!created_ || pid <= 0) { error_ = "Cannot attach a replica before cgroup setup or with an invalid PID."; return false; }
  return io_.write(path_ / "cgroup.procs", std::to_string(pid), error_);
}
bool CgroupScope::kill_all() { return !created_ || io_.write(path_ / "cgroup.kill", "1", error_); }
bool CgroupScope::cleanup() {
  if (!created_) return true;
  // Configuration rollback can remove an empty scope even when an interface is
  // unavailable; a populated scope must first terminate all its descendants.
  if (io_.remove(path_, error_)) { created_ = false; error_.clear(); return true; }
  if (!kill_all()) return false;
  // Exiting descendants can briefly keep the scope populated after cgroup.kill.
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (io_.remove(path_, error_)) { created_ = false; error_.clear(); return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}
const std::filesystem::path& CgroupScope::path() const noexcept { return path_; }
const std::string& CgroupScope::error() const noexcept { return error_; }
}  // namespace mini_cloud
