/* Entry point for the verification console.
 *
 *   --selftest       run the PWRadarCore acceptance suite and exit non-zero on
 *                    any failure (CI gate)
 *   --score [SECS]   run scenarios headless and score the track picture against
 *                    ground truth, exit non-zero if a long-lived target was
 *                    never tracked (regression gate)
 *   --scenario N     load canned scenario N at start-up
 *   --save-scenario F  write the loaded scenario out as an editable text file
 *   --load-scenario F  apply a scenario file, so a baseline can be named,
 *                    edited and replayed without a recompile
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

#include <math.h>
#include <signal.h>
#include <stdarg.h>
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

/* --------------------------------------------------------------------------
 *  Visible fatal errors
 *  --------------------
 *  Every way this program can refuse to start ends in a message on stderr, and
 *  from a shell that is exactly right.  Launched from Explorer or from a build
 *  script's `start`, it is exactly wrong: the console subsystem opens a window
 *  that closes the instant the process exits, so the one line explaining why
 *  nothing appeared is the one line nobody can read.  The symptom is "I ran it
 *  and no screen came up", which is indistinguishable from a hang.
 *
 *  On Windows the same text therefore also goes to a message box, which
 *  survives the process.
 * ------------------------------------------------------------------------ */
static void fatal(const char* fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "PWRadarUI: %s\n", msg);
#if defined(_WIN32)
    (void)MessageBoxA(NULL, msg, "PWRadarSystem - cannot start",
                      MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
}

static void print_usage(const char* argv0)
{
    printf("PWRadarSystem verification console\n"
           "usage: %s [options]\n"
           "  --selftest [quick]  run the acceptance suite and exit; 'quick'\n"
           "                   skips the four cases that integrate over\n"
           "                   thousands of CPIs, so it finishes in a second\n"
           "  --score [SECS]   score a scenario against ground truth with no\n"
           "                   window and exit; all scenarios unless --scenario\n"
           "                   was given first (default 30 s of scenario time)\n"
           "  --scenario N     load canned scenario N at start-up\n"
           "  --save-scenario F  write the loaded scenario to F and exit\n"
           "  --load-scenario F  apply the scenario file F at start-up\n"
           "  --capture N FILE render N frames, write FILE as a PPM and exit\n"
           "  --list-scenarios print the scenario table and exit\n"
           "  --version        print version information and exit\n"
           "  --help           this text\n",
           (argv0 != NULL) ? argv0 : "PWRadarUI");
}

static int run_selftest(int quick)
{
    char report[4096];
    const PWR_Status st = (quick != 0)
        ? pwr_self_test_quick(report, sizeof(report))
        : pwr_self_test(report, sizeof(report));
    fputs(report, stdout);
    printf("result: %s\n", (st == PWR_STATUS_OK) ? "PASS" : "FAIL");
    return (st == PWR_STATUS_OK) ? 0 : 1;
}

/* --------------------------------------------------------------------------
 *  Headless scoring
 *  ----------------
 *  Runs a scenario with no window and no renderer, scoring the published
 *  frames against the ground truth with the same PWR_Scorer the console's
 *  Verify tab uses.  This is what makes the scenario acceptance criteria
 *  machine-checkable: until there was a way to get these numbers out of a
 *  batch run they lived only on screen and were discarded when the window
 *  closed, so nothing could tell a two-decibel calibration slip from a good
 *  build.
 *
 *  The pass criterion is deliberately the weakest one that still catches a
 *  real regression: every target that was alive long enough to be confirmed
 *  must have been held by a track at some point.  Anything tighter - a
 *  completeness floor, an RMS ceiling, a spurious-track budget - belongs with
 *  a stored, named baseline rather than hard-coded here.
 *
 *  The one exemption is not a fudge but a statement about the radar: two
 *  targets closer together than the initiation-inhibit radius cannot be
 *  separated at track level by design, so the second of such a pair is
 *  reported as merged rather than missed.  That is exactly what the scenario-4
 *  range pair is, and reading it as a failure would mean the gate could never
 *  pass on a picture the design is behaving correctly on.
 * ------------------------------------------------------------------------ */
