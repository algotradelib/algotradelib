// Sharing KIS's trading calendar through a calendar cache: blocking clients on one cache standing in for programs on one machine, through the client's own request path against the loopback server, and the file cache's writer.

#include <algotradelib/kis>
#include <algotradelib/network>

#include <catch2/catch_test_macros.hpp>

#include "local_https_server.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ratio>
#include <string>
#include <tuple>
#include <utility>

#if !defined(_WIN32)
#include <stdlib.h>
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

  /** The asynchronous client a blocking client runs. */
  static KisClient& async(KisBlockingClient& client) {
    return *client.client_;
  }

  /** Replaces the clock a blocking client checks a calendar's date against. */
  static void set_clock(KisBlockingClient& client, std::chrono::system_clock::time_point (*now)()) {
    client.now_ = now;
  }

  /** Makes the token a blocking client holds one that expired, or none. */
  static void spoil_token(KisBlockingClient& client, bool expired) {
    client.client_->token_.expires_at_ = std::chrono::system_clock::now() - std::chrono::hours(1);
    if (!expired) {
      client.client_->token_ = KisRest{};
    }
  }
};
} // namespace algotradelib::kis

namespace {

using Clock = std::chrono::system_clock;

constexpr const char* TOKEN = "/oauth2/tokenP";
constexpr const char* HOLIDAY = "/uapi/domestic-stock/v1/quotations/chk-holiday";
constexpr Date FROM{2026, 9, 1};

/** One page of KIS's holiday inquiry covering 2026-09-01 to 2026-09-03. */
test::LocalHttpsServer::Scripted holiday_reply() {
  return {
    200,
    R"({"rt_cd":"0","msg_cd":"MCA00000","msg1":"ok","ctx_area_fk":"","ctx_area_nk":"","output":[)"
    R"({"bass_dt":"20260901","opnd_yn":"Y"},{"bass_dt":"20260902","opnd_yn":"Y"},{"bass_dt":"20260903","opnd_yn":"N"}]})",
    {{"tr_cont", "D"}},
  };
}

/** A calendar the cache could hold: KR, covering `FROM`, received now. */
TradingCalendar covering() {
  TradingCalendar calendar;
  calendar.days_ = {{FROM, true}, {Date{2026, 9, 2}, false}};
  calendar.received_at_ = Clock::now();
  return calendar;
}

/** A loopback server answering KIS's token and holiday requests, and a client TLS context trusting it. */
struct Server {
  test::LocalHttpsServer server_;
  ssl::context context_{ssl::context::tls_client};

  Server() {
    test::trustLocalhostCertificate(context_);
    // A client that waits for a request lets go of its connection, so the next client is served.
    server_.set_idle_timeout(std::chrono::milliseconds(100));
    server_.script(TOKEN, 200, R"({"access_token":"issued","token_type":"Bearer","expires_in":86400})");
    server_.scriptSequence(HOLIDAY, {holiday_reply()});
  }

  std::unique_ptr<kis::KisBlockingClient> client(
    std::shared_ptr<network::TokenStore> store = nullptr, std::shared_ptr<network::CalendarCache> calendars = nullptr
  ) {
    kis::KisConfig config;
    config.from_json_text(R"({"app_key":"KEY","app_secret":"s","demo":false})", "test");
    auto client = std::make_unique<kis::KisBlockingClient>(
      context_, std::move(config), network::HttpClientOptions{}, std::move(store), std::move(calendars)
    );
    kis::KisClientTestAccess::connect(*client, context_, server_.port());
    return client;
  }

  std::size_t tokens() const {
    return server_.scriptedCount(TOKEN);
  }

  std::size_t inquiries() const {
    return server_.scriptedCount(HOLIDAY);
  }
};

} // namespace

TEST_CASE("an adopted calendar answers without a request and a refused one changes nothing", "[network][calendar]") {
  Server server;
  auto blocking = server.client();
  auto& client = kis::KisClientTestAccess::async(*blocking);
  REQUIRE(client.adopt_calendar(covering()));

  auto foreign = covering();
  foreign.country_ = Country::US;
  auto invalid = covering();
  invalid.days_.push_back({Date{2026, 2, 30}, true});
  for (const auto& refused : {foreign, TradingCalendar{}, invalid}) {
    CHECK_FALSE(client.adopt_calendar(refused));
  }
  // The calendar adopted first still answers.
  const auto answered = blocking->calendar(FROM);
  REQUIRE(answered);
  CHECK(answered.value().days_.size() == 2);
  CHECK(server.inquiries() == 0);
  CHECK(server.tokens() == 0);
}

