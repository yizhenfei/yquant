#ifndef YQUANT_SRC_TRADING_SIMNOW_CONTRACT_QUERY_H_
#define YQUANT_SRC_TRADING_SIMNOW_CONTRACT_QUERY_H_

#include <ThostFtdcTraderApi.h>

#include <string>
#include <string_view>
#include <variant>

#include "yquant/trading/simnow_startup_snapshot.h"

namespace yquant::trading::internal {

enum class TermsField { kInstrument, kProduct, kMargin, kExchangeMargin, kFees };

using TermsRow = std::variant<CThostFtdcInstrumentField, CThostFtdcProductField,
    CThostFtdcInstrumentMarginRateField, CThostFtdcExchangeMarginRateField,
    CThostFtdcInstrumentCommissionRateField>;

// Assembles one contract's directional terms on the business thread. Send one
// request at a time and pass its matching rows, including the final marker,
// to Receive. Publish terms only after complete becomes true.
class SimNowContractQuery {
 public:
  explicit SimNowContractQuery(std::string_view symbol);
  int Send(CThostFtdcTraderApi& api, const std::string& broker,
           const std::string& user, int id) const;
  // Returns false for missing, ambiguous or invalid data. field continues
  // to identify the failed request; a null row is valid as a final marker.
  bool Receive(const TermsRow* row, bool last);
  const char* field_name() const;

  TermsField field = TermsField::kInstrument;
  bool complete = false;
  SimNowContractTerms terms;

 private:
  bool Apply(const CThostFtdcInstrumentField& row);
  bool Apply(const CThostFtdcProductField& row);
  bool Apply(const CThostFtdcInstrumentMarginRateField& row);
  bool Apply(const CThostFtdcExchangeMarginRateField& row);
  bool Apply(const CThostFtdcInstrumentCommissionRateField& row);
  bool MatchesRate(std::string_view instrument, std::string_view exchange) const;

  std::string exchange_, product_;
  bool received_ = false, relative_ = false;
};

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_CONTRACT_QUERY_H_