#define SCORE_CONFIRM_GRACE_S  12.0     /* about three scans plus a margin */

/* Whole file into a NUL-terminated buffer.  The library converts text to
 * structs and never touches a file, so opening one is the console's job. */
static char* read_text_file(const char* path)
{
    FILE* fp = fopen(path, "rb");
    char* buf = NULL;
    long  n;

    if (fp == NULL) { return NULL; }
    if (fseek(fp, 0, SEEK_END) == 0 && (n = ftell(fp)) >= 0 &&
        fseek(fp, 0, SEEK_SET) == 0)
    {
        buf = (char*)malloc((size_t)n + 1u);
        if (buf != NULL)
        {
            const size_t got = fread(buf, 1u, (size_t)n, fp);
            buf[got] = '\0';
        }
    }
    (void)fclose(fp);
    return buf;
}

static int write_text_file(const char* path, const char* text)
{
    FILE* fp = fopen(path, "wb");
    size_t n;
    if (fp == NULL) { return 0; }
    n = strlen(text);
    if (fwrite(text, 1u, n, fp) != n) { (void)fclose(fp); return 0; }
    return (fclose(fp) == 0) ? 1 : 0;
}

/* Writes a canned scenario - or one already loaded from text - out as a file,
 * which is what turns the compiled-in scenarios into an editable baseline. */
static int run_save(int scenario, const char* text, const char* path)
{
    PWR_RadarConfig cfg;
    PWR_Engine* eng = NULL;
    char*  out;
    char   err[PWR_ERRMSG_LEN];
    size_t need = 0u;
    int    rc = 0;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread = 0;
    if (pwr_engine_create(&cfg, &eng) != PWR_STATUS_OK)
    {
        fprintf(stderr, "engine creation failed\n");
        return 1;
    }
    if (scenario >= 0 && pwr_scenario_name((uint32_t)scenario) != NULL)
    {
        (void)pwr_engine_load_scenario(eng, (uint32_t)scenario);
    }
    if (text != NULL &&
        pwr_engine_scenario_load(eng, text, err, sizeof(err)) != PWR_STATUS_OK)
    {
        fprintf(stderr, "%s\n", err);
        pwr_engine_destroy(eng);
        return 7;
    }

    (void)pwr_engine_scenario_save(eng, NULL, 0u, &need);
    out = (char*)malloc(need + 1u);
    if (out != NULL &&
        pwr_engine_scenario_save(eng, out, need + 1u, NULL) == PWR_STATUS_OK)
    {
        rc = write_text_file(path, out);
    }
    free(out);
    pwr_engine_destroy(eng);
    if (rc == 0) { fprintf(stderr, "cannot write %s\n", path); return 8; }
    printf("wrote %s (%u bytes)\n", path, (unsigned)need);
    return 0;
}

