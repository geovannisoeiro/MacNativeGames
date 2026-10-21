// TOTK (2026-10-20): the thread census — "where did the CPU of this frame go?"
//
// The question: a frame burns a lot of process CPU, but the thread that called present (@cpu_ms) only
// accounted for part of it. The rest is in some other thread. Is it a guest thread (the game working — the
// cost of exploring, nothing to fix in the port) or one of the port's own threads (a bug to fix)?
//
// The instrument: once per frame the caller passes the frame number. The census keeps its own baseline of
// the process CPU and of the calling thread's CPU, so it can tell how much of the process CPU the present
// thread did NOT account for. When that unaccounted part crosses the threshold (DM_NVN_CENSUS_MIN_MS,
// default 40 ms) the census asks its own thread to walk the thread table once, and writes one row per
// triggered frame: the threads that burned the most CPU since the previous walk, with their name, their id
// and their QoS, plus the totals split by class (guest / port / other).
//
// It never runs on the port's path: the walk happens on the census's own thread, outside every port lock,
// and only on the frames that trigger. The per-frame cost on the present path is one getrusage and one clock
// read.
//
// The classes come from the thread NAME, which is the only thing that distinguishes a guest thread from a
// port thread without a registry of live threads:
//   "totk-guest-<handle>"  a guest thread (runtime/threads.c names them, DM_NVN_THREADNAMES)
//   "totk-*" / "nvn-*" / "TOTK *"  a thread of the port itself
//   anything else           Metal, dispatch, the run loop, unnamed threads
#include "totk_census.h"
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int census_class(const char *name) {
    if (!strncmp(name, "totk-guest-", 11)) return TOTK_CENSUS_CLASS_GUEST;   // the guest's own threads
    // TOTK (2026-10-21): "gcd-pool" is the name the warm-up's threads go back to when it ends (the GDC reuses
    // them for whatever comes next). Without it here, the port's post-warm-up work moved from the "port" bucket
    // to "other" — the same hiding the fix was meant to end, one bucket over. The census cannot split a shared
    // thread, so it attributes the pool to the port, which is the side that dispatches on it.
    if (!strncmp(name, "totk-", 5) || !strncmp(name, "nvn-", 4) || !strncmp(name, "TOTK ", 5) || !strncmp(name, "gcd-pool", 8)) return TOTK_CENSUS_CLASS_PORT;
    return TOTK_CENSUS_CLASS_OTHER;
}
static const char *class_name(int cls) {
    return cls == TOTK_CENSUS_CLASS_GUEST ? "guest" : cls == TOTK_CENSUS_CLASS_PORT ? "port" : "other";
}

// The walk: every thread of this process, with its cumulative CPU (user + system) and its name.
int totk_census_walk(totk_census_thread *out, int max) {
    if (!out || max <= 0) return 0;
    thread_act_array_t th = NULL; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &th, &nth) != KERN_SUCCESS || !th) return 0;
    int n = 0;
    for (mach_msg_type_number_t i = 0; i < nth && n < max; i++) {
        thread_identifier_info_data_t idi; mach_msg_type_number_t c1 = THREAD_IDENTIFIER_INFO_COUNT;
        thread_basic_info_data_t bi; mach_msg_type_number_t c2 = THREAD_BASIC_INFO_COUNT;
        if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&idi, &c1) == KERN_SUCCESS &&
            thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c2) == KERN_SUCCESS) {
            totk_census_thread *t = &out[n];
            memset(t, 0, sizeof *t);
            t->tid = idi.thread_id;
            t->user_ns = (uint64_t)bi.user_time.seconds * 1000000000ull + (uint64_t)bi.user_time.microseconds * 1000ull;
            t->sys_ns = (uint64_t)bi.system_time.seconds * 1000000000ull + (uint64_t)bi.system_time.microseconds * 1000ull;
            t->cpu_pct = bi.cpu_usage;
            t->qos = -1;
            t->cls = TOTK_CENSUS_CLASS_OTHER;
            // The name: pthread_getname_np is safe on any thread of this process (it reads the pthread's own
            // name slot). A thread that is not a pthread (a kernel thread, a thread that died) has no name.
            pthread_t pt = pthread_from_mach_thread_np(th[i]);
            if (pt) {
                char nm[TOTK_CENSUS_NAME_MAX];
                nm[0] = 0;
                if (pthread_getname_np(pt, nm, sizeof nm) == 0 && nm[0]) {
                    strncpy(t->name, nm, TOTK_CENSUS_NAME_MAX - 1);
                    t->name[TOTK_CENSUS_NAME_MAX - 1] = 0;
                    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
                    if (pthread_get_qos_class_np(pt, &qc, &rp) == 0) t->qos = (int32_t)qc;
                }
            }
            t->cls = census_class(t->name);
            n++;
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof *th);
    return n;
}

