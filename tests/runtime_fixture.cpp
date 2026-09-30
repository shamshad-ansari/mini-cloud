#include <string_view>

#include <unistd.h>

int main(const int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "immediate-exit") return 0;
  if (argc == 2 && std::string_view(argv[1]) == "long-running") {
    while (true) pause();
  }
  return 2;
}
