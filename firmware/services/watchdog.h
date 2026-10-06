#ifndef WATCHDOG_H
#define WATCHDOG_H

/* Software watchdog.
 *
 * Each pipeline task registers with a timeout and must call watchdog_kick()
 * at least that often; every blocking wait in those tasks is bounded so an
 * idle task still kicks. WatchdogTask (highest priority) checks every
 * WATCHDOG_PERIOD_MS. A task that stays silent past its timeout is:
 *   - restarted, if it registered a restart function (InferenceTask), or
 *   - treated as fatal: the device resets, as a hardware watchdog would. */

#include <stdint.h>

#define WATCHDOG_PERIOD_MS    100

typedef enum
{
    WD_CAMERA = 0,
    WD_MOTION,
    WD_INFERENCE,
    WD_DECISION,
    WD_NETWORK,
    WD_COUNT
} watchdog_id_t;

typedef struct
{
    uint32_t restarts;      /* Task restarts performed, all tasks. */
    uint32_t last_silent_ms; /* How long the last restarted task had been silent. */
    char last_task[ 16 ];   /* Name of the last task restarted. */
} watchdog_stats_t;

int watchdog_init( void );

/* restart may be NULL: then a silent task resets the device. */
void watchdog_register( watchdog_id_t id, const char * name, uint32_t timeout_ms, void ( * restart )( void ) );

void watchdog_kick( watchdog_id_t id );

void watchdog_get_stats( watchdog_stats_t * out );

#endif /* WATCHDOG_H */