// The delta walk: the same, but each entry carries the CPU consumed since the previous call of this
// function. The first call after start-up yields zero deltas for threads that already existed.
typedef struct { uint64_t tid, ns; } census_prev;
static census_prev g_prev[TOTK_CENSUS_MAX]; static int g_nprev;
static census_prev g_next[TOTK_CENSUS_MAX]; static int g_nnext;

int totk_census_walk_delta(totk_census_thread *out, int max) {
    if (!out || max <= 0) return 0;
    totk_census_thread buf[TOTK_CENSUS_MAX];
    int n = totk_census_walk(buf, TOTK_CENSUS_MAX);
    // the new baseline (absolute totals) is saved before the deltas are taken
    g_nnext = 0;
    for (int i = 0; i < n && g_nnext < TOTK_CENSUS_MAX; i++) { g_next[g_nnext].tid = buf[i].tid; g_next[g_nnext].ns = buf[i].user_ns + buf[i].sys_ns; g_nnext++; }
    for (int i = 0; i < n; i++) {
        uint64_t tot = buf[i].user_ns + buf[i].sys_ns, prev = 0; int found = 0;
        for (int k = 0; k < g_nprev; k++) if (g_prev[k].tid == buf[i].tid) { prev = g_prev[k].ns; found = 1; break; }
        uint64_t d = (found && tot >= prev) ? tot - prev : 0;
        buf[i].user_ns = d; buf[i].sys_ns = 0;   // the delta rides in user_ns; the row prints one number
    }
    memcpy(g_prev, g_next, sizeof(census_prev) * (size_t)g_nnext);
    g_nprev = g_nnext;
    // sort by the delta, largest first
    for (int i = 1; i < n; i++) {
        totk_census_thread v = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j].user_ns < v.user_ns) { buf[j + 1] = buf[j]; j--; }
        buf[j + 1] = v;
    }
    if (n > max) n = max;
    memcpy(out, buf, sizeof(totk_census_thread) * (size_t)n);
    return n;
}

// ---------- the per-frame hook ----------

static FILE *g_census_file; static int g_census_checked, g_census_hdr;
static int g_census_min_ms = 40, g_census_top = 3;
static pthread_t g_census_thread; static int g_census_thread_set;
static pthread_mutex_t g_census_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_census_cv = PTHREAD_COND_INITIALIZER;
static int g_census_want; static uint64_t g_census_frame;
static double g_census_unacc_ms;

static int census_env_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DM_NVN_CENSUS");
        on = e && *e;
        if (on) {
            const char *m = getenv("DM_NVN_CENSUS_MIN_MS"); if (m && atoi(m) > 0) g_census_min_ms = atoi(m);
            const char *t = getenv("DM_NVN_CENSUS_TOP"); if (t && atoi(t) > 0) g_census_top = atoi(t);
            if (g_census_top > 16) g_census_top = 16;
        }
    }
    return on;
}
static FILE *census_file(void) {
    if (g_census_checked) return g_census_file;
    g_census_checked = 1;
    const char *p = getenv("DM_NVN_CENSUS");
    if (!p || !*p) return NULL;
    const char *path = (!strcmp(p, "1")) ? "totk-census.tsv" : p;
    g_census_file = fopen(path, "a");
    return g_census_file;
}
static uint64_t proc_cpu_ns(void) {
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ull +
           (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ull;
}
static uint64_t thread_cpu_ns(void) { return (uint64_t)clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); }

