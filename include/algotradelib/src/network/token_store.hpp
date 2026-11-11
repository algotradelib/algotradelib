#pragma once

#include "../core/types.hpp"

#include <openssl/evp.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace algotradelib {
namespace network {

/** @brief An access token as a `TokenStore` keeps it. */
struct StoredToken {
  /** @brief The bearer token; do not log it. */
  std::string access_token_;
  /** @brief When it expires, as the client that obtained it computed it. */
  std::chrono::system_clock::time_point expires_at_{};
};

/**
 * @brief Where blocking broker clients on one key share its access token, across threads and processes.
 *
 * Give one to a blocking client (`KisBlockingClient`, `KiwoomBlockingClient`, `TossBlockingClient`).
 * Before a call, a client whose token is missing or within five minutes of its expiry takes the lock of its
 * identity, adopts the stored token if it is valid, and otherwise requests one and publishes it before
 * sending anything else. When the broker refuses the token a call carried, the client discards it from the
 * store, if the store still holds that token.
 *
 * Each identity (a broker configuration's `token_identity()`) has its own lock. Call `load`, `publish`,
 * `discard` and `remove` while holding the object `lock` returned for that identity. Each function returns
 * filesystem and parse failures, rather than throwing them, as an `AUTHENTICATION` error with
 * `reached_broker_` false, broker code `token_store` and a fixed message (an allocation can still throw). An
 * implementation must allow calls from several threads at once.
 */
class TokenStore {
public:
  /** @brief Held while one caller works on one identity; destroying it releases the lock. */
  class Lock {
  public:
    virtual ~Lock() = default;
  };

  virtual ~TokenStore() = default;

  /**
   * @brief Takes the lock of one identity, waiting a bounded time for another holder.
   * @param identity The identity.
   * @return The held lock, or the store's error.
   */
  virtual Result<std::unique_ptr<Lock>> lock(const std::string& identity) = 0;

  /**
   * @brief Reads the token stored for an identity.
   * @param identity The identity.
   * @return The token, nothing if none is stored, or the store's error.
   */
  virtual Result<std::optional<StoredToken>> load(const std::string& identity) = 0;

  /**
   * @brief Stores a token for an identity, unless the stored one expires later.
   * @param identity The identity.
   * @param token The token.
   * @return The store's error, or nothing.
   */
  virtual std::optional<BrokerError> publish(const std::string& identity, const StoredToken& token) = 0;

  /**
   * @brief Removes an identity's token if it is the given one.
   * @param identity The identity.
   * @param access_token The token the broker refused.
   * @return The store's error, or nothing.
   */
  virtual std::optional<BrokerError> discard(const std::string& identity, const std::string& access_token) = 0;

  /**
   * @brief Removes an identity's token, whatever it is.
   * @param identity The identity.
   * @return The store's error, or nothing.
   */
  virtual std::optional<BrokerError> remove(const std::string& identity) = 0;
};

/**
 * @brief Returns a store's error: `AUTHENTICATION`, not sent to the broker, code `token_store`.
 * @param reason Fixed text; never a token, a file's contents or another error's message.
 * @return The error.
 */
inline BrokerError token_store_error(std::string_view reason) {
  BrokerError error;
  error.category_ = ErrorCategory::AUTHENTICATION;
  error.reached_broker_ = false;
  error.broker_code_ = "token_store";
  error.message_ = std::string(reason);
  return error;
}

/**
 * @brief Returns the error of a call whose token stopped being usable while it ran on a client with a
 *   store: `AUTHENTICATION`, not sent to the broker, code `token_unusable`. The next call prepares a token.
 * @return The error.
 */
inline BrokerError token_unusable_error() {
  BrokerError error;
  error.category_ = ErrorCategory::AUTHENTICATION;
  error.reached_broker_ = false;
  error.broker_code_ = "token_unusable";
  error.message_ = "the access token stopped being usable during the call; nothing was sent with a new one";
  return error;
}

/**
 * @brief Names what a token may not outlive: the broker, the environment and the app key.
 * @param broker The broker, such as `kis`.
 * @param environment The environment, such as `demo` or `real`.
 * @param key The app key; only its SHA-256 digest appears.
 * @return `<broker>-<environment>-<64 hexadecimal digits>`, or empty if the digest fails (no store accepts it).
 */
inline std::string token_identity(std::string_view broker, std::string_view environment, std::string_view key) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (EVP_Digest(key.data(), key.size(), digest, &length, EVP_sha256(), nullptr) != 1) {
    return {};
  }
  static constexpr char hex[] = "0123456789abcdef";
  std::string identity = std::string(broker) + '-' + std::string(environment) + '-';
  for (unsigned int index = 0; index < length; ++index) {
    identity.push_back(hex[digest[index] >> 4]);
    identity.push_back(hex[digest[index] & 0x0F]);
  }
  return identity;
}

namespace token_store_detail {

/**
 * @brief The step a blocking client with a store makes before each call.
 *
 * Nothing when the client's token is valid. Otherwise, under the identity's lock: adopts the stored token if
 * it is valid, else runs `request` and publishes what it obtained. When publishing fails the client forgets
 * the token, so the next call starts again from the lock.
 *
 * @tparam Client The asynchronous client (`token()`, `adopt_token()`, `forget_token()`).
 * @tparam Request A callable returning `Result<Token>` from the client's own token request.
 * @param store The store.
 * @param identity The identity.
 * @param client The client.
 * @param request The token request.
 * @return The error to return instead of sending, or nothing.
 */
template <typename Client, typename Request>
std::optional<BrokerError> prepare(TokenStore& store, const std::string& identity, Client& client, Request request) {
  using Token = decltype(client.token());
  if (client.token().valid()) {
    return std::nullopt;
  }
  auto held = store.lock(identity);
  if (!held) {
    return std::move(held.error());
  }
  auto loaded = store.load(identity);
  if (!loaded) {
    return std::move(loaded.error());
  }
  if (loaded.value()) {
    Token token;
    token.access_token_ = std::move(loaded.value()->access_token_);
    token.expires_at_ = loaded.value()->expires_at_;
    if (client.adopt_token(std::move(token))) {
      return std::nullopt;
    }
  }
  auto issued = request();
  if (!issued) {
    // A token response read only in part can leave a token that looks valid; it was never published.
    client.forget_token();
    return std::move(issued.error());
  }
  if (auto error = store.publish(identity, StoredToken{issued.value().access_token_, issued.value().expires_at_})) {
    client.forget_token();
    return error;
  }
  return std::nullopt;
}

/**
 * @brief Discards from the store a token the client forgot during a call (its broker refused it).
 *
 * A failure is ignored: the refused call's own result is what the caller sees.
 *
 * @tparam Token The client's token type.
 * @param store The store.
 * @param identity The identity.
 * @param carried The token the client held when the call started.
 * @param held The token it holds now.
 */
template <typename Token>
void discard_refused(TokenStore& store, const std::string& identity, const Token& carried, const Token& held) {
  if (carried.access_token_.empty() || !held.access_token_.empty()) {
    return;
  }
  if (const auto lock = store.lock(identity)) {
    store.discard(identity, carried.access_token_);
  }
}

} // namespace token_store_detail
} // namespace network
} // namespace algotradelib