#if !defined(_WIN32)

namespace algotradelib::network {
struct FileCalendarCacheTestAccess {
  /** Writes a temporary file of a publication without renaming it; its name. */
  static std::string write(FileCalendarCache& cache, const TradingCalendar& calendar) {
    FileCalendarCache::Descriptor folder;
    REQUIRE_FALSE(cache.open_folder("kis-kr", folder));
    auto temporary = FileCalendarCache::write_temporary(folder, "kis-kr", FileCalendarCache::serialize(calendar));
    REQUIRE(temporary);
    return temporary.value();
  }

  /** Renames a publication's temporary file over the calendar file. */
  static void rename(FileCalendarCache& cache, const std::string& temporary) {
    FileCalendarCache::Descriptor folder;
    REQUIRE_FALSE(cache.open_folder("kis-kr", folder));
    REQUIRE_FALSE(FileCalendarCache::rename_temporary(folder, "kis-kr", temporary));
  }
};
} // namespace algotradelib::network

namespace {

/** A private folder under the system's temporary folder, removed with what it holds. */
class TempFolder {
public:
  TempFolder() {
    std::string pattern = (std::filesystem::temp_directory_path() / "atl-calendar-cache-XXXXXX").string();
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

  /** A path inside the folder. */
  std::string at(const std::string& name) const {
    return path_ + "/" + name;
  }

private:
  std::string path_;
};

/** A file's contents. */
std::string contents_of(const std::string& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/** A cache that returns `held_` if it is set and otherwise fails, and fails every publication, counting calls. */
class TestCache : public network::CalendarCache {
public:
  Result<std::optional<TradingCalendar>> load(const std::string&) override {
    ++loads_;
    if (held_) {
      return held_;
    }
    return network::calendar_cache_error("calendar cache: failed");
  }

  std::optional<BrokerError> publish(const std::string&, const TradingCalendar&) override {
    ++publications_;
    return network::calendar_cache_error("calendar cache: failed");
  }

  std::optional<TradingCalendar> held_;
  int loads_{0};
  int publications_{0};
};

/** The instant a test's clock returns. */
Clock::time_point fixed_now;

/** 2026-09-01T00:00:00Z; midnight in Seoul is 15:00 UTC. */
const auto SEPTEMBER = Clock::from_time_t(1788220800);

/** Checks two errors agree in every field a caller reads. */
void check_same_error(const BrokerError& returned, const BrokerError& expected) {
  CHECK(returned.category_ == expected.category_);
  CHECK(returned.broker_code_ == expected.broker_code_);
  CHECK(returned.http_status_ == expected.http_status_);
  CHECK(returned.message_ == expected.message_);
  CHECK(returned.reached_broker_ == expected.reached_broker_);
}

} // namespace

TEST_CASE(
  "a second client on one calendar cache sends no holiday or token request the same day", "[network][calendar]"
) {
  const TempFolder folder;
  Server server;
  const auto cache = std::make_shared<network::FileCalendarCache>(folder.at("calendars"));
  auto first = server.client(std::make_shared<network::FileTokenStore>(folder.at("tokens-one")), cache);
  const auto fetched = first->calendar(FROM);
  REQUIRE(fetched);
  CHECK(server.inquiries() == 1);
  CHECK(server.tokens() == 1);
  const auto published = cache->load("kis-kr");
  REQUIRE(published);
  REQUIRE(published.value());
  CHECK(published.value()->days_.size() == 3);
  CHECK(published.value()->received_at_ == fetched.value().received_at_);

  // A store holding no token: a token step would request one.
  auto second = server.client(std::make_shared<network::FileTokenStore>(folder.at("tokens-two")), cache);
  const auto loaded = second->calendar(FROM);
  REQUIRE(loaded);
  CHECK(loaded.value().days_.size() == 3);
  CHECK(loaded.value().received_at_ == fetched.value().received_at_);
  CHECK(second->token().access_token_.empty());

  // Held by the client now: answered without the file, and still the day after.
  std::filesystem::remove(folder.at("calendars/kis-kr.calendar"));
  REQUIRE(second->calendar(Date{2026, 9, 3}));
  kis::KisClientTestAccess::set_clock(*second, [] {
    return Clock::now() + std::chrono::hours(24);
  });
  REQUIRE(second->calendar(FROM));
  CHECK(server.inquiries() == 1);
  CHECK(server.tokens() == 1);

  // `refresh` asks KIS again although the client holds a calendar, and stores the answer.
  REQUIRE(first->calendar(FROM, true));
  CHECK(server.inquiries() == 2);
  CHECK(cache->load("kis-kr").value()->received_at_ > fetched.value().received_at_);
}

TEST_CASE(
  "a calendar file of yesterday of another country or not covering the date is replaced", "[network][calendar]"
) {
  auto yesterday = covering();
  yesterday.received_at_ -= std::chrono::hours(24);
  auto foreign = covering();
  foreign.country_ = Country::US;
  auto elsewhere = covering();
  elsewhere.days_ = {{Date{2026, 8, 31}, true}, {Date{2026, 9, 2}, true}};
  for (const auto& stored : {yesterday, foreign, elsewhere}) {
    const TempFolder folder;
    Server server;
    const auto cache = std::make_shared<network::FileCalendarCache>(folder.at("calendars"));
    REQUIRE_FALSE(cache->publish("kis-kr", stored));
    REQUIRE(server.client(nullptr, cache)->calendar(FROM));
    CHECK(server.inquiries() == 1);
    const auto replaced = cache->load("kis-kr");
    REQUIRE(replaced);
    REQUIRE(replaced.value());
    CHECK(replaced.value()->country_ == Country::KR);
    CHECK(replaced.value()->days_.size() == 3);
  }

  // Not well formed: read as no calendar, and replaced.
  const TempFolder folder;
  Server server;
  const auto cache = std::make_shared<network::FileCalendarCache>(folder.at("calendars"));
  REQUIRE_FALSE(cache->publish("kis-kr", covering()));
  {
    std::ofstream out(folder.at("calendars/kis-kr.calendar"));
    out << R"({"format":1,"country":"KR"})";
  }
  const auto malformed = cache->load("kis-kr");
  REQUIRE(malformed);
  CHECK_FALSE(malformed.value());
  REQUIRE(server.client(nullptr, cache)->calendar(FROM));
  CHECK(server.inquiries() == 1);
  CHECK(cache->load("kis-kr").value()->days_.size() == 3);
}

TEST_CASE("a failing cache is a miss and a KIS or token error comes back unchanged", "[network][calendar]") {
  {
    // A load that fails asks KIS; a publication that fails still returns the calendar, and KIS is asked once.
    Server server;
    const auto failing = std::make_shared<TestCache>();
    const auto fetched = server.client(nullptr, failing)->calendar(FROM);
    REQUIRE(fetched);
    CHECK(fetched.value().days_.size() == 3);
    CHECK(failing->loads_ == 1);
    CHECK(failing->publications_ == 1);
    CHECK(server.inquiries() == 1);
  }

  const TempFolder folder;
  const auto cache = std::make_shared<network::FileCalendarCache>(folder.at("calendars"));
  REQUIRE_FALSE(cache->publish("kis-kr", covering()));
  const auto written = contents_of(folder.at("calendars/kis-kr.calendar"));

  // KIS refuses the inquiry: the same error as a client without a cache, and the file as it was.
  const test::LocalHttpsServer::Scripted refusal{200, R"({"rt_cd":"1","msg_cd":"OPSQ0002","msg1":"refused"})", {}};
  Server plain;
  plain.server_.scriptSequence(HOLIDAY, {refusal});
  const auto expected = plain.client()->calendar(FROM, true);
  REQUIRE_FALSE(expected);
  Server server;
  server.server_.scriptSequence(HOLIDAY, {refusal});
  const auto refused = server.client(nullptr, cache)->calendar(FROM, true);
  REQUIRE_FALSE(refused);
  check_same_error(refused.error(), expected.error());
  CHECK(contents_of(folder.at("calendars/kis-kr.calendar")) == written);

  // The token is refused: the same.
  server.server_.script(TOKEN, 500, R"({"error_code":"EGW00133","error_description":"too soon"})");
  plain.server_.script(TOKEN, 500, R"({"error_code":"EGW00133","error_description":"too soon"})");
  const auto expected_token = plain.client()->calendar(FROM, true);
  REQUIRE_FALSE(expected_token);
  const auto token_refused = server.client(nullptr, cache)->calendar(FROM, true);
  REQUIRE_FALSE(token_refused);
  check_same_error(token_refused.error(), expected_token.error());
  CHECK(contents_of(folder.at("calendars/kis-kr.calendar")) == written);
}

TEST_CASE("a stored calendar received at the clock's limits is a miss", "[network][calendar]") {
  for (const auto received : {Clock::time_point::max(), Clock::time_point::min()}) {
    Server server;
    const auto cache = std::make_shared<TestCache>();
    cache->held_ = covering();
    cache->held_->received_at_ = received;
    REQUIRE(server.client(nullptr, cache)->calendar(FROM));
    CHECK(server.inquiries() == 1);
  }

  // A clock at its limit too: no date in Seoul on either side, so neither a hit nor a publication.
  Server server;
  const auto cache = std::make_shared<TestCache>();
  cache->held_ = covering();
  cache->held_->received_at_ = Clock::time_point::max();
  auto client = server.client(nullptr, cache);
  kis::KisClientTestAccess::set_clock(*client, [] {
    return Clock::time_point::min();
  });
  REQUIRE(client->calendar(FROM));
  CHECK(server.inquiries() == 1);
  CHECK(cache->publications_ == 0);
}

TEST_CASE("a stored calendar is reused by its date in Seoul and not in UTC", "[network][calendar]") {
  using std::chrono::hours;
  // Checked at 01:00 and received at 23:00 on 2 September in Seoul, on two dates in UTC: reused.
  // Checked at 23:00 on 1 September and received at 01:00 on 2 September in Seoul, on one date in UTC: not reused.
  for (
    const auto& [check, received, reused] : {
      std::tuple{SEPTEMBER + hours(16), SEPTEMBER + hours(38), true},
      std::tuple{SEPTEMBER + hours(14), SEPTEMBER + hours(16), false},
    }
  ) {
    const TempFolder folder;
    Server server;
    const auto cache = std::make_shared<network::FileCalendarCache>(folder.at("calendars"));
    auto stored = covering();
    stored.received_at_ = received;
    REQUIRE_FALSE(cache->publish("kis-kr", stored));
    auto client = server.client(nullptr, cache);
    fixed_now = check;
    kis::KisClientTestAccess::set_clock(*client, [] {
      return fixed_now;
    });
    REQUIRE(client->calendar(FROM));
    CHECK(server.inquiries() == (reused ? 0U : 1U));
  }
}

TEST_CASE("a calendar fetched across midnight in Seoul is returned and not published", "[network][calendar]") {
  const TempFolder folder;
  Server server;
  const auto cache = std::make_shared<network::FileCalendarCache>(folder.at("calendars"));
  auto client = server.client(nullptr, cache);
  // The fetch starts on the date in UTC its last page arrives on, but on another date in Seoul.
  const auto now = Clock::now();
  const auto utc_midnight = std::chrono::floor<std::chrono::duration<std::int64_t, std::ratio<86400>>>(now);
  fixed_now = now - utc_midnight < std::chrono::hours(15) ? utc_midnight + std::chrono::hours(23) : utc_midnight;
  kis::KisClientTestAccess::set_clock(*client, [] {
    return fixed_now;
  });
  const auto fetched = client->calendar(FROM);
  REQUIRE(fetched);
  CHECK(fetched.value().days_.size() == 3);
  CHECK(server.inquiries() == 1);
  const auto stored = cache->load("kis-kr");
  REQUIRE(stored);
  CHECK_FALSE(stored.value());
}

TEST_CASE("a calendar received outside the file's range is refused and the old file kept", "[network][calendar]") {
  const TempFolder folder;
  network::FileCalendarCache cache(folder.at("calendars"));
  // The latest receipt the file holds, to the clock's resolution, and its opposite.
  const auto latest =
    std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds::max() - std::chrono::hours(24));
  for (const auto edge : {latest, -latest}) {
    auto calendar = covering();
    calendar.received_at_ = Clock::time_point(edge);
    REQUIRE_FALSE(cache.publish("kis-kr", calendar));
    CHECK(cache.load("kis-kr").value()->received_at_ == calendar.received_at_);
  }
  const auto written = contents_of(folder.at("calendars/kis-kr.calendar"));
  // One tick beyond either; the clock's own limits are beyond signed 64-bit nanoseconds on a coarser clock (macOS).
  for (
    const auto beyond :
    {Clock::time_point(latest + Clock::duration(1)), Clock::time_point(-latest - Clock::duration(1)),
      Clock::time_point::max(), Clock::time_point::min()}
  ) {
    auto calendar = covering();
    calendar.received_at_ = beyond;
    const auto refused = cache.publish("kis-kr", calendar);
    REQUIRE(refused);
    CHECK(refused->category_ == ErrorCategory::TRANSPORT);
    CHECK(refused->broker_code_ == "calendar_cache");
    CHECK(contents_of(folder.at("calendars/kis-kr.calendar")) == written);
  }
}

TEST_CASE("interleaved publications each rename a whole file of their own", "[network][calendar]") {
  const TempFolder folder;
  network::FileCalendarCache cache(folder.at("calendars"));
  auto first = covering();
  auto second = covering();
  second.days_.push_back({Date{2026, 9, 3}, true});

  // A writes; B writes and renames; A renames.
  const auto a = network::FileCalendarCacheTestAccess::write(cache, first);
  const auto b = network::FileCalendarCacheTestAccess::write(cache, second);
  CHECK(a != b);
  network::FileCalendarCacheTestAccess::rename(cache, b);
  CHECK(std::filesystem::exists(folder.at("calendars/" + a)));
  CHECK(cache.load("kis-kr").value()->days_.size() == 3);
  network::FileCalendarCacheTestAccess::rename(cache, a);
  CHECK(cache.load("kis-kr").value()->days_.size() == 2);
  CHECK(std::filesystem::exists(folder.at("calendars/kis-kr.calendar")));
  CHECK_FALSE(std::filesystem::exists(folder.at("calendars/" + a)));
  CHECK_FALSE(std::filesystem::exists(folder.at("calendars/" + b)));
}

TEST_CASE("without a cache a calendar call makes the same requests as before", "[network][calendar]") {
  const TempFolder folder;
  Server server;
  const auto store = std::make_shared<network::FileTokenStore>(folder.at("tokens"));
  auto client = server.client(store);
  REQUIRE(client->calendar(FROM));
  CHECK(server.tokens() == 1);
  CHECK(server.inquiries() == 1);

  // With a token store, a hit in the client's memory still takes the token step first.
  for (const bool expired : {true, false}) {
    kis::KisClientTestAccess::spoil_token(*client, expired);
    std::filesystem::remove(folder.at("tokens/" + network::token_identity("kis", "real", "KEY") + ".token"));
    REQUIRE(client->calendar(FROM));
  }
  CHECK(server.tokens() == 3);
  CHECK(server.inquiries() == 1);
  // An invalid date too, before it is refused.
  kis::KisClientTestAccess::spoil_token(*client, false);
  std::filesystem::remove(folder.at("tokens/" + network::token_identity("kis", "real", "KEY") + ".token"));
  const auto invalid = client->calendar(Date{2026, 2, 30});
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().broker_code_ == "invalid_date");
  CHECK(server.tokens() == 4);

  // Without a token store, neither sends anything.
  auto plain = server.client();
  REQUIRE(plain->calendar(FROM));
  REQUIRE(plain->calendar(FROM));
  CHECK_FALSE(plain->calendar(Date{2026, 2, 30}));
  CHECK(server.tokens() == 5);
  CHECK(server.inquiries() == 2);

  // With a cache, neither takes the token step.
  auto cached = server.client(store, std::make_shared<network::FileCalendarCache>(folder.at("calendars")));
  REQUIRE(cached->calendar(FROM));
  CHECK(server.tokens() == 5);
  kis::KisClientTestAccess::spoil_token(*cached, false);
  std::filesystem::remove(folder.at("tokens/" + network::token_identity("kis", "real", "KEY") + ".token"));
  REQUIRE(cached->calendar(FROM));
  const auto refused = cached->calendar(Date{2026, 2, 30});
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "invalid_date");
  CHECK(server.tokens() == 5);
  CHECK(server.inquiries() == 3);
}

#endif
