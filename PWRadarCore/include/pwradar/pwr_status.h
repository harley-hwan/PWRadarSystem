/* Status codes. Every fallible exported entry point returns PWR_Status. */
#ifndef PWRADAR_PWR_STATUS_H
#define PWRADAR_PWR_STATUS_H

/* --------------------------------------------------------------------------
 *  Naming
 *  ------
 *  The success code is PWR_STATUS_OK, not the more obvious PWR_OK, because
 *  <winuser.h> defines the legacy WM_POWER broadcast constants as object-like
 *  macros:
 *
 *      #define PWR_OK              1
 *      #define PWR_FAIL            (-1)
 *      #define PWR_SUSPENDREQUEST  1
 *
 *  A macro wins against an enumerator regardless of scope, so an enumerator
 *  called PWR_OK is silently rewritten to 1 in every translation unit that
 *  pulls in <windows.h> - turning "return success" into "return
 *  PWR_STATUS_PENDING" and making every caller's `!= PWR_STATUS_OK` check
 *  fire.  The symptom is remote from the cause, it compiles clean at -Werror,
 *  and the Linux build is entirely unaffected, so the only reliable defence is
 *  to not use the name at all.
 *
 *  Note what does NOT work: a `#if defined(PWR_OK)` tripwire in this header.
 *  This header is included before <windows.h> in every real translation unit,
 *  so the macro does not exist yet when the tripwire is evaluated.  The guard
 *  has to be placed where the include order is known, which is why it lives in
 *  src/pwr_guard.c - a translation unit that includes <windows.h> first on
 *  purpose - backed by tools/check_name_collisions.py, which sweeps every
 *  public identifier against the whole SDK independently of include order.
 *
 *  Status codes.  0 == success, negative == failure.  The numeric values are
 *  part of the ABI: never renumber, only append.  src/pwr_guard.c pins them
 *  with _Static_assert.
 * ------------------------------------------------------------------------ */

typedef enum PWR_Status
{
    PWR_STATUS_OK               =   0,  /* operation completed             */
    PWR_STATUS_PENDING          =   1,  /* accepted, completes later       */
    PWR_STATUS_NO_DATA          =   2,  /* nothing available yet (not err) */

    PWR_ERR_UNKNOWN             =  -1,
    PWR_ERR_INVALID_ARGUMENT    =  -2,
    PWR_ERR_NULL_POINTER        =  -3,
    PWR_ERR_OUT_OF_MEMORY       =  -4,
    PWR_ERR_OUT_OF_RANGE        =  -5,
    PWR_ERR_INVALID_STATE       =  -6,
    PWR_ERR_NOT_INITIALISED     =  -7,
    PWR_ERR_ALREADY_EXISTS      =  -8,
    PWR_ERR_NOT_FOUND           =  -9,
    PWR_ERR_CAPACITY_EXCEEDED   = -10,
    PWR_ERR_ABI_MISMATCH        = -11,
    PWR_ERR_CONFIG_INVALID      = -12,
    PWR_ERR_THREAD              = -13,
    PWR_ERR_TIMEOUT             = -14,
    PWR_ERR_NOT_SUPPORTED       = -15,
    PWR_ERR_IO                  = -16,
    PWR_ERR_NUMERIC             = -17   /* NaN / Inf / divergence detected  */
} PWR_Status;

#define PWR_SUCCEEDED(s)    ((int)(s) >= 0)
#define PWR_FAILED(s)       ((int)(s) <  0)

#endif /* PWRADAR_PWR_STATUS_H */
