#include "simnow_contract_query.h"

#include <cmath>
#include <limits>

#include "simnow_fields.h"

namespace yquant::trading::internal {
namespace {

bool ValidRate(double value) {
  return value >= 0 && value < std::numeric_limits<double>::max();
}

}  // namespace

SimNowContractQuery::SimNowContractQuery(std::string_view symbol) {
  terms.symbol = symbol;
  terms.buy.hedge = terms.sell.hedge = HedgeFlag::kSpeculation;
}

const char* SimNowContractQuery::field_name() const {
  switch (field) {
    case TermsField::kInstrument: return "contract_multiplier";
    case TermsField::kProduct: return "contract_currency";
    case TermsField::kMargin: return "account_margin";
    case TermsField::kExchangeMargin: return "exchange_margin";
    case TermsField::kFees: return "commission";
  }
  return "contract_terms";
}

int SimNowContractQuery::Send(CThostFtdcTraderApi& api,
                              const std::string& broker,
                              const std::string& user, int id) const {
  switch (field) {
    case TermsField::kInstrument: {
      CThostFtdcQryInstrumentField r{};
      Copy(r.InstrumentID, terms.symbol);
      return api.ReqQryInstrument(&r, id);
    }
    case TermsField::kProduct: {
      CThostFtdcQryProductField r{};
      Copy(r.ProductID, product_);
      Copy(r.ExchangeID, exchange_);
      return api.ReqQryProduct(&r, id);
    }
    case TermsField::kMargin: {
      CThostFtdcQryInstrumentMarginRateField r{};
      Copy(r.InstrumentID, terms.symbol);
      Copy(r.ExchangeID, exchange_);
      Copy(r.BrokerID, broker);
      Copy(r.InvestorID, user);
      r.HedgeFlag = THOST_FTDC_HF_Speculation;
      return api.ReqQryInstrumentMarginRate(&r, id);
    }
    case TermsField::kExchangeMargin: {
      CThostFtdcQryExchangeMarginRateField r{};
      Copy(r.InstrumentID, terms.symbol);
      Copy(r.ExchangeID, exchange_);
      Copy(r.BrokerID, broker);
      r.HedgeFlag = THOST_FTDC_HF_Speculation;
      return api.ReqQryExchangeMarginRate(&r, id);
    }
    case TermsField::kFees: {
      CThostFtdcQryInstrumentCommissionRateField r{};
      Copy(r.InstrumentID, terms.symbol);
      Copy(r.ExchangeID, exchange_);
      Copy(r.BrokerID, broker);
      Copy(r.InvestorID, user);
      return api.ReqQryInstrumentCommissionRate(&r, id);
    }
  }
  return -1;
}

bool SimNowContractQuery::Receive(const TermsRow* row, bool last) {
  if (row) {
    if (received_ || !std::visit([this](const auto& r) { return Apply(r); },
                                 *row)) return false;
    received_ = true;
  }
  if (!last) return row != nullptr;
  if (!received_) return false;
  received_ = false;
  switch (field) {
    case TermsField::kInstrument: field = TermsField::kProduct; break;
    case TermsField::kProduct: field = TermsField::kMargin; break;
    case TermsField::kMargin:
      field = relative_ ? TermsField::kExchangeMargin : TermsField::kFees;
      break;
    case TermsField::kExchangeMargin: field = TermsField::kFees; break;
    case TermsField::kFees: complete = true; break;
  }
  return true;
}

bool SimNowContractQuery::Apply(const CThostFtdcInstrumentField& r) {
  if (Field(r.InstrumentID) != terms.symbol || r.VolumeMultiple <= 0 ||
      r.ProductClass != THOST_FTDC_PC_Futures) return false;
  product_ = Field(r.ProductID);
  exchange_ = Field(r.ExchangeID);
  terms.buy.contract_multiplier = terms.sell.contract_multiplier = r.VolumeMultiple;
  return !product_.empty() && product_.size() < sizeof(r.ProductID) &&
         !exchange_.empty() && exchange_.size() < sizeof(r.ExchangeID);
}

bool SimNowContractQuery::Apply(const CThostFtdcProductField& r) {
  model::Currency currency(Field(r.TradeCurrencyID));
  if (Field(r.ProductID) != product_ || Field(r.ExchangeID) != exchange_ ||
      !currency.valid()) return false;
  terms.buy.currency = terms.sell.currency = currency;
  return true;
}

bool SimNowContractQuery::MatchesRate(std::string_view instrument,
                                      std::string_view exchange) const {
  return (instrument == terms.symbol || instrument == product_) &&
         (exchange.empty() || exchange == exchange_);
}

bool SimNowContractQuery::Apply(const CThostFtdcInstrumentMarginRateField& r) {
  const auto valid = [&](double rate) {
    return r.IsRelative ? ValidRate(std::abs(rate)) : ValidRate(rate);
  };
  if (!MatchesRate(Field(r.InstrumentID), Field(r.ExchangeID)) ||
      r.HedgeFlag != THOST_FTDC_HF_Speculation ||
      (r.IsRelative != 0 && r.IsRelative != 1) ||
      !valid(r.LongMarginRatioByMoney) || !valid(r.ShortMarginRatioByMoney) ||
      !valid(r.LongMarginRatioByVolume) || !valid(r.ShortMarginRatioByVolume)) {
    return false;
  }
  relative_ = r.IsRelative;
  terms.buy.margin_rate = r.LongMarginRatioByMoney;
  terms.sell.margin_rate = r.ShortMarginRatioByMoney;
  terms.buy.margin_per_quantity = r.LongMarginRatioByVolume;
  terms.sell.margin_per_quantity = r.ShortMarginRatioByVolume;
  return true;
}

bool SimNowContractQuery::Apply(const CThostFtdcExchangeMarginRateField& r) {
  if (!MatchesRate(Field(r.InstrumentID), Field(r.ExchangeID)) ||
      r.HedgeFlag != THOST_FTDC_HF_Speculation) return false;
  auto add = [](double& base, double rate) {
    if (!ValidRate(rate) || !ValidRate(base + rate)) return false;
    base += rate;
    return true;
  };
  return add(*terms.buy.margin_rate, r.LongMarginRatioByMoney) &&
         add(*terms.sell.margin_rate, r.ShortMarginRatioByMoney) &&
         add(terms.buy.margin_per_quantity, r.LongMarginRatioByVolume) &&
         add(terms.sell.margin_per_quantity, r.ShortMarginRatioByVolume);
}

bool SimNowContractQuery::Apply(const CThostFtdcInstrumentCommissionRateField& r) {
  if (!MatchesRate(Field(r.InstrumentID), Field(r.ExchangeID)) ||
      (r.BizType && r.BizType != THOST_FTDC_BZTP_Future)) return false;
  const FeeRate open{r.OpenRatioByVolume, r.OpenRatioByMoney};
  const FeeRate close{r.CloseRatioByVolume, r.CloseRatioByMoney};
  const FeeRate today{r.CloseTodayRatioByVolume, r.CloseTodayRatioByMoney};
  for (const auto fee : {open, close, today}) {
    if (!ValidRate(fee.per_notional) || !ValidRate(fee.per_quantity)) return false;
  }
  terms.buy.fees = {open, close, today, close};
  terms.sell.fees = terms.buy.fees;
  return true;
}

}  // namespace yquant::trading::internal
