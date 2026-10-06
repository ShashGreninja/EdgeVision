#ifndef INFERENCE_H
#define INFERENCE_H

/* InferenceTask: hands each motion region to the NPU and waits for its
 * completion interrupt, then reduces the NPU's detections to the best
 * score per category and passes that to DecisionTask. */

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "pipeline/pipeline_types.h"

typedef struct
{
    uint32_t jobs;          /* Jobs completed by the NPU.                    */
    uint32_t rejected;      /* NPU refused a job because it was still busy.  */
    uint32_t restarts;      /* Watchdog restarts of InferenceTask.           */
    uint32_t frame_gone;    /* Frame overwritten before the NPU got to it.   */
    uint32_t last_device_us; /* Emulated NPU time of the last job.           */
    uint32_t last_host_us;  /* Host time of the last job.                    */
    float peak[ CAT_COUNT ]; /* Best score per category since the last read. */
} inference_stats_t;

int inference_init( QueueHandle_t infer_queue, QueueHandle_t decision_queue );

/* Copy the stats and reset the per-category peaks. */
void inference_take_stats( inference_stats_t * out );

#endif /* INFERENCE_H */
