#pragma once

/**
 * @file
 * @brief The examples' Kiwoom credentials: one JSON file per key, or the environment.
 *
 * Not part of the library. **Kiwoom issues one key per account** -- a Korean-stock account and a
 * US-stock account have a key each -- and no request carries an account number: the key is the
 * account. So each key has its own file, named by the environment and a label of your choosing:
 * `DIR/kiwoom/demo-kr.json`, `DIR/kiwoom/demo-us.json`, `DIR/kiwoom/real-kr.json`. `--kiwoom-account
 * LABEL` picks one (default `kr`), and `KIWOOM_DEMO` picks the environment (unset or `1`: demo; `0`:
 * real). Each file holds `appkey`, `secretkey` and `demo`, as `KiwoomConfig::from_json` reads them;
 * keep it `chmod 600`.
 *
 * The directory is `--credentials DIR`, else `ATL_CREDENTIALS_DIR` (`kis_credentials::take_directory`).
 * **With a directory, the environment's `KIWOOM_*` keys are not read**: a directory that lacks the file
 * stops the program rather than falling back, since the environment may hold another account's key.
 * Without one, the examples read `KIWOOM_APP_KEY` and `KIWOOM_SECRET_KEY` as before, and a label is
 * refused, because there is nothing for it to select.
 *
 * A label is the operator's name for an account; the library cannot tell which market a key trades.
 * The Kiwoom adapter calls domestic-stock endpoints only, so a US account's key authenticates and its
 * requests answer as Kiwoom answers them for that account.
 */

#include "kis_credentials.hpp"

#include <algotradelib/kiwoom>

#include <filesystem>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kiwoom_credentials {

/**
 * @brief Tests whether a label can name a file in the credentials directory.
 * @param label The label.
 * @return `true` if it is lowercase letters, digits and `-`, not starting with `-`.
 */
inline bool valid_label(std::string_view label) {
  if (label.empty() || label.front() == '-') {
    return false;
  }
  for (const char c : label) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Removes `--kiwoom-account LABEL` from the command line and returns the label.
 * @param argc The argument count; reduced by the arguments removed.
 * @param argv The arguments; the rest are moved down.
 * @return The label, or `std::nullopt` if the flag is absent.
 * @throws std::runtime_error if the flag has no value or the value is not a valid label.
 */
inline std::optional<std::string> take_label(int& argc, char** argv) {
  std::optional<std::string> label;
  int kept = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--kiwoom-account") {
      if (i + 1 >= argc || std::string_view(argv[i + 1]).rfind("--", 0) == 0) {
        throw std::runtime_error("--kiwoom-account needs a label, such as kr");
      }
      label = argv[++i];
      if (!valid_label(*label)) {
        throw std::runtime_error("--kiwoom-account takes lowercase letters, digits and '-', not \"" + *label + "\"");
      }
    } else {
      argv[kept++] = argv[i];
    }
  }
  argc = kept;
  return label;
}

/**
 * @brief Refuses any flag left on the command line after the credential flags were taken, so a
 *   mistyped `--kiwoom-acount us` stops instead of loading the default label's key.
 * @param argc The argument count.
 * @param argv The arguments.
 * @throws std::runtime_error naming the first unknown flag.
 */
inline void refuse_unknown_flags(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]).rfind("--", 0) == 0) {
      throw std::runtime_error(std::string("unknown option ") + argv[i]);
    }
  }
}

/**
 * @brief Returns transport options that pace a Kiwoom client at one request a second.
 *
 * A Kiwoom demo account refuses a second request to the same API within a few milliseconds of the
 * first (`1700`, "허용된 요청 개수를 초과하였습니다", with the API's id): a quote and a price-limit read
 * are both `ka10007`, so a loop reading both back to back loses one. Requests to different APIs back to
 * back, and to the same API 300 ms apart, were served (measured 2026-10-07). Kiwoom's specification
 * gives no numeric allowance. The library paces nothing unless asked (`HttpClientOptions::rate_limit_`),
 * so these examples ask, at the pace the KIS examples use.
 *
 * @return The options.
 */
inline algotradelib::network::HttpClientOptions paced_options() {
  algotradelib::network::HttpClientOptions options;
  options.rate_limit_ = algotradelib::network::RateLimitBudget{1, 1, 0};
  return options;
}

/**
 * @brief Loads the credentials from `DIR/kiwoom/{demo,real}-LABEL.json` (as `KIWOOM_DEMO` chooses;
 *   label `kr` by default), or from the environment without a directory, and prints where from.
 * @param directory The credentials directory, or `std::nullopt` to read the environment.
 * @param label The label, or `std::nullopt` for `kr`.
 * @param config The configuration to load into.
 * @param out Where to print the source.
 * @throws std::runtime_error if a label is given without a directory, or the file or variables are
 *   missing, invalid, or contradict the demo setting.
 */
inline void load(
  const std::optional<std::filesystem::path>& directory,
  const std::optional<std::string>& label,
  algotradelib::kiwoom::KiwoomConfig& config,
  std::ostream& out
) {
  if (!directory) {
    if (label) {
      throw std::runtime_error(
        "--kiwoom-account selects a file in a credentials directory; give --credentials DIR or set "
        "ATL_CREDENTIALS_DIR"
      );
    }
    config.from_env();
    out << "credentials: environment variables\n";
    return;
  }
  const std::string name = label.value_or("kr");
  const bool demo = algotradelib::kiwoom::KiwoomConfig::demo_from_env();
  const auto path = *directory / "kiwoom" / ((demo ? "demo-" : "real-") + name + ".json");
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error(
      "no " + path.string() + "; the credentials directory is given, so the environment is not read"
    );
  }
  config.from_json(path.string());
  if (config.is_demo_ != demo) {
    throw std::runtime_error(
      path.string() + " says \"demo\": " + (config.is_demo_ ? "true" : "false") +
      ", which contradicts its name"
    );
  }
  out << "credentials: " << path.string() << '\n';
  if (name != "kr") {
    out << "note: the Kiwoom adapter calls domestic-stock endpoints only\n";
  }
}

}  // namespace kiwoom_credentials
