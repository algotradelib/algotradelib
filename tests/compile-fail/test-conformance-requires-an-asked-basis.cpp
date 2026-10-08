// expect: no member named 'sellable_inquiry_basis' in [^\n]*InquiresWithoutBasis|'sellable_inquiry_basis' is not a member of [^\n]*InquiresWithoutBasis
// Deliberately does not compile. `conformance-requires-an-asked-basis` builds this file and passes only
// when the diagnostic names `sellable_inquiry_basis`: a capability set whose sellable figure comes from a
// separate inquiry is asked what that figure covers, and must answer.
#include <algotradelib/live>

#include "mock_broker.hpp"

namespace probe {

/** A capability set written by hand that makes the inquiry and states no basis. */
struct InquiresWithoutBasis {
  static constexpr bool data_only = false;
  static constexpr algotradelib::SellableSource sellable_source = algotradelib::SellableSource::INQUIRY;
};

/** The two names `SessionExecutor` reads from a session before anything else. */
struct Session {
  using Caps = InquiresWithoutBasis;
  using BrokerTag = mock::Broker;
};

} // namespace probe

static_assert(algotradelib::live::SessionExecutor<probe::Session>::sellable_inquiry_basis().is_enforceable());
