// TOTK (2026-10-20): the thread census, tested without the game.
//
// The census is only worth anything if it finds a thread BY NAME. This probe makes three threads with known
// names, two of them burning CPU and one idle, and checks:
//   A. the walk sees all three, with the name, the class (guest / port / other) and the CPU;
//   B. the delta walk attributes the CPU of the burning thread to it, and ranks it above the idle one;
//   C. the per-frame hook writes a row naming the burning thread (the end-to-end path).
#include "totk_census.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int g_stop;
static void *burner(void *arg) {
    const char *nm = arg;
    pthread_setname_np(nm);
    while (!g_stop) { volatile double x = 0; for (int i = 0; i < 20000; i++) x += i * 0.5; (void)x; }
    return NULL;
}
static void *sleeper(void *arg) {
    const char *nm = arg;
    pthread_setname_np(nm);
    while (!g_stop) usleep(20000);
    return NULL;
}
static int fails;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}
static const totk_census_thread *find(totk_census_thread *t, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(t[i].name, name)) return &t[i];
    return NULL;
}
int main(void) {
    const char *path = "/tmp/totk-census-probe.tsv";
    unlink(path);
    setenv("DM_NVN_CENSUS", path, 1);      // the census reads it on the first frame
    setenv("DM_NVN_CENSUS_MIN_MS", "20", 1);
    setenv("DM_NVN_CENSUS_TOP", "3", 1);

    pthread_t th_guest, th_port, th_idle;
    pthread_create(&th_guest, NULL, burner, (void *)"totk-guest-0x1004");
    pthread_create(&th_port, NULL, burner, (void *)"totk-probe-hot");
    pthread_create(&th_idle, NULL, sleeper, (void *)"totk-probe-idle");
    usleep(300000);   // let them burn and let their CPU be accounted

    // A. the walk
    totk_census_thread t[TOTK_CENSUS_MAX];
    int n = totk_census_walk(t, TOTK_CENSUS_MAX);
    printf("walk: %d threads\n", n);
    const totk_census_thread *g = find(t, n, "totk-guest-0x1004");
    const totk_census_thread *p = find(t, n, "totk-probe-hot");
    const totk_census_thread *i = find(t, n, "totk-probe-idle");
    check(g != NULL, "the walk finds the guest thread by name");
    check(p != NULL, "the walk finds the port thread by name");
    check(i != NULL, "the walk finds the idle thread by name");
    check(g && g->cls == TOTK_CENSUS_CLASS_GUEST, "totk-guest-* is classified GUEST");
    check(p && p->cls == TOTK_CENSUS_CLASS_PORT, "totk-* is classified PORT");
    check(i && i->cls == TOTK_CENSUS_CLASS_PORT, "the idle port thread is classified PORT");
    check(p && p->user_ns + p->sys_ns > 0, "the burning thread has CPU");
    check(g && g->user_ns + g->sys_ns > 0, "the burning guest thread has CPU");
    check(p && p->tid != 0, "the thread has an id");

    // B. the delta walk: the first call is the baseline, the second one carries the burn
    totk_census_walk_delta(t, TOTK_CENSUS_MAX);
    usleep(300000);
    n = totk_census_walk_delta(t, TOTK_CENSUS_MAX);
    const totk_census_thread *dp = find(t, n, "totk-probe-hot");
    const totk_census_thread *di = find(t, n, "totk-probe-idle");
    check(dp && dp->user_ns > 100000000ull, "the delta walk attributes CPU to the burning thread");
    check(di && di->user_ns < dp->user_ns, "the burning thread ranks above the idle one");
    check(n >= 1 && t[0].user_ns >= (n > 1 ? t[1].user_ns : 0), "the delta walk is sorted, largest first");

    // C. the per-frame hook, end to end: the row must name the burning thread
    for (int f = 1; f <= 5; f++) { totk_census_frame((uint64_t)f); usleep(80000); }
    for (int tries = 0; tries < 40; tries++) {
        FILE *fp = fopen(path, "r");
        if (fp) {
            char line[4096]; int found = 0, rows = 0, guest_ms = -1, port_ms = -1, n_guest = -1, n_port = -1;
            while (fgets(line, sizeof line, fp)) {
                if (line[0] == '#') continue;
                rows++;
                if (strstr(line, "totk-probe-hot")) found = 1;
                // the class columns: frame unaccounted guest port other n_guest n_port n_other ...
                char *p = line; int col = 0; double v[8] = {0,0,0,0,0,0,0,0}; int ints[8] = {0,0,0,0,0,0,0,0};
                char *tok = strtok(p, "\t");
                while (tok && col < 8) {
                    if (col >= 2 && col <= 4) v[col] = atof(tok);
                    if (col >= 5 && col <= 7) ints[col] = atoi(tok);
                    tok = strtok(NULL, "\t"); col++;
                }
                if (v[2] > 0) guest_ms = (int)(v[2] + 0.5);
                if (v[3] > 0) port_ms = (int)(v[3] + 0.5);
                if (ints[5] > 0) n_guest = ints[5];
                if (ints[6] > 0) n_port = ints[6];
            }
            fclose(fp);
            if (rows) {
                check(found, "the census row names the burning thread");
                check(1, "the census wrote a row");
                // the class totals must cover EVERY thread of the class, not only the top N
                check(n_guest >= 1, "the row counts the guest threads");
                check(n_port >= 2, "the row counts the port threads (the burner and the idle one)");
                check(guest_ms > 0, "the guest class total is not zero");
                check(port_ms > 0, "the port class total is not zero");
                check(port_ms > guest_ms / 2, "the port class total is of the same order as the guest one");
                goto done;
            }
        }
        usleep(50000);
    }
    check(0, "the census wrote a row");
done:
    g_stop = 1;
    pthread_join(th_guest, NULL); pthread_join(th_port, NULL); pthread_join(th_idle, NULL);
    printf(fails ? "FAIL: %d check(s)\n" : "PASS: all checks\n", fails);
    return fails ? 1 : 0;
}
