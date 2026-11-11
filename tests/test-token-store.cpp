// Sharing a broker key's access token through a token store: the file store itself, and blocking clients
// on one store standing in for programs on one key, through each client's own request path against the
// loopback server.

#include <algotradelib/kis>
#include <algotradelib/kiwoom>
#include <algotradelib/network>
#include <algotradelib/tossinvest>

#include <catch2/catch_test_macros.hpp>

#include "local_https_server.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace algotradelib;

namespace algotradelib::kis {
struct KisClientTestAccess {
  /** Points a blocking client at a loopback server; every request after an idle moment takes a fresh connection. */
  static void connect(KisBlockingClient& client, ssl::context& context, const std::string& port) {
    network::HttpClientOptions options;
    options.unreplayed_idle_limit_ = std::chrono::milliseconds(0);
    client.client_->http_ =
      std::make_shared<network::AsyncHttpClient>(client.client_->strand_, context, "localhost", port, options);
  }
  /** Runs `ensure_token` on `client`, with or without a store managing its token; the outcome if it completed. */
  template <typename Client>
  static std::optional<std::optional<BrokerError>> ensure(Client& client, bool managed) {
    client.store_managed_ = managed;
    std::optional<std::optional<BrokerError>> outcome;
    client.ensure_token(typename Client::TokenHandler([&outcome](std::optional<BrokerError> error) {
      outcome.emplace(std::move(error));
    }));
    return outcome;
  }

  /** Whether `client` has started a token request. */
  template <typename Client>
  static bool requesting(const Client& client) {
    return client.refreshing_;
  }
};
} // namespace algotradelib::kis

namespace algotradelib::kiwoom {
struct KiwoomClientTestAccess {
  /** Points a blocking client at a loopback server; every request after an idle moment takes a fresh connection. */
  static void connect(KiwoomBlockingClient& client, ssl::context& context, const std::string& port) {
    network::HttpClientOptions options;
    options.unreplayed_idle_limit_ = std::chrono::milliseconds(0);
    client.client_->http_ =
      std::make_shared<network::AsyncHttpClient>(client.client_->strand_, context, "localhost", port, options);
  }
  /** Runs `ensure_token` on `client`, with or without a store managing its token; the outcome if it completed. */
  template <typename Client>
  static std::optional<std::optional<BrokerError>> ensure(Client& client, bool managed) {
    client.store_managed_ = managed;
    std::optional<std::optional<BrokerError>> outcome;
    client.ensure_token(typename Client::TokenHandler([&outcome](std::optional<BrokerError> error) {
      outcome.emplace(std::move(error));
    }));
    return outcome;
  }

  /** Whether `client` has started a token request. */
  template <typename Client>
  static bool requesting(const Client& client) {
    return client.refreshing_;
  }
};
} // namespace algotradelib::kiwoom

namespace algotradelib::tossinvest {
struct TossClientTestAccess {
  /** Points a blocking client at a loopback server; every request after an idle moment takes a fresh connection. */
  static void connect(TossBlockingClient& client, ssl::context& context, const std::string& port) {
    network::HttpClientOptions options;
    options.unreplayed_idle_limit_ = std::chrono::milliseconds(0);
    client.client_->http_ =
      std::make_shared<network::AsyncHttpClient>(client.client_->strand_, context, "localhost", port, options);
  }
  /** Runs `ensure_token` on `client`, with or without a store managing its token; the outcome if it completed. */
  template <typename Client>
  static std::optional<std::optional<BrokerError>> ensure(Client& client, bool managed) {
    client.store_managed_ = managed;
    std::optional<std::optional<BrokerError>> outcome;
    client.ensure_token(typename Client::TokenHandler([&outcome](std::optional<BrokerError> error) {
      outcome.emplace(std::move(error));
    }));
    return outcome;
  }

  /** Whether `client` has started a token request. */
  template <typename Client>
  static bool requesting(const Client& client) {
    return client.refreshing_;
  }
};
} // namespace algotradelib::tossinvest

TEST_CASE("a token identity names the broker and the environment and hides the key", "[network][token]") {
  kis::KisConfig real;
  real.from_json_text(R"({"app_key":"KEY-ONE","app_secret":"s","demo":false})", "test");
  kis::KisConfig demo;
  demo.from_json_text(R"({"app_key":"KEY-ONE","app_secret":"s","demo":true})", "test");
  kis::KisConfig other;
  other.from_json_text(R"({"app_key":"KEY-TWO","app_secret":"s","demo":false})", "test");
  tossinvest::TossConfig toss;
  toss.from_json_text(R"({"client_id":"KEY-ONE","client_secret":"s"})", "test");
  kiwoom::KiwoomConfig kiwoom;
  kiwoom.from_json_text(R"({"appkey":"KEY-ONE","secretkey":"s","demo":true})", "test");

  CHECK(real.token_identity().rfind("kis-real-", 0) == 0);
  CHECK(demo.token_identity().rfind("kis-demo-", 0) == 0);
  CHECK(toss.token_identity().rfind("toss-real-", 0) == 0);
  CHECK(kiwoom.token_identity().rfind("kiwoom-demo-", 0) == 0);
  CHECK(real.token_identity().size() == std::string("kis-real-").size() + 64);
  CHECK(real.token_identity() != other.token_identity());
  CHECK(real.token_identity() == real.token_identity());
  // SHA-256 of "KEY-ONE"; the key itself never appears.
  CHECK(real.token_identity() == network::token_identity("kis", "real", "KEY-ONE"));
  CHECK(real.token_identity().find("KEY-ONE") == std::string::npos);
}

namespace {

/** Runs the managed and unmanaged `ensure_token` of one asynchronous client with a valid and with no token. */
template <typename Access, typename Client, typename Config, typename Token>
void check_managed_ensure_token(const char* name) {
  CAPTURE(name);
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);

