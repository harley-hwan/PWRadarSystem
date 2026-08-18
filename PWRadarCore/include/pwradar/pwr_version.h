/* Library and ABI version. */
#ifndef PWRADAR_PWR_VERSION_H
#define PWRADAR_PWR_VERSION_H

/* --------------------------------------------------------------------------
 *  Semantic version of the implementation.
 * ------------------------------------------------------------------------ */
#define PWR_VERSION_MAJOR   1
#define PWR_VERSION_MINOR   0
#define PWR_VERSION_PATCH   0

/* --------------------------------------------------------------------------
 *  Binary interface version.
 *
 *  PWR_ABI_VERSION must be bumped whenever the layout of any structure that
 *  crosses the DLL boundary changes, or whenever an exported function
 *  signature changes.  The host application compares the value it was
 *  compiled against (this macro) with the value reported by the loaded
 *  binary (pwr_abi_version()) and refuses to run on mismatch.
 * ------------------------------------------------------------------------ */
#define PWR_ABI_VERSION     7

#define PWR_STRINGIFY_(x)   #x
#define PWR_STRINGIFY(x)    PWR_STRINGIFY_(x)

#define PWR_VERSION_STRING                                                    \
    PWR_STRINGIFY(PWR_VERSION_MAJOR) "."                                      \
    PWR_STRINGIFY(PWR_VERSION_MINOR) "."                                      \
    PWR_STRINGIFY(PWR_VERSION_PATCH)

#endif /* PWRADAR_PWR_VERSION_H */
