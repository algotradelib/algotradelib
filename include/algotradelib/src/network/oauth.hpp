#pragma once

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace algotradelib {

namespace json = boost::json;

namespace network {

template <typename BrokerType>
class ClientCredentials {
public:
  static constexpr std::string_view grant_type = "client_credentials";

  void from_env() { static_cast<BrokerType*>(this)->from_env_impl(); }

  void from_json(const std::string& filename) {
    static_cast<BrokerType*>(this)->from_json_impl(filename);
  }

  const std::string& key() const noexcept { return key_; }
  const std::string& secret() const noexcept { return secret_; }

  std::string to_jsonbody() const {
    json::object value;
    value["grant_type"] = grant_type;
    value[BrokerType::key_field()] = key_;
    value[BrokerType::secret_field()] = secret_;
    return json::serialize(value);
  }

  std::string to_form_urlencoded_body() const {
    return "grant_type=client_credentials&client_id=" + form_encode(key_) +
      "&client_secret=" + form_encode(secret_);
  }

protected:
  std::string key_;
  std::string secret_;

private:
  static std::string form_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
      const bool unreserved = (ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '.' || ch == '_' || ch == '~';
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

template <typename BrokerType>
struct AuthBody {
  std::string access_token_;
  std::chrono::system_clock::time_point expires_at_{};

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

  bool valid(std::chrono::seconds margin = std::chrono::minutes(5)) const noexcept {
    return !access_token_.empty() &&
      std::chrono::system_clock::now() + margin < expires_at_;
  }
};

} // namespace network
} // namespace algotradelib
