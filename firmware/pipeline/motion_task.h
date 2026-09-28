#ifndef MOTION_TASK_H
#define MOTION_TASK_H

/* MotionTask: first consumer of the frame queue.
 *
 * Day 1 stub: takes each READY frame, measures its mean brightness, holds it
 * for a simulated processing cost, then frees the slot. Day 2 replaces the
 * body with the real motion gate + ROI extraction. */

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

typedef struct
{
    uint32_t consumed;       /* Frames taken from the queue.              */
    uint32_t last_mean;      /* Mean brightness of the last frame (0-255). */
    uint32_t last_latency_ms; /* DMA-complete -> picked up, last frame.    */
} motion_stats_t;

int motion_task_init( QueueHandle_t frame_queue, uint32_t cost_ms );

void motion_get_stats( motion_stats_t * out );

#endif /* MOTION_TASK_H */
