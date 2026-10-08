// expect: no matching[a-z ]*function for call to[^\n]*instrument
// Deliberately does not compile. `conformance-verbs-reject-a-wrong-signature` builds this file and
// passes only when the diagnostic is the missing overload of `instrument`: the thunk exists under the
// right name with the wrong signature, which only a check that calls it can reject.
#include "mock_broker.hpp"

namespace probe {

/** An adapter whose claimed reference-data thunk forgets the instrument it is asked about. */
struct WrongArityReferenceData : mock::MockClient {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::WrongArityReferenceData> : BrokerTraits<mock::MockClient> {
  static constexpr bool has_reference_data = true;
  template <typename Token>
  static void instrument(Client&, Token&&) {}
};

} // namespace algotradelib

ATL_ASSERT_BROKER(probe::WrongArityReferenceData);
