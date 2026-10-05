// Deliberately does not compile. `conformance-verbs-reject-a-token-copy` builds this file and passes
// only when the diagnostic is the deleted copy of a completion handler. The thunk below works for
// every copyable token -- a plain callback, `use_future` -- and fails for a move-only one, which a
// caller is entitled to pass; `ATL_ASSERT_BROKER` calls every verb with one so it is found here.
#include "mock_broker.hpp"

namespace probe {

/** An adapter whose quote thunk keeps a copy of the caller's token. */
struct CopiesItsToken : mock::MockClient {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::CopiesItsToken> : BrokerTraits<mock::MockClient> {
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    std::decay_t<Token> kept = token;
    return BrokerTraits<mock::MockClient>::quote(client, std::move(instrument), std::move(kept));
  }
};

} // namespace algotradelib

ATL_ASSERT_BROKER(probe::CopiesItsToken);
