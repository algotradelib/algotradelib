#pragma once

/**
 * @file
 * @brief The examples' KIS credentials: one JSON file per account, or the environment.
 *
 * Not part of the library. With a credentials directory -- `--credentials DIR` on the command line,
 * else `ATL_CREDENTIALS_DIR` -- the examples read `DIR/kis/demo.json` or `DIR/kis/real.json`, as
 * `KIS_DEMO` chooses (unset or `1`: demo; `0`: real). Without one they read the environment
 * variables, as before. A directory that is given but lacks the file stops the program: it never
 * falls back to the environment, which may hold the other account's keys.
 *
 * Each file holds `app_key`, `app_secret`, `account_number`, `account_product_code` and `demo`
 * (`KisConfig::from_json`, `KisAccount::from_json`). Keep it `chmod 600`.
 */

#include <algotradelib/kis>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kis_credentials {

/** Returns `ATL_CREDENTIALS_DIR`, or nothing. For an example that parses no arguments of its own. */
inline std::optional<std::filesystem::path> environment_directory() {
  const char* variable = std::getenv("ATL_CREDENTIALS_DIR");
  if (variable != nullptr && *variable == '\0') {
    throw std::runtime_error("ATL_CREDENTIALS_DIR is set but empty");
  }
  return variable != nullptr ? std::optional<std::filesystem::path>(variable) : std::nullopt;
}

/**
 * Removes `--credentials DIR` from the command line and returns the directory it names, else
 * `ATL_CREDENTIALS_DIR`, else nothing. Throws when `--credentials` has no value, so a mistyped
 * command never falls back to another source.
 */
inline std::optional<std::filesystem::path> take_directory(int& argc, char** argv) {
  std::optional<std::filesystem::path> directory;
  int kept = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--credentials") {
      if (i + 1 >= argc || std::string_view(argv[i + 1]).empty() ||
          std::string_view(argv[i + 1]).rfind("--", 0) == 0) {
        throw std::runtime_error("--credentials needs a directory");
      }
      directory = argv[++i];
    } else {
      argv[kept++] = argv[i];
    }
  }
  argc = kept;
  return directory ? directory : environment_directory();
}

/**
 * Returns transport options that pace a KIS client at one request a second.
 *
 * Measured on 2026-10-02 (`labs/FINDINGS.md` F26): a KIS demo account refuses reads closer than about
 * a second apart (`EGW00201`), and with real keys the 20-page holiday calendar is refused at 8 a
 * second and served at 1. The library paces nothing unless asked (`HttpClientOptions::rate_limit_`),
 * so these examples ask. A key with a larger allowance may raise it. With a burst of 1 the reserve
 * makes no difference: every traffic class may spend the one request.
 */
inline algotradelib::network::HttpClientOptions paced_options() {
  algotradelib::network::HttpClientOptions options;
  options.rate_limit_ = algotradelib::network::RateLimitBudget{1, 1, 0};
  return options;
}

/** Returns `DIR/kis/<name>`, or throws when it is missing. */
inline std::filesystem::path required_file(const std::filesystem::path& directory, const char* name) {
  const auto path = directory / "kis" / name;
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error(
      "no " + path.string() + "; the credentials directory is given, so the environment is not read"
    );
  }
  return path;
}

/** Loads `config` from `path` and refuses a file whose `"demo"` contradicts its name. */
inline void load_config(algotradelib::kis::KisConfig& config, const std::filesystem::path& path, bool demo) {
  config.from_json(path.string());
  if (config.is_demo_ != demo) {
    throw std::runtime_error(
      path.string() + " says \"demo\": " + (config.is_demo_ ? "true" : "false") +
      ", which contradicts its name"
    );
  }
}

/**
 * Loads the account `KIS_DEMO` chooses into `config` and, when given, `account`, and prints where
 * from (a path or "environment variables", never a key).
 */
inline void load(
  const std::optional<std::filesystem::path>& directory,
  algotradelib::kis::KisConfig& config,
  algotradelib::kis::KisAccount* account,
  std::ostream& out
) {
  if (!directory) {
    config.from_env();
    if (account != nullptr) {
      account->from_env();
    }
    out << "credentials: environment variables\n";
    return;
  }
  const bool demo = algotradelib::kis::KisConfig::demo_from_env();
  const auto path = required_file(*directory, demo ? "demo.json" : "real.json");
  load_config(config, path, demo);
  if (account != nullptr) {
    account->from_json(path.string());
  }
  out << "credentials: " << path.string() << '\n';
}

/**
 * Returns a config for market data alone, from `DIR/kis/real.json`, or nothing when there is no
 * directory or no such file. Pair it with an empty `KisAccount`, which refuses every account request
 * and order before any I/O.
 */
inline std::optional<algotradelib::kis::KisConfig> market_data_config(
  const std::optional<std::filesystem::path>& directory
) {
  if (!directory || !std::filesystem::exists(*directory / "kis" / "real.json")) {
    return std::nullopt;
  }
  algotradelib::kis::KisConfig config;
  load_config(config, *directory / "kis" / "real.json", false);
  return config;
}

} // namespace kis_credentials
