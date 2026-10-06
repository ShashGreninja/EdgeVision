#include "pipeline/decision.h"

#include <stdio.h>
#include <string.h>

#include "task.h"

#include "common/log.h"
#include "drivers/camera_driver.h"
#include "pipeline/motion.h"
#include "services/network.h"
#include "services/watchdog.h"

#define DECISION_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )

static QueueHandle_t s_in;
static volatile decision_stats_t s_stats;

/* Recent results, newest at s_window[ ( s_next - 1 ) % DECISION_WINDOW ]. */
static detections_t s_window[ DECISION_WINDOW ];
static uint32_t s_next;
static TickType_t s_last_event[ CAT_COUNT ];
static uint8_t s_ever_fired[ CAT_COUNT ];

static void prvRaiseEvent( int cat, float confidence, const detections_t * latest, const hal_roi_t * box )
{
    event_t ev;

    s_stats.events++;
    s_stats.events_by[ cat ]++;

    memset( &ev, 0, sizeof( ev ) );
    snprintf( ev.name, sizeof( ev.name ), "%s_detected", category_name( cat ) );
    ev.confidence = confidence;
    ev.box = *box;
    ev.seq = latest->seq;
    ev.uptime_ms = ( uint32_t ) ( latest->at * portTICK_PERIOD_MS );

    LOG( "[event] {\"event\":\"%s\",\"confidence\":%.2f,\"zone\":\"%s\",\"seq\":%lu,\"box\":[%u,%u,%u,%u],\"uptime_ms\":%lu}",
         ev.name, ( double ) confidence, DECISION_ZONE, ( unsigned long ) ev.seq,
         box->x, box->y, box->w, box->h, ( unsigned long ) ev.uptime_ms );

    /* Only this small message ever leaves the device. */
    network_post_event( &ev );
}

static void prvEvaluate( const detections_t * latest )
{
    const TickType_t now = latest->at;

    for( int cat = 0; cat < CAT_COUNT; cat++ )
    {
        uint32_t hits = 0;
        float best = 0.0f;
        hal_roi_t best_box = { 0, 0, 0, 0 };

        for( uint32_t i = 0; ( i < DECISION_WINDOW ) && ( i < s_next ); i++ )
        {
            const detections_t * d = &s_window[ i ];

            if( ( ( now - d->at ) <= pdMS_TO_TICKS( DECISION_MAX_AGE_MS ) ) && ( d->score[ cat ] >= DECISION_MIN_SCORE ) )
            {
                hits++;

                if( d->score[ cat ] > best )
                {
                    best = d->score[ cat ];
                    best_box = d->box[ cat ];
                }
            }
        }

        /* Something real is in view right now: keep the camera streaming,
         * and have the next re-check look at the same spot. */
        if( latest->score[ cat ] >= DECISION_TRACK_SCORE )
        {
            camera_note_activity();
            motion_set_focus( &latest->box[ cat ] );
        }

        s_stats.confirmed[ cat ] = ( uint8_t ) ( hits >= DECISION_NEED );

        if( !s_stats.confirmed[ cat ] )
        {
            continue;
        }

        if( !s_ever_fired[ cat ] || ( ( now - s_last_event[ cat ] ) >= pdMS_TO_TICKS( DECISION_COOLDOWN_MS ) ) )
        {
            s_ever_fired[ cat ] = 1;
            s_last_event[ cat ] = now;
            prvRaiseEvent( cat, best, latest, &best_box );
        }
    }
}

static void prvDecisionTask( void * param )
{
    ( void ) param;

    for( ; ; )
    {
        detections_t d;

        watchdog_kick( WD_DECISION );

        if( xQueueReceive( s_in, &d, pdMS_TO_TICKS( 500 ) ) != pdPASS )
        {
            continue;
        }

        s_window[ s_next % DECISION_WINDOW ] = d;
        s_next++;
        s_stats.results++;
        prvEvaluate( &d );
    }
}

int decision_init( QueueHandle_t decision_queue )
{
    s_in = decision_queue;
    watchdog_register( WD_DECISION, "DecisionTask", 2000, NULL );
    return ( xTaskCreate( prvDecisionTask, "Decision", DECISION_STACK_WORDS, NULL, PRIO_DECISION, NULL ) == pdPASS ) ? 0 : -1;
}

void decision_get_stats( decision_stats_t * out )
{
    *out = *( const decision_stats_t * ) &s_stats;
}