  // Managed, valid: completes at once with success and starts no token request.
  auto valid = std::make_shared<Client>(ioc.get_executor(), context, Config{});
  Token token;
  token.access_token_ = "held";
  token.expires_at_ = std::chrono::system_clock::now() + std::chrono::hours(20);
  REQUIRE(valid->adopt_token(token));
  const auto passed = Access::ensure(*valid, true);
  REQUIRE(passed);
  CHECK_FALSE(passed->has_value());
  CHECK_FALSE(Access::requesting(*valid));

  // Managed, no token: completes at once with `token_unusable` and starts no token request.
  auto empty = std::make_shared<Client>(ioc.get_executor(), context, Config{});
  const auto refused = Access::ensure(*empty, true);
  REQUIRE(refused);
  REQUIRE(refused->has_value());
  CHECK((*refused)->broker_code_ == "token_unusable");
  CHECK((*refused)->category_ == ErrorCategory::AUTHENTICATION);
  CHECK_FALSE((*refused)->reached_broker_);
  CHECK_FALSE(Access::requesting(*empty));

  // Unmanaged, no token: the same probe sees today's token request start (the loop is never run).
  auto unmanaged = std::make_shared<Client>(ioc.get_executor(), context, Config{});
  CHECK_FALSE(Access::ensure(*unmanaged, false));
  CHECK(Access::requesting(*unmanaged));
}

} // namespace

TEST_CASE("a managed client completes ensure_token at once and never requests a token", "[network][token]") {
  check_managed_ensure_token<kis::KisClientTestAccess, kis::KisClient, kis::KisConfig, kis::KisRest>("kis");
  check_managed_ensure_token<
    kiwoom::KiwoomClientTestAccess, kiwoom::KiwoomClient, kiwoom::KiwoomConfig, kiwoom::KiwoomRest
  >("kiwoom");
  check_managed_ensure_token<
    tossinvest::TossClientTestAccess, tossinvest::TossClient, tossinvest::TossConfig, tossinvest::TossRest
  >("toss");
}

#if !defined(_WIN32)

namespace {

using std::chrono::hours;
using std::chrono::milliseconds;
using std::chrono::seconds;
using Clock = std::chrono::system_clock;

/** A private folder under the system's temporary folder, removed with what it holds. */
class TempFolder {
public:
  TempFolder() {
    std::string pattern = (std::filesystem::temp_directory_path() / "atl-token-store-XXXXXX").string();
    if (::mkdtemp(pattern.data()) != nullptr) {
      path_ = pattern;
    }
  }

  ~TempFolder() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;

  /** The folder a store is given: created by the store, inside this one. */
  std::string tokens() const {
    return path_ + "/tokens";
  }

  /** A file in the store's folder. */
  std::string file(const std::string& name) const {
    return tokens() + "/" + name;
  }

