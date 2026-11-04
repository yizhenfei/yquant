#include <cstdio>
#include <string_view>

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::puts("Usage: monod --help");
    std::puts("Production configuration loading is not implemented yet.");
    return 0;
  }
  if (argc != 1) {
    std::fputs("Unsupported argument. Use monod --help.\n", stderr);
    return 2;
  }
  std::fputs("Production configuration loading is not implemented yet.\n", stderr);
  return 1;
}
