#include "services/governor.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

static int s_enabled;
static int32_t s_tokens_us = ( int32_t ) GOV_BURST_US;
static uint32_t s_est_us = 100000u; /* Start pessimistic: a full frame. */
static TickType_t s_last_refill;
static uint32_t s_admitted;
static uint32_t s_dropped;
static char s_history[ GOV_HISTORY_LEN + 1 ];
static volatile uint32_t s_load_percent = 100;

static void prvPushHistory( char outcome )
{
    memmove( s_history, s_history + 1, GOV_HISTORY_LEN - 1 );
    s_history[ GOV_HISTORY_LEN - 1 ] = outcome;
}

/* Call inside a critical section. */
static void prvRefill( void )
{
    const TickType_t now = xTaskGetTickCount();
    const int64_t earned = ( int64_t ) ( now - s_last_refill ) * portTICK_PERIOD_MS * ( GOV_BUDGET_US_PER_S / 1000u );
    int64_t tokens = ( int64_t ) s_tokens_us + earned;

    s_last_refill = now;
    s_tokens_us = ( int32_t ) ( ( tokens > ( int64_t ) GOV_BURST_US ) ? GOV_BURST_US : tokens );
}

void governor_init( int enabled )
{
    s_enabled = enabled;
    memset( s_history, '.', GOV_HISTORY_LEN );
    s_history[ GOV_HISTORY_LEN ] = '\0';
}

int governor_admit( int force, uint32_t * reserved_us )
{
    int admit;

    taskENTER_CRITICAL();
    prvRefill();
    admit = force || !s_enabled || ( s_tokens_us > 0 );

    if( admit )
    {
        s_tokens_us -= ( int32_t ) s_est_us;
        *reserved_us = s_est_us;
        s_admitted++;
    }
    else
    {
        s_dropped++;
        prvPushHistory( GOV_DROPPED );
    }

    taskEXIT_CRITICAL();
    return admit;
}

void governor_complete( uint32_t reserved_us, uint32_t actual_us, char outcome )
{
    actual_us = ( uint32_t ) ( ( uint64_t ) actual_us * s_load_percent / 100u );

    taskENTER_CRITICAL();

    /* Refund the reservation, charge what the frame really cost. */
    s_tokens_us += ( int32_t ) reserved_us - ( int32_t ) actual_us;

    /* Moving average of frame cost, weight 1/4 on the newest frame. */
    s_est_us = ( 3u * s_est_us + actual_us ) / 4u;
    prvPushHistory( outcome );
    taskEXIT_CRITICAL();
}

void governor_set_load_percent( uint32_t percent )
{
    s_load_percent = percent;
}

void governor_get_stats( governor_stats_t * out )
{
    taskENTER_CRITICAL();
    prvRefill();
    out->admitted = s_admitted;
    out->dropped = s_dropped;
    out->tokens_us = s_tokens_us;
    out->est_cost_us = s_est_us;
    memcpy( out->history, s_history, sizeof( s_history ) );
    taskEXIT_CRITICAL();
}