  const std::string& path() const {
    return path_;
  }

private:
  std::string path_;
};

/** A token expiring `lifetime` from now. */
network::StoredToken stored(const std::string& token, Clock::duration lifetime) {
  return network::StoredToken{token, Clock::now() + lifetime};
}

/** A file's permission bits. */
mode_t mode_of(const std::string& path) {
  struct stat status{};
  ::lstat(path.c_str(), &status);
  return status.st_mode & 07777;
}

/** A file's contents. */
std::string contents_of(const std::string& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/** Writes a file, private to the user. */
void write_file(const std::string& path, const std::string& text) {
  {
    std::ofstream out(path);
    out << text;
  }
  ::chmod(path.c_str(), 0600);
}

/** Checks an error is the store's, with the given fixed message and nothing from a token. */
void check_store_error(const BrokerError& error, const std::string& message) {
  CHECK(error.category_ == ErrorCategory::AUTHENTICATION);
  CHECK_FALSE(error.reached_broker_);
  CHECK(error.broker_code_ == "token_store");
  CHECK(error.message_ == message);
  CHECK(error.message_.find("LEAK") == std::string::npos);
}

constexpr const char* ID = "kis-real-0123abcd";

} // namespace

TEST_CASE("a file store keeps one token per identity and a newer one wins", "[network][token]") {
  const TempFolder folder;
  network::FileTokenStore store(folder.tokens());
  {
    auto lock = store.lock(ID);
    REQUIRE(lock);
    auto empty = store.load(ID);
    REQUIRE(empty);
    CHECK_FALSE(empty.value());

    const auto later = stored("later", hours(2));
    CHECK_FALSE(store.publish(ID, later));
    CHECK_FALSE(store.publish(ID, stored("earlier", hours(1))));
    auto loaded = store.load(ID);
    REQUIRE(loaded);
    REQUIRE(loaded.value());
    CHECK(loaded.value()->access_token_ == "later");
    // The expiry comes back as it was stored, to the clock's resolution.
    CHECK(loaded.value()->expires_at_ == later.expires_at_);
    CHECK_FALSE(store.publish(ID, stored("latest", hours(3))));
    CHECK(store.load(ID).value()->access_token_ == "latest");
  }
  CHECK((mode_of(folder.tokens()) & 077) == 0);
  CHECK((mode_of(folder.file(std::string(ID) + ".token")) & 077) == 0);
  CHECK((mode_of(folder.file(std::string(ID) + ".lock")) & 077) == 0);

  // Another identity sees nothing of it.
  auto other_lock = store.lock("kis-demo-0123abcd");
  REQUIRE(other_lock);
  auto other = store.load("kis-demo-0123abcd");
  REQUIRE(other);
  CHECK_FALSE(other.value());

  // A discard removes only the token it names.
  CHECK_FALSE(store.publish("kis-demo-0123abcd", stored("demo", hours(1))));
  CHECK_FALSE(store.discard(ID, "later"));
  CHECK(store.load(ID).value()->access_token_ == "latest");
  CHECK_FALSE(store.discard(ID, "latest"));
  CHECK_FALSE(store.load(ID).value());

  // A removal takes one identity's token and leaves its lock file.
  CHECK_FALSE(store.publish(ID, stored("again", hours(1))));
  CHECK_FALSE(store.remove(ID));
  CHECK_FALSE(store.load(ID).value());
  CHECK(store.load("kis-demo-0123abcd").value()->access_token_ == "demo");
  CHECK(std::filesystem::exists(folder.file(std::string(ID) + ".lock")));
  CHECK_FALSE(store.remove(ID));

  // A name that could leave the folder is refused.
  for (
    const auto& identity :
    {std::string(), std::string("../escape"), std::string("a/b"), std::string(".hidden"), std::string(201, 'a')}
  ) {
    CAPTURE(identity);
    const auto refused = store.lock(identity);
    REQUIRE_FALSE(refused);
    check_store_error(refused.error(), "token store: the identity cannot name a file");
  }
}

TEST_CASE("a file store refuses a folder or file that is not private and leaves it as it was", "[network][token]") {
  for (const mode_t bit : {0040, 0020, 0010, 0004, 0002, 0001}) {
    CAPTURE(bit);
    const TempFolder folder;
    network::FileTokenStore store(folder.tokens(), milliseconds(100));
    REQUIRE(store.lock(ID));
    REQUIRE_FALSE(store.publish(ID, stored("LEAK-token", hours(1))));
    const auto token_file = folder.file(std::string(ID) + ".token");
    const auto lock_file = folder.file(std::string(ID) + ".lock");
    const auto written = contents_of(token_file);

    ::chmod(token_file.c_str(), 0600 | bit);
    REQUIRE(store.lock(ID));
    const auto loaded = store.load(ID);
    REQUIRE_FALSE(loaded);
    check_store_error(loaded.error(), "token store: a file is not private (owner, permissions or type)");
    const auto published = store.publish(ID, stored("newer", hours(2)));
    REQUIRE(published);
    check_store_error(*published, "token store: a file is not private (owner, permissions or type)");
    REQUIRE(store.discard(ID, "LEAK-token"));
    CHECK(mode_of(token_file) == (0600 | bit));
    CHECK(contents_of(token_file) == written);
    ::chmod(token_file.c_str(), 0600);

    ::chmod(lock_file.c_str(), 0600 | bit);
    const auto locked = store.lock(ID);
    REQUIRE_FALSE(locked);
    check_store_error(locked.error(), "token store: a file is not private (owner, permissions or type)");
    CHECK(mode_of(lock_file) == (0600 | bit));
    ::chmod(lock_file.c_str(), 0600);

    ::chmod(folder.tokens().c_str(), 0700 | bit);
    const auto refused = store.lock(ID);
    REQUIRE_FALSE(refused);
    check_store_error(refused.error(), "token store: the folder is not private (owner or permissions)");
    REQUIRE(store.load(ID).has_value() == false);
    CHECK(mode_of(folder.tokens()) == (0700 | bit));
    ::chmod(folder.tokens().c_str(), 0700);
    CHECK(contents_of(token_file) == written);
  }
}

TEST_CASE("a file store follows no link at the folder or a file", "[network][token]") {
  const TempFolder folder;
  // A private folder elsewhere, and a link to it where the store's folder would be.
  const auto elsewhere = folder.path() + "/elsewhere";
  REQUIRE(::mkdir(elsewhere.c_str(), 0700) == 0);
  REQUIRE(::symlink(elsewhere.c_str(), folder.tokens().c_str()) == 0);
  network::FileTokenStore linked(folder.tokens(), milliseconds(100));
  const auto refused = linked.lock(ID);
  REQUIRE_FALSE(refused);
  check_store_error(refused.error(), "token store: the folder is a symbolic link");
  CHECK(std::filesystem::is_empty(elsewhere));
  REQUIRE(::unlink(folder.tokens().c_str()) == 0);

  network::FileTokenStore store(folder.tokens(), milliseconds(100));
  REQUIRE(store.lock("kis-real-target"));
  REQUIRE_FALSE(store.publish("kis-real-target", stored("LEAK-target", hours(1))));
  const auto target = folder.file("kis-real-target.token");
  const auto written = contents_of(target);

  // A link where the token file would be: not read, not replaced.
  REQUIRE(::symlink(target.c_str(), folder.file(std::string(ID) + ".token").c_str()) == 0);
  REQUIRE(store.lock(ID));
  const auto loaded = store.load(ID);
  REQUIRE_FALSE(loaded);
  check_store_error(loaded.error(), "token store: a file is a symbolic link");
  const auto published = store.publish(ID, stored("newer", hours(2)));
  REQUIRE(published);
  check_store_error(*published, "token store: a file is a symbolic link");
  CHECK(contents_of(target) == written);

  // A link where the lock file would be.
  REQUIRE(::symlink(target.c_str(), folder.file("kis-real-other.lock").c_str()) == 0);
  const auto locked = store.lock("kis-real-other");
  REQUIRE_FALSE(locked);
  check_store_error(locked.error(), "token store: a file is a symbolic link");
  CHECK(contents_of(target) == written);
}

TEST_CASE(
  "a file store keeps the old token when a write fails and reads a malformed file as none", "[network][token]"
) {
  const TempFolder folder;
  network::FileTokenStore store(folder.tokens());
  REQUIRE(store.lock(ID));
  REQUIRE_FALSE(store.publish(ID, stored("old", hours(1))));

  // A directory where the temporary file goes: the write fails, the token file stays.
  REQUIRE(::mkdir(folder.file(std::string(ID) + ".tmp").c_str(), 0700) == 0);
  const auto failed = store.publish(ID, stored("LEAK-new", hours(2)));
  REQUIRE(failed);
  check_store_error(*failed, "token store: the token file could not be written");
  CHECK(store.load(ID).value()->access_token_ == "old");
  REQUIRE(::rmdir(folder.file(std::string(ID) + ".tmp").c_str()) == 0);

  // A temporary file a killed writer left is replaced.
  write_file(folder.file(std::string(ID) + ".tmp"), "partial");
  CHECK_FALSE(store.publish(ID, stored("new", hours(2))));
  CHECK(store.load(ID).value()->access_token_ == "new");
  CHECK_FALSE(std::filesystem::exists(folder.file(std::string(ID) + ".tmp")));

  const auto token_file = folder.file(std::string(ID) + ".token");
  for (
    const auto& text :
    {std::string("LEAK not json"), std::string(R"({"access_token":"LEAK"})"),
      std::string(R"({"access_token":"","expires_at_ns":1})"),
      std::string(R"({"access_token":"LEAK","expires_at_ns":"1"})"), std::string(R"(["LEAK"])"),
      R"({"access_token":"LEAK","expires_at_ns":1,"pad":")" + std::string(70000, 'x') + "\"}"}
  ) {
    CAPTURE(text.substr(0, 60));
    write_file(token_file, text);
    const auto loaded = store.load(ID);
    REQUIRE(loaded);
    CHECK_FALSE(loaded.value());
    // A malformed file is replaced by the next token.
    CHECK_FALSE(store.publish(ID, stored("replacement", hours(1))));
    CHECK(store.load(ID).value()->access_token_ == "replacement");
  }
}

