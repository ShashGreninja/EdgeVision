#ifndef DECISION_H
#define DECISION_H

/* DecisionTask: turns per-frame detections into events.
 *
 * Two thresholds (hysteresis): being forgiving only costs a little power,
 * while a false event wakes up a human.
 * - Tracking: any result scoring at least DECISION_TRACK_SCORE keeps the
 *   camera streaming and points the next re-check at that spot.
 * - Event: a category is confirmed when at least DECISION_NEED of the last
 *   DECISION_WINDOW results saw it at DECISION_MIN_SCORE or more, all within
 *   DECISION_MAX_AGE_MS. A confirmed category raises an event at most once
 *   per DECISION_COOLDOWN_MS. */

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "pipeline/pipeline_types.h"

#define DECISION_WINDOW        5
#define DECISION_NEED          3
#define DECISION_MIN_SCORE     0.50f
#define DECISION_TRACK_SCORE   0.35f
#define DECISION_MAX_AGE_MS    3000
#define DECISION_COOLDOWN_MS   10000
#define DECISION_ZONE          "front_door"

typedef struct
{
    uint32_t results;             /* Inference results examined. */
    uint32_t events;              /* Events raised, all categories. */
    uint32_t events_by[ CAT_COUNT ];
    uint8_t confirmed[ CAT_COUNT ]; /* Currently confirmed in view. */
} decision_stats_t;

int decision_init( QueueHandle_t decision_queue );

void decision_get_stats( decision_stats_t * out );

#endif /* DECISION_H */
