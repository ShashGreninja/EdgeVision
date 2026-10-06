#ifndef PIPELINE_TYPES_H
#define PIPELINE_TYPES_H

/* Messages passed between pipeline tasks through queues. */

#include <stdint.h>

#include "FreeRTOS.h"

#include "hal/hal_npu.h"

/* What the camera cares about. COCO classes are mapped onto these. */
typedef enum
{
    CAT_PERSON = 0,
    CAT_VEHICLE,
    CAT_ANIMAL,
    CAT_COUNT
} category_t;

/* MotionTask -> InferenceTask: "look at this region of this frame". */
typedef struct
{
    uint32_t seq;          /* Sensor frame sequence number (the NPU fetches it by this). */
    hal_roi_t roi;         /* Where the motion was, in frame coordinates. */
    uint8_t wake;          /* Whole-frame check (wake-up or periodic), not a motion region. */
    uint32_t reserved_us;  /* Governor reservation for this frame. */
    uint32_t cost_us;      /* Device cost spent on the frame so far. */
} infer_job_t;

/* InferenceTask -> DecisionTask: best detection per category in one frame. */
typedef struct
{
    uint32_t seq;
    TickType_t at;
    float score[ CAT_COUNT ]; /* 0 if the category was not seen. */
    hal_roi_t box[ CAT_COUNT ];
} detections_t;

/* DecisionTask -> NetworkTask: one confirmed event, to be sent to the cloud. */
typedef struct
{
    char name[ 24 ];      /* e.g. "person_detected". */
    float confidence;
    hal_roi_t box;
    uint32_t seq;         /* Sensor frame sequence number. */
    uint32_t uptime_ms;
} event_t;

static inline const char * category_name( int cat )
{
    static const char * const names[ CAT_COUNT ] = { "person", "vehicle", "animal" };

    return ( ( cat >= 0 ) && ( cat < CAT_COUNT ) ) ? names[ cat ] : "unknown";
}

/* Map a 0-based COCO class id to a category, or -1 for classes we ignore. */
static inline int category_from_coco( int coco )
{
    switch( coco )
    {
        case 0:
            return CAT_PERSON;

        case 1: /* bicycle */
        case 2: /* car */
        case 3: /* motorcycle */
        case 5: /* bus */
        case 7: /* truck */
            return CAT_VEHICLE;

        default:
            /* bird, cat, dog, horse, sheep, cow, elephant, bear, zebra, giraffe */
            return ( ( coco >= 14 ) && ( coco <= 23 ) ) ? CAT_ANIMAL : -1;
    }
}

#endif /* PIPELINE_TYPES_H */