TEST_CASE("each lock is its own open of the lock file and is closed on exec", "[network][token]") {
  const TempFolder folder;
  network::FileTokenStore store(folder.tokens(), milliseconds(150));
  auto first = store.lock(ID);
  REQUIRE(first);
  // The same store, the same identity: a second caller waits, then gives up.
  const auto second = store.lock(ID);
  REQUIRE_FALSE(second);
  check_store_error(second.error(), "token store: lock not obtained in time");
  // Another identity is not held up.
  CHECK(store.lock("kis-demo-0123abcd"));

  // A program started while the lock is held does not inherit it.
  pid_t child = 0;
  char program[] = "/bin/sleep";
  char duration[] = "5";
  char* arguments[] = {program, duration, nullptr};
  char* environment[] = {nullptr};
  REQUIRE(::posix_spawn(&child, program, nullptr, nullptr, arguments, environment) == 0);
  first.value().reset();
  CHECK(store.lock(ID));
  ::kill(child, SIGKILL);
  int status = 0;
  ::waitpid(child, &status, 0);
}

TEST_CASE("a lock held by another process makes the wait end with the store's error", "[network][token]") {
  const TempFolder folder;
  int ready[2];
  int release[2];
  REQUIRE(::pipe(ready) == 0);
  REQUIRE(::pipe(release) == 0);
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // The child holds the lock until the parent has waited, then leaves without unwinding into Catch2.
    ::close(ready[0]);
    ::close(release[1]);
    network::FileTokenStore holder(folder.tokens());
    auto lock = holder.lock(ID);
    const char held = lock ? 'y' : 'n';
    (void)!::write(ready[1], &held, 1);
    char done = 0;
    (void)!::read(release[0], &done, 1);
    ::_exit(0);
  }
  ::close(ready[1]);
  ::close(release[0]);
  char held = 0;
  const auto got = ::read(ready[0], &held, 1);

  network::FileTokenStore store(folder.tokens(), milliseconds(300));
  const auto started = std::chrono::steady_clock::now();
  const auto waited = store.lock(ID);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  const char done = 'x';
  (void)!::write(release[1], &done, 1);
  int status = 0;
  ::waitpid(child, &status, 0);
  ::close(ready[0]);
  ::close(release[1]);
  REQUIRE(got == 1);
  REQUIRE(held == 'y');
  REQUIRE_FALSE(waited);
  check_store_error(waited.error(), "token store: lock not obtained in time");
  CHECK(elapsed >= milliseconds(300));
  // The child is gone, and its lock with it.
  CHECK(store.lock(ID));
}

namespace {

/** Korean time `lifetime` from now, as Kiwoom's `expires_dt` states it (`yyyyMMddHHmmss`). */
std::string kiwoom_expiry(Clock::duration lifetime) {
  const std::time_t seconds_since_epoch = Clock::to_time_t(Clock::now() + lifetime) + 9 * 3600;
  std::tm parts{};
  ::gmtime_r(&seconds_since_epoch, &parts);
  char text[16];
  std::strftime(text, sizeof(text), "%Y%m%d%H%M%S", &parts);
  return text;
}

struct KisBroker {
  using Client = kis::KisBlockingClient;
  static constexpr const char* NAME = "kis";
  static constexpr const char* TOKEN = "/oauth2/tokenP";
  static constexpr const char* READ = "/uapi/domestic-stock/v1/quotations/chk-holiday";
  static constexpr const char* AUTHORIZATION = "authorization";

  static std::string token_body(const std::string& token, seconds lifetime) {
    return R"({"access_token":")" + token + R"(","token_type":"Bearer","expires_in":)" +
      std::to_string(lifetime.count()) + "}";
  }

  static test::LocalHttpsServer::Scripted read_reply() {
    return {
      200,
      R"({"rt_cd":"0","msg_cd":"MCA00000","msg1":"ok","ctx_area_fk":"T1","ctx_area_nk":"T1",)"
      R"("output":[{"bass_dt":"20260901","opnd_yn":"Y"}]})",
      {{"tr_cont", "D"}},
    };
  }

  static std::vector<test::LocalHttpsServer::Scripted> refusals() {
    return {{200, R"({"rt_cd":"1","msg_cd":"EGW00123","msg1":"expired"})", {}}};
  }

