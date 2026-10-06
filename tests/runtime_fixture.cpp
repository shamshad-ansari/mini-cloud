#include <string_view>
#include <fstream>
#include <sys/mman.h>
#include <chrono>

#include <unistd.h>

int main(const int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "immediate-exit") return 0;
  if (argc == 2 && std::string_view(argv[1]) == "long-running") {
    while (true) pause();
  }
  if (argc == 3 && std::string_view(argv[1]) == "touch-marker") {
    std::ofstream marker(argv[2]); marker << "executed"; return marker ? 0 : 3;
  }
  if (argc == 2 && std::string_view(argv[1]) == "memory-abuse") {
    // Bounded to 128 MiB even if the limit is broken; the privileged test sets 32 MiB.
    constexpr std::size_t bytes = 128 * 1024 * 1024;
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 3;
    auto* pages = static_cast<volatile unsigned char*>(memory);
    for (std::size_t offset = 0; offset < bytes; offset += 4096) pages[offset] = 1;
    munmap(memory, bytes);
    return 4;  // Reaching this exit means the 32-MiB ceiling was not enforced.
  }
  if (argc == 2 && std::string_view(argv[1]) == "cpu-burn") {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {}
    return 0;
  }
  return 2;
}
