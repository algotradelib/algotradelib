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

/** Whether `label` names a file inside the directory: lowercase letters, digits and `-`. */
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
 * Removes `--kiwoom-account LABEL` from the command line and returns the label, or nothing. Throws
 * when the flag has no value or the value is not a valid label.
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

/** Loads `config` from the directory's file for `label`, or from the environment without a directory. */
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
