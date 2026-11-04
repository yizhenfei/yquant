#include <string_view>

#include "trader_login.h"

int main(int argc, char* argv[]) {
  const bool snapshot = argc > 1 && std::string_view(argv[1]) == "--snapshot";
  const int skip = snapshot ? 2 : 1;
  return yquant::verification::RunTraderLogin(argc - skip, argv + skip, snapshot);
}
