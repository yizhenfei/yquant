#include "fake_md_api.h"
#include "simnow_market_demo.h"
#include "simnow_session.h"

namespace {
yquant::test::FakeMdApi api;
std::unique_ptr<yquant::market::SimNowQuoteSource> Connect(
    const yquant::market::SimNowMarketConfig& config) {
  api.automatic = config.front != "tcp://stalled:1";
  return yquant::market::internal::StartSession(config, [] { return &api; });
}
}  // namespace

int main(int argc, char* argv[]) {
  return yquant::verification::RunSimNowMarket(argc - 1, argv + 1, Connect);
}
