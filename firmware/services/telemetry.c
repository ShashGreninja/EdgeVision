#include "services/telemetry.h"

#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "common/log.h"
#include "drivers/camera_driver.h"
#include "drivers/frame_pool.h"
#include "hal/hal_sensor.h"
#include "pipeline/decision.h"
#include "pipeline/inference.h"
#include "pipeline/motion.h"
#include "services/governor.h"
#include "services/network.h"

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
    governor_stats_t gov_prev = { 0 };
    motion_stats_t mot_prev = { 0 };
    inference_stats_t inf_prev = { 0 };
    uint32_t seconds = 0;

    ( void ) param;

    /* This is the lowest-priority task, so by the time it runs the scheduler
     * has finished creating its own tasks. From here on, any allocation is a bug. */
    ev_heap_lock();

    LOG( "[boot] heap locked: %u KB of %u KB used by start-up allocations",
         prvKb( configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize() ), prvKb( configTOTAL_HEAP_SIZE ) );
    LOG( "[boot] columns: cam=frames captured/s | gov=admitted/dropped per s, last %d outcomes (P=full M=motion-gated D=dropped) | "
         "gate=frames stopped at motion gate/s | npu=inferences/s, device ms | best=top person/vehicle/animal score this second",
         GOV_HISTORY_LEN );

    for( ; ; )
    {
        hal_sensor_stats_t hw;
        camera_stats_t cam;
        governor_stats_t gov;
        motion_stats_t mot;
        inference_stats_t inf;
        decision_stats_t dec;
        network_stats_t net;
        char pool[ FRAME_POOL_SLOTS + 1 ];

        vTaskDelayUntil( &wake, pdMS_TO_TICKS( 1000 ) );
        seconds++;

        hal_sensor_get_stats( &hw );
        camera_get_stats( &cam );
        governor_get_stats( &gov );
        motion_get_stats( &mot );
        inference_take_stats( &inf );
        decision_get_stats( &dec );
        frame_pool_snapshot( pool );

        network_get_stats( &net );

        LOG( "[%3lus] %-9s | cam %2lu/s ovr %lu | gov %2lu/%2lu [%s] est %3lu ms | gate %2lu/s | npu %2lu/s %3lu ms | best P%.2f V%.2f A%.2f | events %lu | net %s q%lu pub %lu | pool [%s] | heap %lu/%lu KB late %lu",
             ( unsigned long ) seconds,
             camera_is_streaming() ? "STREAMING" : "idle",
             ( unsigned long ) ( cam.frames_captured - cam_prev.frames_captured ),
             ( unsigned long ) ( hw.overruns - hw_prev.overruns ),
             ( unsigned long ) ( gov.admitted - gov_prev.admitted ),
             ( unsigned long ) ( gov.dropped - gov_prev.dropped ),
             gov.history,
             ( unsigned long ) ( gov.est_cost_us / 1000 ),
             ( unsigned long ) ( mot.gated - mot_prev.gated ),
             ( unsigned long ) ( inf.jobs - inf_prev.jobs ),
             ( unsigned long ) ( inf.last_device_us / 1000 ),
             ( double ) inf.peak[ CAT_PERSON ],
             ( double ) inf.peak[ CAT_VEHICLE ],
             ( double ) inf.peak[ CAT_ANIMAL ],
             ( unsigned long ) dec.events,
             !net.cable ? "nomod" : ( net.online ? "up" : "DOWN" ),
             ( unsigned long ) net.queued,
             ( unsigned long ) net.published,
             pool,
             ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize() ),
             ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE ),
             ( unsigned long ) ev_late_allocs() );

        hw_prev = hw;
        cam_prev = cam;
        gov_prev = gov;
        mot_prev = mot;
        inf_prev = inf;

        if( ( s_run_seconds > 0 ) && ( seconds >= s_run_seconds ) )
        {
            LOG( "[done] %lu s: wakeups %lu | sensor %lu frames, overruns %lu | captured %lu, governor admitted %lu dropped %lu | "
                 "motion gated %lu forwarded %lu busy %lu | npu jobs %lu timeouts %lu gone %lu (host %lu ms/job) | "
                 "events %lu (person %lu, vehicle %lu, animal %lu) | heap peak %lu KB, late allocs %lu",
                 ( unsigned long ) seconds,
                 ( unsigned long ) cam.wakeups,
                 ( unsigned long ) hw.produced,
                 ( unsigned long ) hw.overruns,
                 ( unsigned long ) cam.frames_captured,
                 ( unsigned long ) gov.admitted,
                 ( unsigned long ) gov.dropped,
                 ( unsigned long ) mot.gated,
                 ( unsigned long ) mot.forwarded,
                 ( unsigned long ) mot.infer_busy,
                 ( unsigned long ) inf.jobs,
                 ( unsigned long ) inf.timeouts,
                 ( unsigned long ) inf.frame_gone,
                 ( unsigned long ) ( inf.last_host_us / 1000 ),
                 ( unsigned long ) dec.events,
                 ( unsigned long ) dec.events_by[ CAT_PERSON ],
                 ( unsigned long ) dec.events_by[ CAT_VEHICLE ],
                 ( unsigned long ) dec.events_by[ CAT_ANIMAL ],
                 ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE - xPortGetMinimumEverFreeHeapSize() ),
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
