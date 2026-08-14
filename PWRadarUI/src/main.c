/* Entry point for the verification console.
 *
 *   --selftest       run the PWRadarCore acceptance suite and exit non-zero on
 *                    any failure (CI gate)
 *   --scenario N     load canned scenario N at start-up
 *   --capture N F    run N frames, write the framebuffer to F as a binary PPM
 *                    and exit (headless regression imaging, no image library)
 *   --version        print the core version and exit
 *   --help           usage
 *
 * On Windows this links as a console subsystem application so --selftest output
 * is visible; the window is created either way.
 */
#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L      /* sigaction */
#  endif
#endif

#include "ui_app.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

/* --------------------------------------------------------------------------
 *  Interruption
 * --------------------------------------------------------------------------
 *  Ctrl+C, a kill, or the console window closing would otherwise terminate the
 *  process wherever it happened to be: the worker thread dies mid-CPI and
 *  app_destroy never runs, so a --capture in progress leaves a half-written
 *  image behind.  The handler does nothing but raise a flag - the only thing
 *  that is safe in a signal context - and the main loop leaves through the
 *  ordinary shutdown path, which joins the worker and frees every buffer.
 *
 *  This lives in the console, not in the library: a library that installs
 *  signal handlers steals them from whatever application links it.
 * ------------------------------------------------------------------------ */
static volatile sig_atomic_t g_interrupted = 0;

#if defined(_WIN32)
static BOOL WINAPI on_console_ctrl(DWORD type)
{
    switch (type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_interrupted = 1;
        return TRUE;
    default:
        return FALSE;
    }
}
#else
static void on_signal(int sig)
{
    (void)sig;
    g_interrupted = 1;
}
#endif

static void install_interrupt_handlers(void)
{
#if defined(_WIN32)
    (void)SetConsoleCtrlHandler(on_console_ctrl, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    (void)sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: a blocking read in the platform layer should come back
     * with EINTR so the loop notices the flag on this iteration. */
    sa.sa_flags = 0;
    (void)sigaction(SIGINT,  &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGHUP,  &sa, NULL);
#endif
}

static void print_usage(const char* argv0)
{
    printf("PWRadarSystem verification console\n"
           "usage: %s [options]\n"
           "  --selftest       run the PWRadarCore acceptance suite and exit\n"
           "  --scenario N     load canned scenario N at start-up\n"
           "  --capture N FILE render N frames, write FILE as a PPM and exit\n"
           "  --list-scenarios print the scenario table and exit\n"
           "  --version        print version information and exit\n"
           "  --help           this text\n",
           (argv0 != NULL) ? argv0 : "PWRadarUI");
}

static int run_selftest(void)
{
    char report[4096];
    const PWR_Status st = pwr_self_test(report, sizeof(report));
    fputs(report, stdout);
    printf("result: %s\n", (st == PWR_STATUS_OK) ? "PASS" : "FAIL");
    return (st == PWR_STATUS_OK) ? 0 : 1;
}

int main(int argc, char** argv)
{
    App  app;
    char err[512];
    int  i;
    int  scenario = -1;
    int  capture_frames = 0;
    const char* capture_path = NULL;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0)
        {
            printf("PWRadarCore %s  (ABI %u)\n", pwr_version_string(),
                   pwr_abi_version());
            return 0;
        }
        if (strcmp(argv[i], "--selftest") == 0)
        {
            return run_selftest();
        }
        if (strcmp(argv[i], "--list-scenarios") == 0)
        {
            uint32_t k = 0u;
            const char* n;
            while ((n = pwr_scenario_name(k)) != NULL)
            {
                printf("%2u  %s\n", k, n);
                ++k;
            }
            return 0;
        }
        if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc)
        {
            scenario = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--capture") == 0 && i + 2 < argc)
        {
            capture_frames = atoi(argv[++i]);
            capture_path   = argv[++i];
            if (capture_frames < 1) { capture_frames = 1; }
            continue;
        }
        fprintf(stderr, "unrecognised option: %s\n", argv[i]);
        print_usage(argv[0]);
        return 2;
    }

    if (pwr_abi_version() != (uint32_t)PWR_ABI_VERSION)
    {
        fprintf(stderr,
                "PWRadarCore ABI mismatch: this binary was built against %d, "
                "the library reports %u\n",
                PWR_ABI_VERSION, pwr_abi_version());
        return 3;
    }

    install_interrupt_handlers();

    err[0] = '\0';
    if (app_create(&app, err, sizeof(err)) == 0)
    {
        fprintf(stderr, "PWRadarUI: %s\n",
                (err[0] != '\0') ? err : "initialisation failed");
        return 4;
    }
    if (scenario >= 0 && pwr_scenario_name((uint32_t)scenario) != NULL)
    {
        (void)pwr_engine_load_scenario(app.eng, (uint32_t)scenario);
        app.scenario = scenario;
    }

    if (capture_path != NULL)
    {
        int n = 0;
        int ok;
        while (n < capture_frames && g_interrupted == 0 &&
               app_step(&app) != 0) { ++n; }
        /* Interrupted before the requested frame count: the image would not be
         * the one that was asked for, so write nothing rather than something
         * a regression check might accept. */
        if (g_interrupted != 0 && n < capture_frames)
        {
            fprintf(stderr, "interrupted after %d of %d frames, "
                            "%s not written\n", n, capture_frames, capture_path);
            app_destroy(&app);
            return 6;
        }
        ok = app_write_ppm(&app, capture_path);
        printf("captured %d frames -> %s (%s)\n", n, capture_path,
               (ok != 0) ? "ok" : "FAILED");
        app_destroy(&app);
        return (ok != 0) ? 0 : 5;
    }

    while (g_interrupted == 0 && app_step(&app) != 0) { }

    if (g_interrupted != 0)
    {
        fprintf(stderr, "interrupted, shutting down\n");
    }
    app_destroy(&app);
    return 0;
}
