// expect: no member named 'instrument' in [^\n]*ClaimsReferenceData|'instrument' is not a member of [^\n]*ClaimsReferenceData
// Deliberately does not compile. `conformance-names-an-unbacked-capability` builds this file and passes
// only when the diagnostic names the missing thunk and this adapter: a capability claimed with no verb
// behind it is what `ATL_ASSERT_BROKER` exists to find before a caller does.
#include "mock_broker.hpp"

namespace probe {

/** An adapter complete in every respect except the one verb its capabilities claim. */
struct ClaimsReferenceData : vendor::MockClient {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::ClaimsReferenceData> : BrokerTraits<vendor::MockClient> {
  static constexpr bool has_reference_data = true;
};

} // namespace algotradelib

ATL_ASSERT_BROKER(probe::ClaimsReferenceData);
