// Deliberately does not compile. `conformance-names-an-unbacked-capability` builds this file and
// passes only when the diagnostic names the capability and the thunk, which is what proves
// `ATL_ASSERT_BROKER` checks a claimed capability at all -- the detector tests in
// test-capability-conformance.cpp would still pass with the assertion deleted.
#include "mock_broker.hpp"

namespace probe {

/** An adapter complete in every respect except the one verb its capabilities claim. */
struct ClaimsReferenceData : mock::MockClient {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::ClaimsReferenceData> : BrokerTraits<mock::MockClient> {
  static constexpr bool has_reference_data = true;
};

} // namespace algotradelib

ATL_ASSERT_BROKER(probe::ClaimsReferenceData);