static int score_scenario(uint32_t index, const char* text, double seconds,
                          int* out_checked)
{
    PWR_RadarConfig    cfg;
    PWR_DerivedMetrics dm;
    PWR_Engine*        eng = NULL;
    PWR_Scorer         sc;
    PWR_Stats          st;
    PWR_SimTarget      truth[PWR_MAX_SIM_TARGETS];
    uint32_t           truth_n = 0u;
    uint32_t           i, cpi, n_cpi;
    int                failures = 0;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread = 0;              /* stepped on this thread          */
    cfg.deterministic = 1;
    if (pwr_engine_create(&cfg, &eng) != PWR_STATUS_OK)
    {
        fprintf(stderr, "scenario %u: engine creation failed\n", index);
        return 1;
    }
    (void)pwr_engine_load_scenario(eng, index);
    if (text != NULL)
    {
        char err[PWR_ERRMSG_LEN];
        if (pwr_engine_scenario_load(eng, text, err, sizeof(err)) != PWR_STATUS_OK)
        {
            fprintf(stderr, "%s\n", err);
            pwr_engine_destroy(eng);
            return 1;
        }
    }
    (void)pwr_engine_get_config(eng, &cfg);
    (void)pwr_engine_get_metrics(eng, &dm);
    (void)pwr_scorer_init(&sc, 0.0);

    n_cpi = (uint32_t)(seconds / dm.cpi_duration_s);
    for (cpi = 0u; cpi < n_cpi; ++cpi)
    {
        PWR_Frame f;
        (void)pwr_engine_step(eng, 1u);
        if (pwr_engine_frame_acquire(eng, &f) != PWR_STATUS_OK) { continue; }
        (void)pwr_scorer_update(&sc, &f);
        /* Last positions, so an untracked target can be told apart from one
         * that was merged into a neighbour's track. */
        truth_n = (f.truth_count < PWR_MAX_SIM_TARGETS) ? f.truth_count
                                                        : PWR_MAX_SIM_TARGETS;
        memcpy(truth, f.truth, (size_t)truth_n * sizeof(PWR_SimTarget));
        (void)pwr_engine_frame_release(eng);
    }
    (void)pwr_engine_get_stats(eng, &st);

    printf("\n%2u  %s\n", index,
           (text != NULL) ? "loaded from file" : pwr_scenario_name(index));
    printf("    %.1f s, %u CPI, %llu plots, tracks +%u -%u, dropped plots %u\n",
           (double)n_cpi * dm.cpi_duration_s, n_cpi,
           (unsigned long long)st.detection_total,
           st.tracks_created_total, st.tracks_deleted_total,
           st.detections_dropped);
    printf("    spurious %u, redundant %u\n",
           sc.summary.spurious, sc.summary.redundant);

    for (i = 0u; i < PWR_MAX_SIM_TARGETS; ++i)
    {
        const PWR_TargetScore* t = &sc.targets[i];
        int held;
        if (t->truth_id == 0 || t->time_active_s <= 0.0) { continue; }
        held = (t->first_track_s >= 0.0) ? 1 : 0;
        ++(*out_checked);
        if (held != 0)
        {
            printf("    %-12s active %6.1f s  completeness %5.1f%%  "
                   "TTT %5.2f s  rmse %5.0f m\n",
                   t->label, t->time_active_s, 100.0 * t->completeness,
                   t->first_track_s - t->first_seen_s, t->err_rms_m);
        }
        else
        {
            /* Was a neighbour inside the initiation-inhibit radius tracked
             * instead?  Then the two are unresolvable at track level by
             * design and this is a merge, not a miss. */
            int merged = 0;
            uint32_t a, b;
            for (a = 0u; a < truth_n && merged == 0; ++a)
            {
                if (truth[a].id != t->truth_id) { continue; }
                for (b = 0u; b < truth_n; ++b)
                {
                    const PWR_TargetScore* o;
                    double dx, dy;
                    uint32_t q;
                    if (b == a || truth[b].enabled == 0) { continue; }
                    dx = truth[b].x_m - truth[a].x_m;
                    dy = truth[b].y_m - truth[a].y_m;
                    if (sqrt(dx * dx + dy * dy) >
                        (double)cfg.tracker.init_inhibit_m) { continue; }
                    for (q = 0u; q < PWR_MAX_SIM_TARGETS; ++q)
                    {
                        o = &sc.targets[q];
                        if (o->truth_id == truth[b].id &&
                            o->first_track_s >= 0.0) { merged = 1; break; }
                    }
                    if (merged != 0) { break; }
                }
            }
            printf("    %-12s active %6.1f s  %s\n", t->label, t->time_active_s,
                   (merged != 0)
                       ? "merged into a neighbour inside the inhibit radius"
                       : "NEVER TRACKED");
            if (merged == 0 && t->time_active_s > SCORE_CONFIRM_GRACE_S)
            {
                ++failures;
            }
        }
    }
    pwr_engine_destroy(eng);
    return failures;
}

