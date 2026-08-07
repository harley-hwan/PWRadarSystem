/* ==========================================================================
 *  PWRadarSystem - PWRadarCore
 *  File    : pwr_names.c
 *  Purpose : Human-readable names for every public status code and enumeration
 *            so the presentation layer never has to duplicate the tables.
 *  Language: ISO C17
 * ========================================================================== */
#include "pwr_core.h"

PWR_EXPORT(const char*) pwr_status_string(PWR_Status status)
{
    switch (status)
    {
    case PWR_STATUS_OK:             return "OK";
    case PWR_STATUS_PENDING:        return "pending";
    case PWR_STATUS_NO_DATA:        return "no data";
    case PWR_ERR_UNKNOWN:           return "unknown error";
    case PWR_ERR_INVALID_ARGUMENT:  return "invalid argument";
    case PWR_ERR_NULL_POINTER:      return "null pointer";
    case PWR_ERR_OUT_OF_MEMORY:     return "out of memory";
    case PWR_ERR_OUT_OF_RANGE:      return "out of range";
    case PWR_ERR_INVALID_STATE:     return "invalid state";
    case PWR_ERR_NOT_INITIALISED:   return "not initialised";
    case PWR_ERR_ALREADY_EXISTS:    return "already exists";
    case PWR_ERR_NOT_FOUND:         return "not found";
    case PWR_ERR_CAPACITY_EXCEEDED: return "capacity exceeded";
    case PWR_ERR_ABI_MISMATCH:      return "ABI mismatch";
    case PWR_ERR_CONFIG_INVALID:    return "configuration invalid";
    case PWR_ERR_THREAD:            return "thread failure";
    case PWR_ERR_TIMEOUT:           return "timeout";
    case PWR_ERR_NOT_SUPPORTED:     return "not supported";
    case PWR_ERR_IO:                return "I/O failure";
    case PWR_ERR_NUMERIC:           return "numeric failure";
    default:                        return "unrecognised status";
    }
}

PWR_EXPORT(const char*) pwr_window_name(PWR_WindowType w)
{
    static const char* const names[PWR_WIN_COUNT] = {
        "Rectangular", "Hann", "Hamming", "Blackman", "Blackman-Harris",
        "Taylor -35 dB", "Taylor -50 dB", "Kaiser b=6", "Chebyshev -60 dB"
    };
    return ((int)w >= 0 && (int)w < PWR_WIN_COUNT) ? names[(int)w] : "?";
}

PWR_EXPORT(const char*) pwr_mti_name(PWR_MtiMode m)
{
    static const char* const names[PWR_MTI_COUNT] = {
        "Off", "DC removal", "2-pulse", "3-pulse", "4-pulse"
    };
    return ((int)m >= 0 && (int)m < PWR_MTI_COUNT) ? names[(int)m] : "?";
}

PWR_EXPORT(const char*) pwr_cfar_name(PWR_CfarType c)
{
    static const char* const names[PWR_CFAR_COUNT] = {
        "CA-CFAR", "GO-CFAR", "SO-CFAR", "OS-CFAR", "TM-CFAR"
    };
    return ((int)c >= 0 && (int)c < PWR_CFAR_COUNT) ? names[(int)c] : "?";
}

PWR_EXPORT(const char*) pwr_swerling_name(PWR_Swerling s)
{
    static const char* const names[PWR_SWERLING_COUNT] = {
        "SW 0 (steady)", "SW 1 (scan, Ray)", "SW 2 (pulse, Ray)",
        "SW 3 (scan, chi4)", "SW 4 (pulse, chi4)"
    };
    return ((int)s >= 0 && (int)s < PWR_SWERLING_COUNT) ? names[(int)s] : "?";
}

PWR_EXPORT(const char*) pwr_class_name(PWR_TargetClass c)
{
    static const char* const names[PWR_CLASS_COUNT] = {
        "UNKNOWN", "SURFACE", "AIR", "ROTARY", "MISSILE", "UAV", "CLUTTER"
    };
    return ((int)c >= 0 && (int)c < PWR_CLASS_COUNT) ? names[(int)c] : "?";
}

PWR_EXPORT(const char*) pwr_track_state_name(PWR_TrackState s)
{
    static const char* const names[PWR_TRACK_STATE_COUNT] = {
        "FREE", "TENTATIVE", "CONFIRMED", "COAST", "TERMINATED"
    };
    return ((int)s >= 0 && (int)s < PWR_TRACK_STATE_COUNT) ? names[(int)s] : "?";
}