  static std::unique_ptr<Client> make(
    ssl::context& context, const test::LocalHttpsServer& server, std::shared_ptr<network::TokenStore> store,
    const std::string& key = "KEY"
  ) {
    kis::KisConfig config;
    config.from_json_text(R"({"app_key":")" + key + R"(","app_secret":"s","demo":false})", "test");
    auto client = std::make_unique<Client>(context, std::move(config), network::HttpClientOptions{}, std::move(store));
    kis::KisClientTestAccess::connect(*client, context, server.port());
    return client;
  }

  static auto read(Client& client) {
    return client.calendar(Date{2026, 9, 1}, true);
  }
};

struct KiwoomBroker {
  using Client = kiwoom::KiwoomBlockingClient;
  static constexpr const char* NAME = "kiwoom";
  static constexpr const char* TOKEN = "/oauth2/token";
  static constexpr const char* READ = "/api/dostk/acnt";
  static constexpr const char* AUTHORIZATION = "authorization";

  static std::string token_body(const std::string& token, seconds lifetime) {
    return R"({"expires_dt":")" + kiwoom_expiry(lifetime) + R"(","token_type":"bearer","token":")" + token +
      R"(","return_code":0,"return_msg":"ok"})";
  }

  static test::LocalHttpsServer::Scripted read_reply() {
    return {200, R"({"acctNo":"0123456789","return_code":0,"return_msg":"ok"})", {}};
  }

  static std::vector<test::LocalHttpsServer::Scripted> refusals() {
    return {
      {200, R"({"return_code":8005,"return_msg":"Token이 유효하지 않습니다"})", {}},
      {200, R"({"return_code":8010,"return_msg":"Token을 발급받은 IP와 서비스를 요청한 IP가 동일하지 않습니다"})", {}},
    };
  }

  static std::unique_ptr<Client> make(
    ssl::context& context, const test::LocalHttpsServer& server, std::shared_ptr<network::TokenStore> store,
    const std::string& key = "KEY"
  ) {
    kiwoom::KiwoomConfig config;
    config.from_json_text(R"({"appkey":")" + key + R"(","secretkey":"s","demo":false})", "test");
    auto client = std::make_unique<Client>(context, std::move(config), network::HttpClientOptions{}, std::move(store));
    kiwoom::KiwoomClientTestAccess::connect(*client, context, server.port());
    return client;
  }

  static auto read(Client& client) {
    return client.accounts();
  }
};

struct TossBroker {
  using Client = tossinvest::TossBlockingClient;
  static constexpr const char* NAME = "toss";
  static constexpr const char* TOKEN = "/oauth2/token";
  static constexpr const char* READ = "/api/v1/accounts";
  static constexpr const char* AUTHORIZATION = "Authorization";

  static std::string token_body(const std::string& token, seconds lifetime) {
    return R"({"access_token":")" + token + R"(","token_type":"Bearer","expires_in":)" +
      std::to_string(lifetime.count()) + "}";
  }

  static test::LocalHttpsServer::Scripted read_reply() {
    return {200, R"({"result":[{"accountNo":"12345678901","accountSeq":1,"accountType":"BROKERAGE"}]})", {}};
  }

  static std::vector<test::LocalHttpsServer::Scripted> refusals() {
    return {{401, R"({"error":{"requestId":"r1","code":"token-revoked","message":"revoked"}})", {}}};
  }

  static std::unique_ptr<Client> make(
    ssl::context& context, const test::LocalHttpsServer& server, std::shared_ptr<network::TokenStore> store,
    const std::string& key = "KEY"
  ) {
    tossinvest::TossConfig config;
    config.from_json_text(R"({"client_id":")" + key + R"(","client_secret":"s"})", "test");
    auto client = std::make_unique<Client>(context, std::move(config), network::HttpClientOptions{}, std::move(store));
    tossinvest::TossClientTestAccess::connect(*client, context, server.port());
    return client;
  }

  static auto read(Client& client) {
    return client.accounts();
  }
};

/** A loopback server, a client TLS context trusting it, and a file store in a temporary folder. */
struct Fixture {
  TempFolder folder_;
  test::LocalHttpsServer server_;
  ssl::context context_{ssl::context::tls_client};
  std::shared_ptr<network::FileTokenStore> store_;

  explicit Fixture(milliseconds lock_wait = milliseconds(5000))
  : store_(std::make_shared<network::FileTokenStore>(folder_.tokens(), lock_wait)) {
    test::trustLocalhostCertificate(context_);
    // A client that waits for a request lets go of its connection, so the next client is served.
    server_.set_idle_timeout(milliseconds(100));
  }

  /** The token stored for `identity`, or empty. */
  std::string stored_token(const std::string& identity) {
    auto lock = store_->lock(identity);
    REQUIRE(lock);
    auto loaded = store_->load(identity);
    REQUIRE(loaded);
    return loaded.value() ? loaded.value()->access_token_ : std::string();
  }
};

/** The identity a broker's test client stores its token under. */
template <typename Broker>
std::string identity_of(const std::string& key = "KEY") {
  if constexpr (std::is_same_v<Broker, KisBroker>) {
    return network::token_identity("kis", "real", key);
  } else if constexpr (std::is_same_v<Broker, KiwoomBroker>) {
    return network::token_identity("kiwoom", "real", key);
  } else {
    return network::token_identity("toss", "real", key);
  }
}

/** Checks two errors agree in every field a caller reads. */
void check_same_error(const BrokerError& returned, const BrokerError& expected) {
  CHECK(returned.category_ == expected.category_);
  CHECK(returned.broker_code_ == expected.broker_code_);
  CHECK(returned.http_status_ == expected.http_status_);
  CHECK(returned.message_ == expected.message_);
  CHECK(returned.reached_broker_ == expected.reached_broker_);
}

/** Checks a call failed locally because its token stopped being usable. */
void check_unusable(const BrokerError& error) {
  CHECK(error.category_ == ErrorCategory::AUTHENTICATION);
  CHECK_FALSE(error.reached_broker_);
  CHECK(error.broker_code_ == "token_unusable");
}

