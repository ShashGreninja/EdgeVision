#ifndef MOTION_H
#define MOTION_H

/* MotionTask: the cheap gate in front of the expensive detector.
 *
 * Shrinks each frame to an 80x60 grid of 4x4-pixel cell averages and
 * compares it with the previous frame's grid. A cell counts as changed if
 * its brightness moved by more than `threshold`; isolated changed cells
 * (no changed neighbour) are treated as noise. If enough cells changed, the
 * bounding box of the changed cells becomes the region of interest and the
 * frame goes to inference. Otherwise it is dropped here ("gated"). */

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "hal/hal_npu.h"

#define MOTION_GRID_W      80
#define MOTION_GRID_H      60
#define MOTION_CELL        4   /* FRAME_W / MOTION_GRID_W */
#define MOTION_MIN_CELLS   8   /* Changed cells needed to call it motion. */

/* While streaming, send a re-check to inference at least this often even
 * without motion, so a person standing still is not missed. The re-check
 * looks where something was last seen (if within MOTION_FOCUS_MS), else at
 * the whole frame. */
#define MOTION_RECHECK_MS  500
#define MOTION_FOCUS_MS    3000

typedef struct
{
    uint32_t frames;      /* Frames examined.                          */
    uint32_t gated;       /* No motion: stopped here.                  */
    uint32_t forwarded;   /* Sent to inference.                        */
    uint32_t rechecks;    /* Of those, periodic whole-frame checks.    */
    uint32_t infer_busy;  /* Motion found but the inference queue was full. */
    uint32_t last_cells;  /* Changed cells in the last frame.          */
    uint32_t last_cost_us; /* Device-equivalent cost of the last frame. */
} motion_stats_t;

/* frame_queue: slot indices from CameraTask. infer_queue: infer_job_t out.
 * threshold: per-cell brightness change (0-255). cpu_scale: how many times
 * slower the device CPU is than the host. */
int motion_init( QueueHandle_t frame_queue, QueueHandle_t infer_queue, uint32_t threshold, uint32_t cpu_scale );

void motion_get_stats( motion_stats_t * out );

/* Tell the motion stage where something was just seen (frame coordinates). */
void motion_set_focus( const hal_roi_t * box );

#endif /* MOTION_H */
