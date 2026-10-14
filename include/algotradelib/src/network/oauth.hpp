#pragma once

/**
 * @brief Defined as 1 once a transport (network) header is included.
 *
 * The library's tests use it to check that the strategy headers include no transport.
 */
#define ATL_TRANSPORT_LAYER_PRESENT 1

#include <boost/json.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace algotradelib {

namespace json = boost::json;

namespace network {

/**
 * @brief OAuth client credentials (an app key and secret), the base of each broker's credentials
 *   class.
 *
 * The broker class derives from it (CRTP) and provides `from_env_impl()`, `from_json_impl(filename)`,
 * and the static `key_field()` and `secret_field()` naming the token request's JSON fields.
 *
 * @tparam BrokerType The derived broker credentials class.
 */
template <typename BrokerType>
class ClientCredentials {
public:
  /** @brief The OAuth grant type sent in a token request. */
  static constexpr std::string_view grant_type = "client_credentials";

  /**
   * @brief Loads the credentials from the broker's environment variables.
   * @throws std::runtime_error (from the broker's implementation) if a variable is missing or
   *   invalid (such as a demo setting that is neither `0` nor `1`).
   */
  void from_env() { static_cast<BrokerType*>(this)->from_env_impl(); }

  /**
   * @brief Loads the credentials from a JSON file, in the broker's format.
   * @param filename The file.
   * @throws std::runtime_error (from the broker's implementation) if the file cannot be read or is
   *   not valid JSON, or a field is missing, of the wrong type or invalid.
   */
  void from_json(const std::string& filename) {
    static_cast<BrokerType*>(this)->from_json_impl(filename);
  }

  /**
   * @brief Returns the app key (client id).
   * @return The key.
   */
  const std::string& key() const noexcept { return key_; }

  /**
   * @brief Returns the app secret.
   * @return The secret; do not log it.
   */
  const std::string& secret() const noexcept { return secret_; }

  /**
   * @brief Builds a JSON token-request body (grant type, key and secret under the broker's field
   *   names).
   * @return The body.
   */
  std::string to_jsonbody() const {
    json::object value;
    value["grant_type"] = grant_type;
    value[BrokerType::key_field()] = key_;
    value[BrokerType::secret_field()] = secret_;
    return json::serialize(value);
  }

  /**
   * @brief Builds a form-encoded token-request body (`grant_type`, `client_id`, `client_secret`).
   * @return The body, values percent-encoded.
   */
  std::string to_form_urlencoded_body() const {
    return "grant_type=client_credentials&client_id=" + form_encode(key_) +
      "&client_secret=" + form_encode(secret_);
  }

protected:
  /** @brief The app key (client id). */
  std::string key_;
  /** @brief The app secret. */
  std::string secret_;

private:
  // Percent-encodes one form value using the RFC 3986 unreserved character set.
  //
  // Encoding reserved characters such as `&`, `=`, `+`, and `%` prevents a
  // credential value from changing the surrounding form-field structure.
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

/**
 * @brief An OAuth access token and its expiry, the base of each broker's token-response class.
 *
 * The broker class derives from it (CRTP) and provides the static `token_field()` naming the token's
 * JSON field, and `from_json_impl(document)` to read anything else (such as an absolute expiry).
 *
 * @tparam BrokerType The derived broker token class.
 */
template <typename BrokerType>
struct AuthBody {
  /** @brief The bearer token; do not log it. */
  std::string access_token_;
  /** @brief When the token expires. */
  std::chrono::system_clock::time_point expires_at_{};

  /**
   * @brief Reads a token response.
   *
   * Reads the token, a relative `expires_in` (a whole number, or a string of one) if present, then calls the
   * broker's `from_json_impl`, which may set an absolute expiry.
   *
   * @param document The response.
   * @throws std::runtime_error if the token field is missing or not a string, or the response states
   *   no expiry (no lifetime is assumed); or whatever the broker's `from_json_impl` throws.
   */
  void from_json(const json::object& document) {
    const std::string_view field = BrokerType::token_field();
    const auto token = document.if_contains(field);
    if (!token || !token->is_string()) {
      throw std::runtime_error("token response has no " + std::string(field) + " string");
    }
    access_token_ = token->as_string().c_str();

    expires_at_ = std::chrono::system_clock::time_point{};
    if (const auto value = document.if_contains("expires_in")) {
      std::optional<std::int64_t> expires_in;
      if (value->is_int64()) {
        expires_in = value->as_int64();
      } else if (value->is_uint64()) {
        expires_in = static_cast<std::int64_t>(value->as_uint64());
      } else if (value->is_string()) {
        expires_in = parse_seconds(value->as_string());
      }
      if (expires_in) {
        expires_at_ = std::chrono::system_clock::now() + std::chrono::seconds(*expires_in);
      }
    }
    static_cast<BrokerType*>(this)->from_json_impl(document);
    if (expires_at_ == std::chrono::system_clock::time_point{}) {
      throw std::runtime_error(
        "token response states no expiry, neither expires_in nor a broker-specific absolute one"
      );
    }
  }

  /**
   * @brief Tests whether the token is set and stays valid for at least a margin.
   * @param margin How long it must still be valid.
   * @return `true` if the token is nonempty and expires later than now plus `margin`.
   */
  bool valid(std::chrono::seconds margin = std::chrono::minutes(5)) const noexcept {
    return !access_token_.empty() && std::chrono::system_clock::now() + margin < expires_at_;
  }

private:
  // Reads a whole number of seconds, or nothing when the text is not one. A malformed `expires_in`
  // leaves `expires_at_` unset, so it reaches the no-expiry refusal rather than throwing an
  // exception that names neither the broker nor the field.
  static std::optional<std::int64_t> parse_seconds(std::string_view text) {
    std::int64_t seconds{0};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
      return std::nullopt;
    }
    return seconds;
  }
};

} // namespace network
} // namespace algotradelib
