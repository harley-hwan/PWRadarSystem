/* Compile-time guards. Emits no code; exists so that a class of silent
 * miscompilation becomes a build error instead.
 *
 * <winuser.h> defines the legacy WM_POWER constants as object-like macros:
 *
 *     #define PWR_OK              1
 *     #define PWR_FAIL            (-1)
 *     #define PWR_SUSPENDREQUEST  1
 *     #define PWR_CRITICALRESUME  3
 *
 * A macro beats an enumerator regardless of scope, so an enumerator named
 * PWR_OK is rewritten to 1 in every translation unit that reaches <windows.h>:
 * "return PWR_OK;" becomes "return 1;" and every caller's success test fires.
 * It compiles clean at -Werror and the Linux build is unaffected.
 *
 * A tripwire inside pwr_status.h cannot catch it. The public headers are
 * included before <windows.h> in every real translation unit, so the macro
 * does not exist yet when the tripwire is evaluated, and no ordering rule can
 * be imposed on downstream code.
 *
 * This file controls the order instead: platform headers first, public API
 * second, then assert that every public name is still an identifier. It is
 * compiled into the library, so a collision breaks the build whatever any
 * other translation unit does. tools/check_name_collisions.py sweeps the whole
 * SDK ahead of the compiler, covering names this file does not include.
 */

/* ---- 1. System headers first, deliberately ----------------------------- */
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>          /* drags in winuser.h and its PWR_* macros   */
#  include <winuser.h>
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* ---- 2. Public API second --------------------------------------------- */
#include "pwradar/pwr_api.h"

/* ==========================================================================
 *  Collision guard
 *  ---------------
 *  Every public status name must still be an identifier here, i.e. not a
 *  macro.  If one of these fires, RENAME OUR ENUMERATOR.  Do not #undef the
 *  macro: some downstream consumer legitimately wants it, and the collision
 *  returns the moment include order changes.
 * ========================================================================== */
#if defined(PWR_STATUS_OK)          || defined(PWR_STATUS_PENDING)        || \
    defined(PWR_STATUS_NO_DATA)     || defined(PWR_ERR_UNKNOWN)           || \
    defined(PWR_ERR_INVALID_ARGUMENT) || defined(PWR_ERR_NULL_POINTER)    || \
    defined(PWR_ERR_OUT_OF_MEMORY)  || defined(PWR_ERR_OUT_OF_RANGE)      || \
    defined(PWR_ERR_INVALID_STATE)  || defined(PWR_ERR_NOT_INITIALISED)   || \
    defined(PWR_ERR_ALREADY_EXISTS) || defined(PWR_ERR_NOT_FOUND)         || \
    defined(PWR_ERR_CAPACITY_EXCEEDED) || defined(PWR_ERR_ABI_MISMATCH)   || \
    defined(PWR_ERR_CONFIG_INVALID) || defined(PWR_ERR_THREAD)            || \
    defined(PWR_ERR_TIMEOUT)        || defined(PWR_ERR_NOT_SUPPORTED)     || \
    defined(PWR_ERR_IO)             || defined(PWR_ERR_NUMERIC)
#  error "A PWR_Status enumerator is shadowed by a macro from a system header. \
Rename the enumerator in pwradar/pwr_status.h - do NOT #undef the macro."
#endif

/* The enumerators must also survive as *values*, not merely as spellings: a
 * macro expanding to a valid integer constant would satisfy the check above
 * only if it were function-like.  Pin the ABI numbering while we are here -
 * these values are part of the published interface and must never be
 * renumbered, only appended to. */
