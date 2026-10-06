#include "services/watchdog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "common/log.h"

#define WATCHDOG_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )

typedef struct
{
    const char * name;
    uint32_t timeout_ms;
    void ( * restart )( void );
    volatile TickType_t last_kick;
    int registered;
} watched_t;

static watched_t s_watched[ WD_COUNT ];
static watchdog_stats_t s_stats;

static void prvWatchdogTask( void * param )
{
    TickType_t wake = xTaskGetTickCount();

    ( void ) param;

    for( ; ; )
    {
        vTaskDelayUntil( &wake, pdMS_TO_TICKS( WATCHDOG_PERIOD_MS ) );

        for( int i = 0; i < WD_COUNT; i++ )
        {
            watched_t * w = &s_watched[ i ];
            const TickType_t silent = xTaskGetTickCount() - w->last_kick;

            if( !w->registered || ( silent < pdMS_TO_TICKS( w->timeout_ms ) ) )
            {
                continue;
            }

            const uint32_t silent_ms = ( uint32_t ) ( silent * portTICK_PERIOD_MS );

            if( w->restart == NULL )
            {
                LOG( "[watchdog] %s silent for %lu ms -> device reset", w->name, ( unsigned long ) silent_ms );
                exit( 3 );
            }

            LOG( "[watchdog] %s silent for %lu ms -> restarting it", w->name, ( unsigned long ) silent_ms );
            w->restart();
            w->last_kick = xTaskGetTickCount();

            taskENTER_CRITICAL();
            s_stats.restarts++;
            s_stats.last_silent_ms = silent_ms;
            snprintf( s_stats.last_task, sizeof( s_stats.last_task ), "%s", w->name );
            taskEXIT_CRITICAL();
        }
    }
}

int watchdog_init( void )
{
    return ( xTaskCreate( prvWatchdogTask, "Watchdog", WATCHDOG_STACK_WORDS, NULL, PRIO_WATCHDOG, NULL ) == pdPASS ) ? 0 : -1;
}

void watchdog_register( watchdog_id_t id, const char * name, uint32_t timeout_ms, void ( * restart )( void ) )
{
    s_watched[ id ].name = name;
    s_watched[ id ].timeout_ms = timeout_ms;
    s_watched[ id ].restart = restart;
    s_watched[ id ].last_kick = xTaskGetTickCount();
    s_watched[ id ].registered = 1;
}

void watchdog_kick( watchdog_id_t id )
{
    s_watched[ id ].last_kick = xTaskGetTickCount();
}

void watchdog_get_stats( watchdog_stats_t * out )
{
    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}
