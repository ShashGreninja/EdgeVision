#include "pipeline/motion_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "drivers/frame_pool.h"
#include "hal/hal_sensor.h"

#define MOTION_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )

static QueueHandle_t s_queue;
static uint32_t s_cost_ms;
static volatile motion_stats_t s_stats;

static void prvMotionTask( void * param )
{
    ( void ) param;

    for( ; ; )
    {
        int idx;
        uint32_t sum = 0;

        xQueueReceive( s_queue, &idx, portMAX_DELAY );
        frame_pool_move( idx, BUF_READY, BUF_PROCESSING );

        frame_buf_t * fb = frame_pool_get( idx );

        s_stats.last_latency_ms = ( uint32_t ) ( ( xTaskGetTickCount() - fb->captured_at ) * portTICK_PERIOD_MS );

        for( uint32_t i = 0; i < FRAME_BYTES; i++ )
        {
            sum += fb->data[ i ];
        }

        s_stats.last_mean = sum / FRAME_BYTES;

        /* Stand-in for the Day 2 motion + inference cost. */
        if( s_cost_ms > 0 )
        {
            vTaskDelay( pdMS_TO_TICKS( s_cost_ms ) );
        }

        s_stats.consumed++;
        frame_pool_move( idx, BUF_PROCESSING, BUF_FREE );
    }
}

int motion_task_init( QueueHandle_t frame_queue, uint32_t cost_ms )
{
    s_queue = frame_queue;
    s_cost_ms = cost_ms;
    return ( xTaskCreate( prvMotionTask, "Motion", MOTION_STACK_WORDS, NULL, PRIO_MOTION, NULL ) == pdPASS ) ? 0 : -1;
}

void motion_get_stats( motion_stats_t * out )
{
    *out = *( const motion_stats_t * ) &s_stats;
}
