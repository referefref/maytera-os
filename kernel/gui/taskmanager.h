// taskmanager.h - Task Manager / Process Explorer for MayteraOS (#487/#349).
// Windows-11-style, kernel-compositor built-in app. Tabs: Processes,
// Performance, Details, Services. Theme-aware, non-blocking draw (#426).
#ifndef TASKMANAGER_H
#define TASKMANAGER_H

#include "window.h"
#include "../types.h"
#include "../proc/process.h"   // proc_info_t
#include "../net/tcp.h"        // tcp_conn_info_t
#include "../proc/cron.h"      // #487/#265: cron_job_t (Scheduled Tasks tab)

#define TM_MAX_PROCS   64      // == MAX_PROCESSES
#define TM_HIST        64      // performance history ring depth
#define TM_MAX_CONNS   32
#define TM_MAX_MENU    8
#define TM_MAX_CORES   16      // per-core graphs shown (Win11 "Logical processors")
#define TM_MAX_CRON    32      // == CRON_MAX_JOBS (Scheduled Tasks tab)

// Tabs
typedef enum {
    TM_TAB_PROCESSES = 0,
    TM_TAB_PERFORMANCE,
    TM_TAB_DETAILS,
    TM_TAB_SERVICES,
    TM_TAB_SCHEDULED,     // #487/#265: surface the cron-like scheduler
    TM_TAB_COUNT
} tm_tab_t;

// #487: Performance sub-view. Win11 splits "overall" from "logical processors";
// the same toggle drives which chart grid the Performance tab draws.
typedef enum {
    TM_PERF_OVERALL = 0,  // CPU / Memory / Disk / Network
    TM_PERF_CORES         // one chart per logical core
} tm_perf_view_t;

// Sort key (mirrors the Rust/C data-core enum in taskmanager.c)
typedef enum {
    TM_SORT_CPU = 0,     // CPU desc
    TM_SORT_MEM = 1,     // Mem desc
    TM_SORT_PID = 2,     // PID asc
    TM_SORT_CPU_ASC = 3,
    TM_SORT_MEM_ASC = 4
} tm_sort_t;

// Display-ready per-process row (derived + cached each refresh).
typedef struct {
    uint32_t pid, ppid;
    char     name[32];
    uint32_t state;        // process_state_t
    uint32_t mem_kb;
    uint64_t cpu_ticks;    // cumulative
    uint32_t cpu_pct;      // derived share-of-busy this interval (0..100)
    uint32_t threads;
    uint32_t fds;          // open VFS fd count
    int32_t  running_cpu;
    uint8_t  priority;     // process_priority_t
    uint32_t uid;
    uint8_t  privilege;    // 0 kernel / 3 user
    int8_t   depth;        // tree indent depth
    uint8_t  has_kids;
} tm_proc_t;

typedef struct {
    window_t *window;
    tm_tab_t  tab;

    // ---- Processes cache ----
    tm_proc_t procs[TM_MAX_PROCS];   // display order (tree/sorted)
    int       nproc;
    int       sort_key;              // tm_sort_t
    int       selected_row;          // index into procs[]
    uint32_t  selected_pid;          // sticky selection across refreshes
    int       scroll;
    int       tree_mode;             // 1 = tree grouping, 0 = flat sorted
    uint32_t  collapsed[TM_MAX_PROCS]; // pids whose children are hidden
    int       ncollapsed;

    // ---- CPU% delta bookkeeping ----
    uint32_t  prev_pid[TM_MAX_PROCS];
    uint64_t  prev_ticks[TM_MAX_PROCS];
    int       prev_n;

    // ---- Performance history rings ----
    uint32_t  cpu_hist[TM_HIST];
    uint32_t  mem_hist[TM_HIST];   // percent
    uint32_t  disk_hist[TM_HIST];  // percent used
    uint32_t  net_hist[TM_HIST];   // KB/s
    uint32_t  hist_head;           // index of oldest sample
    uint32_t  hist_count;
    uint64_t  prev_net_bytes;
    uint64_t  mem_total_kb, mem_used_kb;
    int64_t   disk_total, disk_free;

    // #487: per-logical-core history (Win11 per-core graphs). Shares hist_head/
    // hist_count with the rings above: every ring is pushed once per refresh at
    // the same slot, so one head/count describes them all.
    uint32_t  core_hist[TM_MAX_CORES][TM_HIST];
    int       ncores;              // logical cores reported by smp_get_core_count()
    tm_perf_view_t perf_view;      // Overall vs Logical processors

    // ---- Details cache ----
    tcp_conn_info_t conns[TM_MAX_CONNS];
    int       nconns;

    // ---- #487/#265 Scheduled Tasks cache (cron_list snapshot) ----
    cron_job_t jobs[TM_MAX_CRON];
    int       njobs;
    int       sched_sel;           // selected row on the Scheduled tab

    // ---- Context menu ----
    uint8_t   menu_open;
    int32_t   menu_x, menu_y;
    uint32_t  menu_pid;
    int       menu_hover;

    uint64_t  last_update;
} taskmanager_t;

taskmanager_t *taskmanager_create(void);
void taskmanager_destroy(taskmanager_t *tm);
void taskmanager_handle_event(taskmanager_t *tm, gui_event_t *event);
void taskmanager_draw(taskmanager_t *tm);
void taskmanager_launch(void);

// #404 boot-time [RUST-DIFF] self-test for the taskmgr_core data seam.
// Wired from main.c by the serial integrator (see integration snippets).
void taskmgr_core_selftest(void);

#endif // TASKMANAGER_H
