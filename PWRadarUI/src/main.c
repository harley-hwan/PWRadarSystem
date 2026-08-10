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
#include "ui_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        while (n < capture_frames && app_step(&app) != 0) { ++n; }
        ok = app_write_ppm(&app, capture_path);
        printf("captured %d frames -> %s (%s)\n", n, capture_path,
               (ok != 0) ? "ok" : "FAILED");
        app_destroy(&app);
        return (ok != 0) ? 0 : 5;
    }

    while (app_step(&app) != 0) { }

    app_destroy(&app);
    return 0;
}