_Static_assert(PWR_STATUS_OK            ==   0, "PWR_STATUS_OK must be 0");
_Static_assert(PWR_STATUS_PENDING       ==   1, "ABI: PWR_STATUS_PENDING");
_Static_assert(PWR_STATUS_NO_DATA       ==   2, "ABI: PWR_STATUS_NO_DATA");
_Static_assert(PWR_ERR_UNKNOWN          ==  -1, "ABI: PWR_ERR_UNKNOWN");
_Static_assert(PWR_ERR_INVALID_ARGUMENT ==  -2, "ABI: PWR_ERR_INVALID_ARGUMENT");
_Static_assert(PWR_ERR_NULL_POINTER     ==  -3, "ABI: PWR_ERR_NULL_POINTER");
_Static_assert(PWR_ERR_OUT_OF_MEMORY    ==  -4, "ABI: PWR_ERR_OUT_OF_MEMORY");
_Static_assert(PWR_ERR_OUT_OF_RANGE     ==  -5, "ABI: PWR_ERR_OUT_OF_RANGE");
_Static_assert(PWR_ERR_INVALID_STATE    ==  -6, "ABI: PWR_ERR_INVALID_STATE");
_Static_assert(PWR_ERR_NOT_INITIALISED  ==  -7, "ABI: PWR_ERR_NOT_INITIALISED");
_Static_assert(PWR_ERR_ALREADY_EXISTS   ==  -8, "ABI: PWR_ERR_ALREADY_EXISTS");
_Static_assert(PWR_ERR_NOT_FOUND        ==  -9, "ABI: PWR_ERR_NOT_FOUND");
_Static_assert(PWR_ERR_CAPACITY_EXCEEDED== -10, "ABI: PWR_ERR_CAPACITY_EXCEEDED");
_Static_assert(PWR_ERR_ABI_MISMATCH     == -11, "ABI: PWR_ERR_ABI_MISMATCH");
_Static_assert(PWR_ERR_CONFIG_INVALID   == -12, "ABI: PWR_ERR_CONFIG_INVALID");
_Static_assert(PWR_ERR_THREAD           == -13, "ABI: PWR_ERR_THREAD");
_Static_assert(PWR_ERR_TIMEOUT          == -14, "ABI: PWR_ERR_TIMEOUT");
_Static_assert(PWR_ERR_NOT_SUPPORTED    == -15, "ABI: PWR_ERR_NOT_SUPPORTED");
_Static_assert(PWR_ERR_IO               == -16, "ABI: PWR_ERR_IO");
_Static_assert(PWR_ERR_NUMERIC          == -17, "ABI: PWR_ERR_NUMERIC");

/* Success / failure classification must agree with the sign convention the
 * whole library relies on. */
_Static_assert(PWR_SUCCEEDED(PWR_STATUS_OK),        "PWR_SUCCEEDED(OK)");
_Static_assert(PWR_SUCCEEDED(PWR_STATUS_NO_DATA),   "NO_DATA is not an error");
_Static_assert(PWR_FAILED(PWR_ERR_THREAD),          "PWR_FAILED(THREAD)");
_Static_assert(PWR_FAILED(PWR_ERR_NUMERIC),         "PWR_FAILED(NUMERIC)");

/* ==========================================================================
 *  Representation assumptions made by the DSP chain and the ABI
 * ========================================================================== */
_Static_assert(CHAR_BIT == 8,                 "8-bit bytes assumed");
_Static_assert(sizeof(pwr_real) == 4u || sizeof(pwr_real) == 8u,
               "pwr_real must be float or double");
_Static_assert(sizeof(PWR_Complex) == 2u * sizeof(pwr_real),
               "PWR_Complex must be two packed reals: the FFT, the matched "
               "filter and the I/Q simulator all alias it as a flat array");
_Static_assert(offsetof(PWR_Complex, re) == 0u,
               "PWR_Complex.re must be first");
_Static_assert(offsetof(PWR_Complex, im) == sizeof(pwr_real),
               "PWR_Complex must have no padding between re and im");

/* ISO C forbids an empty translation unit.  Give the file one internal-linkage
 * declaration rather than an object, so nothing reaches the binary. */
typedef int pwr_guard_translation_unit_is_not_empty;