// The census's own thread: the walk and the write happen here, never on the port's path.
static void *census_worker(void *arg) {
    (void)arg;
    pthread_setname_np("totk-census");
    for (;;) {
        pthread_mutex_lock(&g_census_mu);
        while (!g_census_want) pthread_cond_wait(&g_census_cv, &g_census_mu);
        g_census_want = 0;
        uint64_t fr = g_census_frame; double un = g_census_unacc_ms;
        pthread_mutex_unlock(&g_census_mu);

        enum { TOP_MAX = 16 };   // the number of threads a row can carry (DM_NVN_CENSUS_TOP)
        totk_census_thread th[TOTK_CENSUS_MAX];
        // The walk returns EVERY thread with its delta since the previous walk; the class totals are summed
        // over all of them and only the top N are written as rows. (Summing over the top N alone would
        // under-report the classes: the port has ~150 live threads and the default row carries 3.)
        int n = totk_census_walk_delta(th, TOTK_CENSUS_MAX);
        double cls_ms[3] = {0, 0, 0}; int cls_n[3] = {0, 0, 0};
        for (int i = 0; i < n; i++) { int c = th[i].cls; if (c < 0 || c > 2) c = 0; cls_ms[c] += th[i].user_ns / 1e6; cls_n[c]++; }
        FILE *f = census_file();
        if (f) {
            if (!g_census_hdr) {
                fprintf(f, "# frame\tunaccounted_ms\tguest_ms\tport_ms\tother_ms\tn_guest\tn_port\tn_other");
                for (int i = 0; i < g_census_top; i++) fprintf(f, "\tt%d_name\tt%d_tid\tt%d_ms\tt%d_class\tt%d_qos", i, i, i, i, i);
                fprintf(f, "\n");
                fflush(f); g_census_hdr = 1;
            }
            fprintf(f, "%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t%d\t%d", (unsigned long long)fr, un, cls_ms[1], cls_ms[2], cls_ms[0], cls_n[1], cls_n[2], cls_n[0]);
            for (int i = 0; i < g_census_top; i++) {
                if (i < n) fprintf(f, "\t%s\t%llu\t%.3f\t%s\t%d", th[i].name, (unsigned long long)th[i].tid, th[i].user_ns / 1e6, class_name(th[i].cls), th[i].qos);
                else fprintf(f, "\t-\t0\t0.000\t-\t-1");
            }
            fprintf(f, "\n");
            fflush(f);
        }
    }
    return NULL;
}

// Called once per frame, from the present path. Cheap when the census is off (one branch).
void totk_census_frame(uint64_t frame) {
    if (!census_env_on()) return;
    static uint64_t pproc, pthr; static int first = 1;
    uint64_t pc = proc_cpu_ns(), tc = thread_cpu_ns();
    if (first) { first = 0; pproc = pc; pthr = tc; return; }
    double dproc = (double)(pc - pproc) / 1e6, dthr = (double)(tc - pthr) / 1e6;
    pproc = pc; pthr = tc;
    double un = dproc - dthr;                       // the process CPU the present thread did not account for
    if (un < (double)g_census_min_ms) return;
    if (!g_census_thread_set) {
        if (pthread_create(&g_census_thread, NULL, census_worker, NULL) != 0) { g_census_thread_set = -1; return; }
        g_census_thread_set = 1;
    }
    if (g_census_thread_set != 1) return;
    pthread_mutex_lock(&g_census_mu);
    g_census_want = 1; g_census_frame = frame; g_census_unacc_ms = un;
    pthread_cond_signal(&g_census_cv);
    pthread_mutex_unlock(&g_census_mu);
}
