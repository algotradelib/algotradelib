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

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
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

/**
 * Reads a required demo/live switch from the environment variable `variable`.
 *
 * **Required, because the unsafe answer is the silent one.** An unset variable used to mean the live
 * endpoints, so a shell that forgot one `export` traded real money with nothing on screen to say so.
 * Now `1` selects the broker's 모의투자 (paper-trading) endpoints, `0` selects live, and anything else
 * -- unset, empty, `true`, `yes`, a stray space -- refuses to load. No trimming and no case folding:
 * a value that has to be interpreted is a value somebody could have meant the other way.
 *
 * @param variable The environment variable to read, such as `KIS_DEMO`.
 * @return `true` for the demo endpoints, `false` for live.
 * @throws std::runtime_error naming the variable and both accepted values.
 */
inline bool required_demo_flag(const char* variable) {
  const char* value = std::getenv(variable);
  if (value && std::string_view(value) == "1") {
    return true;
  }
  if (value && std::string_view(value) == "0") {
    return false;
  }
  throw std::runtime_error(
    std::string(variable) + (value ? " is \"" + std::string(value) + "\"" : std::string(" is not set")) +
    "; it is required. Set " + variable + "=1 for the paper-trading (demo) endpoints or " + variable +
    "=0 to trade live."
  );
}

/**
 * Reads the required `"demo"` key from a credential file's JSON object.
 *
 * The file form of `required_demo_flag(const char*)`: a missing key used to mean live. The key must be a
 * JSON boolean -- `true` for the demo endpoints, `false` for live. A string or a number is refused
 * rather than read, for the same reason the environment form refuses `yes`.
 *
 * @param doc The parsed credential file.
 * @param file The file's name, for the message.
 * @return `true` for the demo endpoints, `false` for live.
 * @throws std::runtime_error naming the key, the file and both accepted values.
 */
inline bool required_demo_flag(const json::object& doc, std::string_view file) {
  const json::value* demo = doc.if_contains("demo");
  if (demo && demo->is_bool()) {
    return demo->get_bool();
  }
  throw std::runtime_error(
    "Credential file " + std::string(file) +
    (demo ? " has a \"demo\" key that is not true or false" : std::string(" has no \"demo\" key")) +
    "; it is required. Write \"demo\": true for the paper-trading (demo) endpoints or \"demo\": false "
    "to trade live."
  );
}

/**
 * CRTP storage and parsing for an OAuth access-token response.
 *
 * **The base names no wire field of its own**, which is the whole reason it is a template. It used
 * to read the literal `"access_token"` and to fall back to a twenty-four hour lifetime, and both
 * were properties of the two brokers that existed rather than of OAuth: Kiwoom's `au10001` returns
 * the token under `token` and states its expiry as an absolute `expires_dt`, with no relative field
 * at all. A broker declares its own spelling through `token_field()`, exactly as it already declares
 * the request side through `ClientCredentials<BrokerType>::key_field()`.
 *
 * `token_field()` is required rather than defaulted on purpose. A default would let a new broker
 * inherit KIS's spelling in silence and discover it on the first live token.
 */
template <typename BrokerType>
struct AuthBody {
  /** The bearer token itself. Never logged. */
  std::string access_token_;
  /** When it stops being usable, absolute rather than relative. */
  std::chrono::system_clock::time_point expires_at_{};

  /**
   * Parses the token and any relative expiry, then hands over to broker-specific parsing.
   *
   * **A response that states no expiry is refused rather than given one.** The relative
   * `expires_in` is read when the broker sends it; a broker that states its expiry absolutely sets
   * `expires_at_` in its own `from_json_impl`, as `KisRest` does for `access_token_token_expired`
   * and `KiwoomRest` does for `expires_dt`. If neither happened, `expires_at_` is still unset and
   * this throws, because the alternative -- the invented day this code used to supply -- is a
   * guessed lifetime presented as a stated one, and every caller treats it as stated. Decision 6:
   * never silently downgrade.
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

  /** Returns whether the token remains valid beyond the requested margin. */
  bool valid(std::chrono::seconds margin = std::chrono::minutes(5)) const noexcept {
    return !access_token_.empty() && std::chrono::system_clock::now() + margin < expires_at_;
  }

private:
  /**
   * Reads a whole number of seconds, or nothing when the text is not one.
   *
   * A malformed `expires_in` leaves `expires_at_` unset, so it reaches the refusal above rather
   * than throwing an exception whose message names neither the broker nor the field.
   */
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
