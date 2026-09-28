#include "services/telemetry.h"

#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "common/log.h"
#include "drivers/camera_driver.h"
#include "drivers/frame_pool.h"
#include "hal/hal_sensor.h"
#include "pipeline/motion_task.h"

#define TELEMETRY_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )

/* Provided by main.c. */
void ev_heap_lock( void );
uint32_t ev_late_allocs( void );

static uint32_t s_run_seconds;

static uint32_t prvKb( size_t bytes )
{
    return ( uint32_t ) ( ( bytes + 1023 ) / 1024 );
}

static void prvTelemetryTask( void * param )
{
    TickType_t wake = xTaskGetTickCount();
    hal_sensor_stats_t hw_prev = { 0 };
    camera_stats_t cam_prev = { 0 };
    motion_stats_t mot_prev = { 0 };
    uint32_t seconds = 0;

    ( void ) param;

    /* This is the lowest-priority task, so by the time it runs the scheduler
     * has finished creating its own tasks. From here on, any allocation is a bug. */
    ev_heap_lock();

    LOG( "[boot] heap locked: %u KB of %u KB used by start-up allocations",
         prvKb( configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize() ), prvKb( configTOTAL_HEAP_SIZE ) );
    LOG( "[boot] pool letters: F=free W=DMA writing R=ready in queue P=processing" );

    for( ; ; )
    {
        hal_sensor_stats_t hw;
        camera_stats_t cam;
        motion_stats_t mot;
        char pool[ FRAME_POOL_SLOTS + 1 ];

        vTaskDelayUntil( &wake, pdMS_TO_TICKS( 1000 ) );
        seconds++;

        hal_sensor_get_stats( &hw );
        camera_get_stats( &cam );
        motion_get_stats( &mot );
        frame_pool_snapshot( pool );

        LOG( "[%3lus] %-9s | sensor %2lu/s overrun %2lu | captured %2lu/s | consumed %2lu/s (mean %3lu, lat %2lu ms) | nobuf %lu qdrop %lu dmato %lu | pool [%s] | heap %lu/%lu KB peak %lu | late allocs %lu",
             ( unsigned long ) seconds,
             camera_is_streaming() ? "STREAMING" : "idle",
             ( unsigned long ) ( hw.produced - hw_prev.produced ),
             ( unsigned long ) ( hw.overruns - hw_prev.overruns ),
             ( unsigned long ) ( cam.frames_captured - cam_prev.frames_captured ),
             ( unsigned long ) ( mot.consumed - mot_prev.consumed ),
             ( unsigned long ) mot.last_mean,
             ( unsigned long ) mot.last_latency_ms,
             ( unsigned long ) cam.no_buffer,
             ( unsigned long ) cam.queue_drops,
             ( unsigned long ) cam.dma_timeouts,
             pool,
             ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize() ),
             ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE ),
             ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE - xPortGetMinimumEverFreeHeapSize() ),
             ( unsigned long ) ev_late_allocs() );

        hw_prev = hw;
        cam_prev = cam;
        mot_prev = mot;

        if( ( s_run_seconds > 0 ) && ( seconds >= s_run_seconds ) )
        {
            LOG( "[done] %lu s: motion irqs %lu, wakeups %lu, sensor frames %lu, DMA transfers %lu, overruns %lu, captured %lu, consumed %lu, late allocs %lu",
                 ( unsigned long ) seconds,
                 ( unsigned long ) cam.motion_irqs,
                 ( unsigned long ) cam.wakeups,
                 ( unsigned long ) hw.produced,
                 ( unsigned long ) hw.transferred,
                 ( unsigned long ) hw.overruns,
                 ( unsigned long ) cam.frames_captured,
                 ( unsigned long ) mot.consumed,
                 ( unsigned long ) ev_late_allocs() );
            exit( 0 );
        }
    }
}

int telemetry_init( uint32_t run_seconds )
{
    s_run_seconds = run_seconds;
    return ( xTaskCreate( prvTelemetryTask, "Telemetry", TELEMETRY_STACK_WORDS, NULL, PRIO_TELEMETRY, NULL ) == pdPASS ) ? 0 : -1;
}
