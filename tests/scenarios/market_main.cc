#include "simnow_market_demo.h"

int main(int argc, char* argv[]) {
  return yquant::verification::RunSimNowMarket(argc - 1, argv + 1);
}