template <typename Broker>
void second_client_loads_the_shared_token() {
  Fixture fixture;
  fixture.server_.script(Broker::TOKEN, 200, Broker::token_body("issued", hours(24)));
  fixture.server_.scriptSequence(Broker::READ, {Broker::read_reply()});
  auto first = Broker::make(fixture.context_, fixture.server_, fixture.store_);
  REQUIRE(Broker::read(*first));
  CHECK(fixture.server_.scriptedCount(Broker::TOKEN) == 1);
  CHECK(fixture.stored_token(identity_of<Broker>()) == "issued");

  auto second = Broker::make(fixture.context_, fixture.server_, fixture.store_);
  REQUIRE(Broker::read(*second));
  CHECK(fixture.server_.scriptedCount(Broker::TOKEN) == 1);
  CHECK(fixture.server_.lastHeader(Broker::READ, Broker::AUTHORIZATION) == "Bearer issued");
  CHECK(second->token().access_token_ == "issued");
}

template <typename Broker>
void refused_token_is_discarded_and_replaced_once(const test::LocalHttpsServer::Scripted& refusal) {
  CAPTURE(Broker::NAME, refusal.body_);
  // What a client without a store returns for the same refusal.
  test::LocalHttpsServer reference;
  reference.scriptSequence(Broker::READ, {refusal});
  ssl::context reference_context(ssl::context::tls_client);
  test::trustLocalhostCertificate(reference_context);
  auto plain = Broker::make(reference_context, reference, nullptr);
  auto adopted = plain->token();
  adopted.access_token_ = "adopted";
  adopted.expires_at_ = Clock::now() + hours(20);
  REQUIRE(plain->adopt_token(adopted));
  const auto expected = Broker::read(*plain);
  REQUIRE_FALSE(expected);

  Fixture fixture;
  fixture.server_.scriptSequence(
    Broker::TOKEN,
    {{200, Broker::token_body("first", hours(24)), {}}, {200, Broker::token_body("second", hours(24)), {}}}
  );
  fixture.server_.scriptSequence(
    Broker::READ, {Broker::read_reply(), refusal, Broker::read_reply(), refusal, Broker::read_reply()}
  );
  const auto identity = identity_of<Broker>();
  auto one = Broker::make(fixture.context_, fixture.server_, fixture.store_);
  auto two = Broker::make(fixture.context_, fixture.server_, fixture.store_);
  REQUIRE(Broker::read(*one));
  REQUIRE(fixture.server_.scriptedCount(Broker::TOKEN) == 1);

  // The second client loads the shared token; the broker refuses it.
  const auto refused = Broker::read(*two);
  REQUIRE_FALSE(refused);
  check_same_error(refused.error(), expected.error());
  CHECK(fixture.server_.lastHeader(Broker::READ, Broker::AUTHORIZATION) == "Bearer first");
  CHECK(two->token().access_token_.empty());
  CHECK(fixture.stored_token(identity).empty());
  CHECK(fixture.server_.scriptedCount(Broker::TOKEN) == 1);

  // Its next call requests one token and shares it.
  REQUIRE(Broker::read(*two));
  CHECK(fixture.server_.scriptedCount(Broker::TOKEN) == 2);
  CHECK(fixture.server_.lastHeader(Broker::READ, Broker::AUTHORIZATION) == "Bearer second");
  CHECK(fixture.stored_token(identity) == "second");

  // The first client's old token is refused too; the newer stored token is not discarded for it.
  const auto stale = Broker::read(*one);
  REQUIRE_FALSE(stale);
  CHECK(fixture.server_.lastHeader(Broker::READ, Broker::AUTHORIZATION) == "Bearer first");
  CHECK(fixture.stored_token(identity) == "second");
  REQUIRE(Broker::read(*one));
  CHECK(fixture.server_.lastHeader(Broker::READ, Broker::AUTHORIZATION) == "Bearer second");
  CHECK(fixture.server_.scriptedCount(Broker::TOKEN) == 2);
  CHECK(fixture.server_.scriptedCount(Broker::READ) == 5);
}

template <typename Broker>
void token_unusable_before_dispatch_fails_locally() {
  CAPTURE(Broker::NAME);
  Fixture fixture;
  // Issued already inside the five-minute margin: unusable by the time the call is dispatched.
  fixture.server_.scriptSequence(
    Broker::TOKEN,
    {{200, Broker::token_body("brief", seconds(200)), {}}, {200, Broker::token_body("lasting", hours(24)), {}}}
  );
  fixture.server_.scriptSequence(Broker::READ, {Broker::read_reply()});
  auto client = Broker::make(fixture.context_, fixture.server_, fixture.store_);

  const auto failed = Broker::read(*client);
  REQUIRE_FALSE(failed);
  check_unusable(failed.error());
  CHECK(fixture.server_.scriptedCount(Broker::TOKEN) == 1);
  CHECK(fixture.server_.scriptedCount(Broker::READ) == 0);
  CHECK(fixture.stored_token(identity_of<Broker>()) == "brief");

  REQUIRE(Broker::read(*client));
  CHECK(fixture.server_.scriptedCount(Broker::TOKEN) == 2);
  CHECK(fixture.server_.lastHeader(Broker::READ, Broker::AUTHORIZATION) == "Bearer lasting");
  CHECK(fixture.stored_token(identity_of<Broker>()) == "lasting");
}

} // namespace

TEST_CASE("a second client on the store sends no token request while the shared token is valid", "[network][token]") {
  second_client_loads_the_shared_token<KisBroker>();
  second_client_loads_the_shared_token<KiwoomBroker>();
  second_client_loads_the_shared_token<TossBroker>();
}

