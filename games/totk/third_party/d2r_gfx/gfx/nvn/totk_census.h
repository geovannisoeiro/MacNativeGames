// TOTK (2026-10-20): the thread census — "where did the CPU of this frame go?"
//
// The question it answers: a frame burned a lot of process CPU, but the thread that called present (@cpu_ms)
// only accounted for part of it. The rest is in some other thread. Is it a guest thread (the game working —
// the cost of exploring, nothing to fix in the port) or one of the port's own threads (a bug to fix)?
//
// The instrument: once per frame the caller passes the frame number. The census keeps its own baseline of the
// process CPU and of the calling thread's CPU, so it can tell how much of the process CPU the present thread
// did NOT account for. When that unaccounted part crosses the threshold (DM_NVN_CENSUS_MIN_MS, default 40 ms)
// the census asks its own thread to walk the thread table once, and writes one row per triggered frame: the
// three threads that burned the most CPU since the previous walk, with their name, their id and their QoS,
// plus the totals split by class (guest / port / other).
//
// It never runs on the port's path: the walk happens on the census's own thread, outside every port lock, and
// only on the frames that trigger. The per-frame cost on the present path is one getrusage and one clock read.
//
// Environment:
//   DM_NVN_CENSUS=<path>   enable it; <path> is the TSV it writes ("1" -> totk-census.tsv in the cwd)
//   DM_NVN_CENSUS_MIN_MS  unaccounted-process-CPU threshold that triggers a walk (default 40)
//   DM_NVN_CENSUS_TOP     how many threads per row to report (default 3)
#pragma once
#include <stdint.h>

enum { TOTK_CENSUS_MAX = 512 };   // threads we can hold in one walk (the port has ~150 live)
enum { TOTK_CENSUS_NAME_MAX = 64 };

// One thread, as the census sees it.
typedef struct {
    uint64_t tid;                       // system-wide unique thread id (THREAD_IDENTIFIER_INFO)
    uint64_t user_ns, sys_ns;           // cumulative CPU (THREAD_EXTENDED_INFO)
    int32_t  cpu_pct;                   // the kernel's own scaled CPU usage
    int32_t  qos;                       // QoS class, -1 = unknown (not a pthread)
    int32_t  cls;                       // TOTK_CENSUS_CLASS_*
    char     name[TOTK_CENSUS_NAME_MAX];
} totk_census_thread;

enum { TOTK_CENSUS_CLASS_OTHER = 0, TOTK_CENSUS_CLASS_GUEST = 1, TOTK_CENSUS_CLASS_PORT = 2 };

// Called once per frame, from the present path. Cheap when the census is off (one branch).
void totk_census_frame(uint64_t frame);

// The walk itself, exposed so it can be tested without the game: fills `out` with up to `max` threads and
// their current cumulative CPU. Returns how many were found.
int totk_census_walk(totk_census_thread *out, int max);

// The same walk, but each entry carries the CPU consumed since the previous call of this function (the
// first call after start-up yields zero deltas for threads that already existed). `out` is sorted by that
// delta, largest first. Returns how many entries were filled.
int totk_census_walk_delta(totk_census_thread *out, int max);