static int run_score(int scenario, double seconds, const char* text)
{
    int failures = 0, checked = 0;
    printf("PWRadarCore %s  (ABI %u)\n", pwr_version_string(), pwr_abi_version());
    if (text != NULL)
    {
        failures = score_scenario(0u, text, seconds, &checked);
    }
    else if (scenario >= 0)
    {
        if (pwr_scenario_name((uint32_t)scenario) == NULL)
        {
            fprintf(stderr, "no such scenario: %d\n", scenario);
            return 2;
        }
        failures = score_scenario((uint32_t)scenario, NULL, seconds, &checked);
    }
    else
    {
        uint32_t k = 0u;
        while (pwr_scenario_name(k) != NULL)
        {
            failures += score_scenario(k, NULL, seconds, &checked);
            ++k;
        }
    }
    printf("\n%d target(s) scored, %d failure(s)\n", checked, failures);
    printf("result: %s\n", (failures == 0) ? "PASS" : "FAIL");
    return (failures == 0) ? 0 : 1;
}

int main(int argc, char** argv)
{
    App  app;
    char err[512];
    int  i;
    int  scenario = -1;
    int  capture_frames = 0;
    int  do_selftest = 0;
    double score_secs = 0.0;
    const char* capture_path = NULL;
    const char* save_path = NULL;
    const char* load_path = NULL;
    char* scenario_text = NULL;

    /* The parse loop only records what was asked for; dispatch happens after
     * it, so an option never depends on whether it came before or after the
     * --scenario it refers to. */
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
            do_selftest = 1;
            if (i + 1 < argc && strcmp(argv[i + 1], "quick") == 0)
            {
                do_selftest = 2;
                ++i;
            }
            continue;
        }
        if (strcmp(argv[i], "--score") == 0)
        {
            score_secs = 30.0;
            if (i + 1 < argc && atof(argv[i + 1]) > 0.0)
            {
                score_secs = atof(argv[++i]);
            }
            continue;
        }
        if (strcmp(argv[i], "--save-scenario") == 0 && i + 1 < argc)
        {
            save_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--load-scenario") == 0 && i + 1 < argc)
        {
            load_path = argv[++i];
            continue;
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

    if (do_selftest != 0) { return run_selftest(do_selftest == 2); }

    if (load_path != NULL)
    {
        scenario_text = read_text_file(load_path);
        if (scenario_text == NULL)
        {
            fatal("cannot read the scenario file %s", load_path);
            return 7;
        }
    }
    if (save_path != NULL)
    {
        const int rc = run_save(scenario, scenario_text, save_path);
        free(scenario_text);
        return rc;
    }
    if (score_secs > 0.0)
    {
        const int rc = run_score(scenario, score_secs, scenario_text);
        free(scenario_text);
        return rc;
    }

    if (pwr_abi_version() != (uint32_t)PWR_ABI_VERSION)
    {
        fatal("PWRadarCore ABI mismatch: this console was built against "
              "version %d, the loaded PWRadarCore.dll reports %u.\n\n"
              "The two came from different builds.  Rebuild both, and check "
              "that the PWRadarCore.dll being loaded is the one beside this "
              "executable and not an older copy earlier on PATH.",
              PWR_ABI_VERSION, pwr_abi_version());
        return 3;
    }

    install_interrupt_handlers();

    err[0] = '\0';
    if (app_create(&app, err, sizeof(err)) == 0)
    {
        fatal("%s", (err[0] != '\0') ? err : "initialisation failed");
        return 4;
    }
    if (scenario >= 0 && pwr_scenario_name((uint32_t)scenario) != NULL)
    {
        (void)pwr_engine_load_scenario(app.eng, (uint32_t)scenario);
        app.scenario = scenario;
    }
    if (scenario_text != NULL)
    {
        const PWR_Status lst = pwr_engine_scenario_load(app.eng, scenario_text,
                                                        err, sizeof(err));
        free(scenario_text);
        scenario_text = NULL;
        if (lst != PWR_STATUS_OK)
        {
            fprintf(stderr, "%s: %s\n", load_path, err);
            app_destroy(&app);
            return 7;
        }
        app.scenario = -1;
        (void)pwr_engine_get_config(app.eng, &app.cfg);
        (void)pwr_engine_get_metrics(app.eng, &app.dm);
        (void)pwr_engine_get_environment(app.eng, &app.env);
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
