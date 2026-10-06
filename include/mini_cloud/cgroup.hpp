#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace mini_cloud {

struct CgroupLimits {
  // A one-second period represents even a 1-millicore request with the kernel's
  // minimum 1-millisecond quota, without rounding the admitted CPU upward.
  static constexpr std::uint64_t period_microseconds = 1000000;
  std::uint64_t quota_microseconds;
  std::uint64_t memory_bytes;
  [[nodiscard]] static std::optional<CgroupLimits> from_request(std::uint64_t millicores,
                                                               std::uint64_t memory_mib);
};

// The interface permits deterministic failure/cleanup tests without pretending
// that an ordinary filesystem can enforce cgroup limits.
class CgroupIo {
 public:
  virtual ~CgroupIo() = default;
  virtual bool validate_root(const std::filesystem::path& root, std::string& error) = 0;
  virtual bool create(const std::filesystem::path& path, std::string& error) = 0;
  virtual bool write(const std::filesystem::path& path, std::string_view value, std::string& error) = 0;
  virtual bool remove(const std::filesystem::path& path, std::string& error) = 0;
};

[[nodiscard]] CgroupIo& system_cgroup_io();

class CgroupScope {
 public:
  CgroupScope(std::filesystem::path root, std::string name, CgroupIo& io = system_cgroup_io());
  ~CgroupScope();
  CgroupScope(const CgroupScope&) = delete;
  CgroupScope& operator=(const CgroupScope&) = delete;
  [[nodiscard]] bool configure(std::uint64_t millicores, std::uint64_t memory_mib);
  [[nodiscard]] bool attach(pid_t pid);
  [[nodiscard]] bool kill_all();
  [[nodiscard]] bool cleanup();
  [[nodiscard]] const std::filesystem::path& path() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept;
 private:
  std::filesystem::path root_, path_;
  CgroupIo& io_;
  bool created_ = false;
  std::string error_;
};
}  // namespace mini_cloud
