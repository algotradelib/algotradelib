#pragma once

/**
 * Declares that this translation unit has reached the transport layer.
 *
 * Read by `tests/test-strategy-isolation.cpp` to assert that the strategy surface does not. A
 * strategy must name no broker and perform no I/O, and that property degrades silently: one
 * convenience include in a strategy header would couple every strategy to a vendor, and nothing
 * would fail until someone built the same source against the other broker. Declaring the layer
 * here makes the assertion an architectural statement rather than a search for a third-party
 * macro that happens to come along for the ride.
 */
#define ATL_TRANSPORT_LAYER_PRESENT 1

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace algotradelib {

namespace json = boost::json;

namespace network {

/** CRTP storage and serialization for OAuth client credentials. */
template <typename BrokerType>
class ClientCredentials {
public:
  /** OAuth grant used by the supported broker machine clients. */
  static constexpr std::string_view grant_type = "client_credentials";

  /** Loads credentials using the broker-specific environment mapping. */
  void from_env() { static_cast<BrokerType*>(this)->from_env_impl(); }

  /** Loads credentials using the broker-specific JSON mapping. */
  void from_json(const std::string& filename) {
    static_cast<BrokerType*>(this)->from_json_impl(filename);
  }

  /** Returns the public application or client identifier. */
  const std::string& key() const noexcept { return key_; }

  /** Returns the application or client secret. */
  const std::string& secret() const noexcept { return secret_; }

  /** Serializes credentials as a JSON OAuth token request. */
  std::string to_jsonbody() const {
    json::object value;
    value["grant_type"] = grant_type;
    value[BrokerType::key_field()] = key_;
    value[BrokerType::secret_field()] = secret_;
    return json::serialize(value);
  }

  /** Serializes credentials as an application/x-www-form-urlencoded request. */
  std::string to_form_urlencoded_body() const {
    return "grant_type=client_credentials&client_id=" + form_encode(key_) +
      "&client_secret=" + form_encode(secret_);
  }

protected:
  /** The application key or client id. Never logged. */
  std::string key_;
  /** The application secret. Never logged. */
  std::string secret_;

private:
  /**
   * Percent-encodes one form value using the RFC 3986 unreserved character set.
   *
   * Encoding reserved characters such as `&`, `=`, `+`, and `%` prevents a
   * credential value from changing the surrounding form-field structure.
   */
  static std::string form_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
      const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
      if (unreserved) {
        result.push_back(static_cast<char>(ch));
      } else {
        result.push_back('%');
        result.push_back(hex[ch >> 4]);
        result.push_back(hex[ch & 0x0F]);
      }
    }
    return result;
  }
};

/** CRTP storage and parsing for an OAuth access-token response. */
template <typename BrokerType>
struct AuthBody {
  /** The bearer token itself. Never logged. */
  std::string access_token_;
  /** When it stops being usable, absolute rather than relative. */
  std::chrono::system_clock::time_point expires_at_{};

  /** Parses common token fields before invoking broker-specific parsing. */
  void from_json(const json::object& document) {
    const auto token = document.if_contains("access_token");
    if (!token || !token->is_string()) {
      throw std::runtime_error("token response has no access_token string");
    }
    access_token_ = token->as_string().c_str();

    std::int64_t expires_in = 86400;
    if (const auto value = document.if_contains("expires_in")) {
      if (value->is_int64()) {
        expires_in = value->as_int64();
      } else if (value->is_uint64()) {
        expires_in = static_cast<std::int64_t>(value->as_uint64());
      } else if (value->is_string()) {
        expires_in = std::stoll(std::string(value->as_string()));
      }
    }
    expires_at_ = std::chrono::system_clock::now() + std::chrono::seconds(expires_in);
    static_cast<BrokerType*>(this)->from_json_impl(document);
  }

  /** Returns whether the token remains valid beyond the requested margin. */
  bool valid(std::chrono::seconds margin = std::chrono::minutes(5)) const noexcept {
    return !access_token_.empty() && std::chrono::system_clock::now() + margin < expires_at_;
  }
};

} // namespace network
} // namespace algotradelib