TEST_CASE("a refused shared token is discarded and replaced by one token request in all", "[network][token]") {
  for (const auto& refusal : KisBroker::refusals()) {
    refused_token_is_discarded_and_replaced_once<KisBroker>(refusal);
  }
  for (const auto& refusal : KiwoomBroker::refusals()) {
    refused_token_is_discarded_and_replaced_once<KiwoomBroker>(refusal);
  }
  for (const auto& refusal : TossBroker::refusals()) {
    refused_token_is_discarded_and_replaced_once<TossBroker>(refusal);
  }
}

TEST_CASE(
  "a token unusable before dispatch fails the call locally and the next call prepares one", "[network][token]"
) {
  token_unusable_before_dispatch_fails_locally<KisBroker>();
  token_unusable_before_dispatch_fails_locally<KiwoomBroker>();
  token_unusable_before_dispatch_fails_locally<TossBroker>();
}

TEST_CASE("a token that expires between two pages fails the read without a token request", "[network][token]") {
  Fixture fixture;
  // Valid for one to two seconds past the margin; the first page's reply takes longer than that.
  fixture.server_.scriptSequence(
    KiwoomBroker::TOKEN,
    {{200, KiwoomBroker::token_body("brief", seconds(302)), {}},
      {200, KiwoomBroker::token_body("lasting", hours(24)), {}}}
  );
  const std::string row =
    R"({"ord_no":"0000069","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자","ord_qty":"1","ord_pric":"70000",)"
    R"("oso_qty":"1","io_tp_nm":"+매수","trde_tp":"보통","tm":"154113","cntr_qty":"0","stex_tp":"1"})";
  fixture.server_.scriptSequence(
    KiwoomBroker::READ,
    {
      {200, R"({"return_code":0,"oso":[)" + row + "]}", {{"cont-yn", "Y"}, {"next-key", "page-2"}}},
      {200, R"({"return_code":0,"oso":[)" + row + "]}", {{"cont-yn", "N"}}},
    }
  );
  fixture.server_.delayReplies(KiwoomBroker::READ, milliseconds(2500));
  auto client = KiwoomBroker::make(fixture.context_, fixture.server_, fixture.store_);

  const auto failed = client->open_orders(kiwoom::KiwoomAccount{});
  REQUIRE_FALSE(failed);
  check_unusable(failed.error());
  CHECK(fixture.server_.scriptedCount(KiwoomBroker::READ) == 1);
  CHECK(fixture.server_.scriptedCount(KiwoomBroker::TOKEN) == 1);

  fixture.server_.delayReplies(KiwoomBroker::READ, milliseconds(0));
  const auto listed = client->open_orders(kiwoom::KiwoomAccount{});
  REQUIRE(listed);
  CHECK(listed.value().orders_.size() == 1);
  CHECK(fixture.server_.scriptedCount(KiwoomBroker::TOKEN) == 2);
  CHECK(fixture.server_.lastHeader(KiwoomBroker::READ, "authorization") == "Bearer lasting");
}

TEST_CASE("a shared token near its expiry is replaced by one token request for both clients", "[network][token]") {
  Fixture fixture;
  const auto identity = identity_of<KisBroker>();
  {
    auto lock = fixture.store_->lock(identity);
    REQUIRE_FALSE(fixture.store_->publish(identity, stored("old", seconds(302))));
  }
  fixture.server_.script(KisBroker::TOKEN, 200, KisBroker::token_body("new", hours(24)));
  fixture.server_.scriptSequence(KisBroker::READ, {KisBroker::read_reply()});
  auto one = KisBroker::make(fixture.context_, fixture.server_, fixture.store_);
  auto two = KisBroker::make(fixture.context_, fixture.server_, fixture.store_);
  REQUIRE(KisBroker::read(*one));
  REQUIRE(KisBroker::read(*two));
  CHECK(fixture.server_.lastHeader(KisBroker::READ, "authorization") == "Bearer old");
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 0);

  // Both copies cross the margin.
  std::this_thread::sleep_for(milliseconds(2500));
  REQUIRE(KisBroker::read(*one));
  CHECK(fixture.server_.lastHeader(KisBroker::READ, "authorization") == "Bearer new");
  REQUIRE(KisBroker::read(*two));
  CHECK(fixture.server_.lastHeader(KisBroker::READ, "authorization") == "Bearer new");
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 1);
  CHECK(fixture.stored_token(identity) == "new");
}

TEST_CASE("a token response read only in part is not kept and the next call prepares again", "[network][token]") {
  Fixture fixture;
  const auto identity = identity_of<KisBroker>();
  // The token and its relative lifetime are read before the absolute expiry, which is not a string here.
  fixture.server_.scriptSequence(
    KisBroker::TOKEN,
    {
      {200, R"({"access_token":"LEAK-partial","expires_in":86400,"access_token_token_expired":42})", {}},
      {200, KisBroker::token_body("prepared", hours(24)), {}},
    }
  );
  fixture.server_.scriptSequence(KisBroker::READ, {KisBroker::read_reply()});
  auto client = KisBroker::make(fixture.context_, fixture.server_, fixture.store_);

  const auto failed = KisBroker::read(*client);
  REQUIRE_FALSE(failed);
  CHECK(failed.error().broker_code_ == "invalid_token_response");
  CHECK_FALSE(failed.error().reached_broker_);
  CHECK(failed.error().message_.find("LEAK") == std::string::npos);
  CHECK(client->token().access_token_.empty());
  CHECK(fixture.stored_token(identity).empty());
  CHECK(fixture.server_.scriptedCount(KisBroker::READ) == 0);

  REQUIRE(KisBroker::read(*client));
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 2);
  CHECK(fixture.server_.lastHeader(KisBroker::READ, "authorization") == "Bearer prepared");
  CHECK(fixture.stored_token(identity) == "prepared");
}

