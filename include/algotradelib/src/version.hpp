#pragma once

/**
 * The library's version, written here and nowhere else.
 *
 * `CMakeLists.txt` reads these four lines with a regular expression before `project()`, so the CMake
 * package version, `PROJECT_VERSION` and these macros cannot disagree. The header is the source rather
 * than a file CMake generates because a consumer who adds `include/` to their own build, with no CMake
 * at all, must still have a version to test.
 *
 * The form is `<major>.<minor>.<patch>.<dev>`, R's convention: a dev part other than 0 means "after
 * this release, unreleased", a moving target that no number identifies. `0.0.0.9000` is the
 * development version before any release.
 *
 * Test a release with `ATL_VERSION` and `ATL_VERSION_ENCODE`, and whether this is a development
 * checkout with `ATL_VERSION_DEV`:
 *
 * @code
 * #include <algotradelib/version>
 *
 * #if ATL_VERSION >= ATL_VERSION_ENCODE(0, 1, 0)
 * // something 0.1.0 added
 * #endif
 * @endcode
 */

/** The major version. */
#define ATL_VERSION_MAJOR 0
/** The minor version, below 1000. */
#define ATL_VERSION_MINOR 0
/** The patch version, below 1000. */
#define ATL_VERSION_PATCH 0
/**
 * The development part: 0 for a release, otherwise the unreleased work after `ATL_VERSION`.
 *
 * Always defined, so one form of check serves every build and `-Wundef` stays quiet.
 */
#define ATL_VERSION_DEV 9000

/**
 * Encodes a release as one integer, `major * 1000000 + minor * 1000 + patch`.
 *
 * Minor and patch must stay below 1000, or two releases would encode alike.
 */
#define ATL_VERSION_ENCODE(major, minor, patch) ((major) * 1000000 + (minor) * 1000 + (patch))

/**
 * The release this build is, or follows, encoded by `ATL_VERSION_ENCODE`.
 *
 * **Without the development part**, as `BOOST_VERSION` is. A development version is its release plus
 * a moving target, so no number would identify it, and adding the part would overflow a 32-bit `int`
 * once the major version reaches 1. A development checkout therefore compares equal to the release it
 * follows; `ATL_VERSION_DEV` tells them apart.
 */
#define ATL_VERSION ATL_VERSION_ENCODE(ATL_VERSION_MAJOR, ATL_VERSION_MINOR, ATL_VERSION_PATCH)

/** Stringizes its argument. An implementation detail of `ATL_VERSION_STRING`. */
#define ATL_VERSION_STRINGIZE_IMPL(x) #x
/** Stringizes its argument after expanding it. An implementation detail of `ATL_VERSION_STRING`. */
#define ATL_VERSION_STRINGIZE(x) ATL_VERSION_STRINGIZE_IMPL(x)

#if ATL_VERSION_DEV
/** The version as text, built from the parts above: `"0.0.0.9000"`, or `"0.1.0"` for a release. */
#define ATL_VERSION_STRING \
  ATL_VERSION_STRINGIZE(ATL_VERSION_MAJOR) "." ATL_VERSION_STRINGIZE(ATL_VERSION_MINOR) "." \
  ATL_VERSION_STRINGIZE(ATL_VERSION_PATCH) "." ATL_VERSION_STRINGIZE(ATL_VERSION_DEV)
#else
/** The version as text, built from the parts above: `"0.1.0"`; a development version adds `.dev`. */
#define ATL_VERSION_STRING \
  ATL_VERSION_STRINGIZE(ATL_VERSION_MAJOR) "." ATL_VERSION_STRINGIZE(ATL_VERSION_MINOR) "." \
  ATL_VERSION_STRINGIZE(ATL_VERSION_PATCH)
#endif
