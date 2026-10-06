#pragma once

/**
 * @file
 * @brief The library's version macros.
 *
 * The version is `<major>.<minor>.<patch>.<dev>`: a `dev` part other than 0 means unreleased work
 * after that release (`0.0.0.9000` is the development version before any release). CMake reads its
 * package version from this header, so the two always agree, and a build without CMake still has
 * the macros.
 *
 * @code
 * #include <algotradelib/version>
 *
 * #if ATL_VERSION >= ATL_VERSION_ENCODE(0, 1, 0)
 * // something 0.1.0 added
 * #endif
 * @endcode
 */

/** @brief The major version. */
#define ATL_VERSION_MAJOR 0
/** @brief The minor version, below 1000. */
#define ATL_VERSION_MINOR 0
/** @brief The patch version, below 1000. */
#define ATL_VERSION_PATCH 0
/**
 * @brief The development part: 0 for a release, otherwise unreleased work after `ATL_VERSION`.
 *
 * Always defined, so `#if ATL_VERSION_DEV` works under `-Wundef`.
 */
#define ATL_VERSION_DEV 9000

/**
 * @brief Encodes a release as one integer, `major * 1000000 + minor * 1000 + patch`.
 * @param major The major version.
 * @param minor The minor version, below 1000.
 * @param patch The patch version, below 1000.
 */
#define ATL_VERSION_ENCODE(major, minor, patch) ((major) * 1000000 + (minor) * 1000 + (patch))

/**
 * @brief The release this build is, or follows, encoded by `ATL_VERSION_ENCODE`.
 *
 * It excludes the development part, so a development checkout compares equal to the release it
 * follows; test `ATL_VERSION_DEV` to tell them apart.
 */
#define ATL_VERSION ATL_VERSION_ENCODE(ATL_VERSION_MAJOR, ATL_VERSION_MINOR, ATL_VERSION_PATCH)

/**
 * @brief Stringizes its argument (used by `ATL_VERSION_STRING`).
 * @param x The token.
 */
#define ATL_VERSION_STRINGIZE_IMPL(x) #x
/**
 * @brief Stringizes its argument after macro expansion (used by `ATL_VERSION_STRING`).
 * @param x The token.
 */
#define ATL_VERSION_STRINGIZE(x) ATL_VERSION_STRINGIZE_IMPL(x)

#if ATL_VERSION_DEV
/** @brief The version as text, such as `"0.0.0.9000"` (with the development part). */
#define ATL_VERSION_STRING \
  ATL_VERSION_STRINGIZE(ATL_VERSION_MAJOR) "." ATL_VERSION_STRINGIZE(ATL_VERSION_MINOR) "." \
  ATL_VERSION_STRINGIZE(ATL_VERSION_PATCH) "." ATL_VERSION_STRINGIZE(ATL_VERSION_DEV)
#else
/** @brief The version as text, such as `"0.1.0"`. */
#define ATL_VERSION_STRING \
  ATL_VERSION_STRINGIZE(ATL_VERSION_MAJOR) "." ATL_VERSION_STRINGIZE(ATL_VERSION_MINOR) "." \
  ATL_VERSION_STRINGIZE(ATL_VERSION_PATCH)
#endif