TEST_CASE("a failed token request stores nothing and a failed publish sends nothing", "[network][token]") {
  Fixture fixture;
  const auto identity = identity_of<KisBroker>();
  fixture.server_.scriptSequence(
    KisBroker::TOKEN,
    {
      {500, R"({"error_code":"EGW00133","error_description":"too soon"})", {}},
      {200, KisBroker::token_body("LEAK-unpublished", hours(24)), {}},
      {200, KisBroker::token_body("published", hours(24)), {}},
    }
  );
  fixture.server_.scriptSequence(KisBroker::READ, {KisBroker::read_reply()});
  auto client = KisBroker::make(fixture.context_, fixture.server_, fixture.store_);

  const auto refused = KisBroker::read(*client);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().category_ == ErrorCategory::AUTHENTICATION);
  CHECK_FALSE(refused.error().reached_broker_);
  CHECK(fixture.stored_token(identity).empty());
  // The lock was released: another holder takes it at once.
  network::FileTokenStore impatient(fixture.folder_.tokens(), milliseconds(0));
  CHECK(impatient.lock(identity));

  // The token is issued and cannot be stored: nothing is sent, and the token is not kept.
  REQUIRE(::mkdir(fixture.folder_.file(identity + ".tmp").c_str(), 0700) == 0);
  const auto unpublished = KisBroker::read(*client);
  REQUIRE_FALSE(unpublished);
  check_store_error(unpublished.error(), "token store: the token file could not be written");
  CHECK(unpublished.error().message_.find("unpublished") == std::string::npos);
  CHECK(client->token().access_token_.empty());
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 2);
  CHECK(fixture.server_.scriptedCount(KisBroker::READ) == 0);

  // The next call starts again from the lock.
  REQUIRE(::rmdir(fixture.folder_.file(identity + ".tmp").c_str()) == 0);
  REQUIRE(KisBroker::read(*client));
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 3);
  CHECK(fixture.server_.lastHeader(KisBroker::READ, "authorization") == "Bearer published");
  CHECK(fixture.stored_token(identity) == "published");
}

TEST_CASE("a KIS order refused for its shared token is not sent again", "[network][token]") {
  Fixture fixture;
  const auto identity = identity_of<KisBroker>();
  {
    auto lock = fixture.store_->lock(identity);
    REQUIRE_FALSE(fixture.store_->publish(identity, stored("shared", hours(20))));
  }
  const char* order = "/uapi/domestic-stock/v1/trading/order-cash";
  fixture.server_.script(order, 200, R"({"rt_cd":"1","msg_cd":"EGW00121","msg1":"invalid"})");
  fixture.server_.script(KisBroker::TOKEN, 200, KisBroker::token_body("fresh", hours(24)));
  fixture.server_.scriptSequence(KisBroker::READ, {KisBroker::read_reply()});
  auto client = KisBroker::make(fixture.context_, fixture.server_, fixture.store_);

  OrderRequest request;
  request.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  request.side_ = OrderSide::BUY;
  request.type_ = OrderType::LIMIT;
  request.time_in_force_ = TimeInForce::DAY;
  request.venue_ = Venue::KRX;
  request.quantity_ = Decimal64::parse("1");
  request.price_ = Decimal64::parse("70000");
  const auto placed = client->place_order(kis::KisAccount{"12345678", "01"}, request);
  REQUIRE_FALSE(placed);
  CHECK(placed.error().broker_code_ == "EGW00121");
  CHECK(placed.error().reached_broker_);
  CHECK(fixture.server_.lastHeader(order, "authorization") == "Bearer shared");
  CHECK(fixture.stored_token(identity).empty());

  REQUIRE(KisBroker::read(*client));
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 1);
  CHECK(fixture.server_.scriptedCount(order) == 1);
}

TEST_CASE("clients with different identities on one store do not wait on each other", "[network][token]") {
  // A KIS demo run's trading client and its real-account market-data client, for example.
  Fixture fixture(milliseconds(300));
  fixture.server_.script(KisBroker::TOKEN, 200, KisBroker::token_body("issued", hours(24)));
  fixture.server_.scriptSequence(KisBroker::READ, {KisBroker::read_reply()});
  auto held = fixture.store_->lock(identity_of<KisBroker>("KEY-A"));
  REQUIRE(held);

  auto other = KisBroker::make(fixture.context_, fixture.server_, fixture.store_, "KEY-B");
  const auto started = std::chrono::steady_clock::now();
  REQUIRE(KisBroker::read(*other));
  CHECK(std::chrono::steady_clock::now() - started < milliseconds(300));
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 1);

  auto waiting = KisBroker::make(fixture.context_, fixture.server_, fixture.store_, "KEY-A");
  const auto timed_out = KisBroker::read(*waiting);
  REQUIRE_FALSE(timed_out);
  check_store_error(timed_out.error(), "token store: lock not obtained in time");
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 1);
  CHECK(fixture.server_.scriptedCount(KisBroker::READ) == 1);

  held.value().reset();
  REQUIRE(KisBroker::read(*waiting));
  CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 2);
}

TEST_CASE("two threads on one identity sharing one store request one token", "[network][token]") {
  for (int round = 0; round < 3; ++round) {
    CAPTURE(round);
    Fixture fixture;
    fixture.server_.script(KisBroker::TOKEN, 200, KisBroker::token_body("issued", hours(24)));
    fixture.server_.scriptSequence(KisBroker::READ, {KisBroker::read_reply()});
    std::promise<void> start;
    const auto go = start.get_future().share();
    const auto call = [&fixture, go] {
      auto client = KisBroker::make(fixture.context_, fixture.server_, fixture.store_);
      go.wait();
      return KisBroker::read(*client).has_value();
    };
    auto first = std::async(std::launch::async, call);
    auto second = std::async(std::launch::async, call);
    start.set_value();
    CHECK(first.get());
    CHECK(second.get());
    CHECK(fixture.server_.scriptedCount(KisBroker::TOKEN) == 1);
    CHECK(fixture.server_.scriptedCount(KisBroker::READ) == 2);
  }
}

#endif
