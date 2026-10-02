#ifndef GOVERNOR_H
#define GOVERNOR_H

/* Frame-skip governor: keeps the pipeline inside the device's compute budget.
 *
 * The simulated device has GOV_BUDGET_US_PER_S of compute per second. The
 * governor keeps a token bucket of that budget. Each frame it admits
 * reserves the expected cost of a frame (a moving average of real costs);
 * when the frame finishes, its actual cost is settled against the
 * reservation. When the bucket is empty, new frames are dropped on purpose,
 * evenly spaced, instead of piling up and being lost at random.
 *
 * Costs are device-equivalent: CPU work is measured on the host and
 * multiplied by the CPU scale; NPU work is the NPU's emulated latency. */

#include <stdint.h>

#define GOV_BUDGET_US_PER_S    1000000u /* One device-second of compute per second. */
#define GOV_BURST_US           100000u  /* Unused budget kept for bursts: 100 ms.  */
#define GOV_FRAME_BASE_US      15000u   /* Firmware cost of taking any frame in.   */
#define GOV_HISTORY_LEN        15

/* Frame outcomes, as shown in the history string. */
#define GOV_FULL       'P' /* Went through inference.            */
#define GOV_GATED      'M' /* Stopped at the motion gate (cheap). */
#define GOV_DROPPED    'D' /* Dropped (governor or queue full).   */

typedef struct
{
    uint32_t admitted;
    uint32_t dropped;      /* By the governor.                     */
    int32_t tokens_us;     /* Current budget balance (can go negative). */
    uint32_t est_cost_us;  /* Expected cost of the next frame.     */
    char history[ GOV_HISTORY_LEN + 1 ]; /* Latest outcomes, oldest first. */
} governor_stats_t;

void governor_init( int enabled );

/* Decide whether to process a new frame. force admits regardless (e.g. the
 * first frame after wake-up). On admit, *reserved_us receives the amount
 * reserved, to be passed back to governor_complete(). */
int governor_admit( int force, uint32_t * reserved_us );

/* Settle a finished frame: its actual device cost and outcome (GOV_*). */
void governor_complete( uint32_t reserved_us, uint32_t actual_us, char outcome );

void governor_get_stats( governor_stats_t * out );

#endif /* GOVERNOR_H */
