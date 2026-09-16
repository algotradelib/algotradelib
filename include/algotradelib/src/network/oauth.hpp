#pragma once

#include <string>
#include <string_view>
#include <boost/json/src.hpp>
#include <boost/json.hpp>

namespace algotradelib {

namespace json = boost::json;

namespace network {

template <typename BrokerType>
struct ClientCredentials {
  std::string key_;
  std::string secret_;
  static constexpr std::string_view grant_type_ = "client_credentials";

	/**
	 * @brief Read id and secret from the environment
	 * 
	 */
	void from_env() {
		static_cast<BrokerType*>(this)->from_env_impl();
  }

	/**
	 * @brief Read id and secret from json file 
	 * 
	 * @param filename json file
	 */
	void from_json(const std::string& filename) {
		static_cast<BrokerType*>(this)->from_json_impl();
	}

	/**
	 * @brief Make a jsonbody string
	 * 
	 * @return const std::string 
	 */
	const std::string to_jsonbody() {
		// Check overhead when converting to std::string
		return R"({"grant_type":")"
			+ std::string(grant_type_)
			+ R"(",")"
			+ std::string(BrokerType::key_field())
			+ R"(":")"
			+ key_
			+ R"(",")"
			+ std::string(BrokerType::secret_field())
			+ R"(":")"
			+ secret_
			+ R"("})";
		// std::string body;
    // body.reserve(64 + key_.size() + secret_.size());
    // body.append(R"({"grant_type":")");
    // body.append(grant_type_);
    // body.append(R"("," )"); // remove the space if exact JSON formatting matters
    // body.append(BrokerType::key_field());
    // body.append(R"(":")");
    // body.append(key_);
    // body.append(R"("," )");
    // body.append(BrokerType::secret_field());
    // body.append(R"(":")");
    // body.append(secret_);
    // body.append(R"("})");
    // return body;
	}
};

template <typename BrokerType>
struct AuthBody {
	std::string access_token_;
	int64_t expires_in_;
	static constexpr std::string_view grant_type_ = "Bearer";

	void from_json(const json::object& doc) {
		if (!doc.contains("access_token")) {
			throw std::runtime_error("Token authentication failed");
		}
		access_token_ = doc.at("access_token").as_string().c_str();
		expires_in_ = doc.contains("expires_in")
      ? doc.at("expires_in").as_int64()
      : 86400; // 1min (different value in each broker?)
		static_cast<BrokerType*>(this)->from_json_impl(doc);
	}

	// bool valid() const noexcept {
	// 	return static_cast<BrokerType*>(this)->valid_impl();
	// }
};

} // namespace network
} // namespace algotradelib