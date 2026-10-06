#include "services/telemetry.h"

#include <stdio.h>
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
#include "services/faults.h"
#include "services/governor.h"
#include "services/network.h"

#define TELEMETRY_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )

/* Bandwidth baseline: what a camera that streams video to the cloud for
 * analysis would upload. 2 Mbit/s is a typical 1080p30 H.264 stream. */
#define VIDEO_BASELINE_BYTES_PER_S    ( 2000000u / 8u )

/* Provided by main.c. */
void ev_heap_lock( void );
uint32_t ev_late_allocs( void );

static uint32_t s_run_seconds;
static char s_json[ TELEMETRY_JSON_MAX ]; /* Built once a second; static, not on the stack. */

typedef struct
{
    hal_sensor_stats_t hw;
    camera_stats_t cam;
    governor_stats_t gov;
    motion_stats_t mot;
    inference_stats_t inf;
    decision_stats_t dec;
    network_stats_t net;
    faults_stats_t flt;
    char pool[ FRAME_POOL_SLOTS + 1 ];
} snapshot_t;

static uint32_t prvKb( size_t bytes )
{
    return ( uint32_t ) ( ( bytes + 1023 ) / 1024 );
}

static void prvTake( snapshot_t * s )
{
    hal_sensor_get_stats( &s->hw );
    camera_get_stats( &s->cam );
    governor_get_stats( &s->gov );
    motion_get_stats( &s->mot );
    inference_take_stats( &s->inf );
    decision_get_stats( &s->dec );
    network_get_stats( &s->net );
    faults_get_stats( &s->flt );
    frame_pool_snapshot( s->pool );
}

/* One JSON object with per-second rates (against prev) and running totals. */
static void prvBuildJson( const snapshot_t * s, const snapshot_t * prev, uint32_t seconds )
{
    const uint32_t awake_ms = camera_awake_ms();
    const double streamed = ( double ) awake_ms / 1000.0 * VIDEO_BASELINE_BYTES_PER_S;
    const double always_on = ( double ) seconds * VIDEO_BASELINE_BYTES_PER_S;
    const double up = ( double ) s->net.bytes_up;

    snprintf( s_json, sizeof( s_json ),
              "{\"t\":%lu,\"device\":\"%s\",\"state\":\"%s\","
              "\"camera\":{\"fps\":%lu,\"overruns\":%lu,\"captured\":%lu,\"corrupt\":%lu,\"sensor_lost\":%lu,\"sensor_resets\":%lu,\"wakeups\":%lu,\"awake_ms\":%lu},"
              "\"governor\":{\"admitted\":%lu,\"dropped\":%lu,\"admitted_total\":%lu,\"dropped_total\":%lu,\"history\":\"%s\",\"est_ms\":%lu},"
              "\"motion\":{\"gated\":%lu,\"forwarded\":%lu,\"gated_total\":%lu,\"forwarded_total\":%lu,\"rechecks\":%lu,\"busy\":%lu},"
              "\"npu\":{\"jobs\":%lu,\"jobs_total\":%lu,\"device_ms\":%lu,\"host_ms\":%lu,\"rejected\":%lu,\"restarts\":%lu},"
              "\"best\":{\"person\":%.2f,\"vehicle\":%.2f,\"animal\":%.2f},"
              "\"events\":{\"total\":%lu,\"person\":%lu,\"vehicle\":%lu,\"animal\":%lu},"
              "\"network\":{\"cable\":%u,\"online\":%u,\"queued\":%lu,\"published\":%lu,\"dropped\":%lu,\"retries\":%lu,\"bytes_up\":%lu},"
              "\"bandwidth\":{\"uploaded_bytes\":%lu,\"streaming_baseline_bytes\":%.0f,\"always_on_baseline_bytes\":%.0f,"
              "\"saved_vs_streaming_pct\":%.3f,\"saved_vs_always_on_pct\":%.4f},"
              "\"pool\":\"%s\",\"heap\":{\"used_kb\":%lu,\"peak_kb\":%lu,\"total_kb\":%lu,\"late_allocs\":%lu},"
              "\"faults\":{\"mem_pressure\":%u,\"cpu_overload\":%u}}",
              ( unsigned long ) seconds, network_device_id(), camera_is_streaming() ? "streaming" : "idle",
              ( unsigned long ) ( s->cam.frames_captured - prev->cam.frames_captured ),
              ( unsigned long ) s->hw.overruns, ( unsigned long ) s->cam.frames_captured,
              ( unsigned long ) s->cam.corrupt_frames, ( unsigned long ) s->cam.sensor_lost,
              ( unsigned long ) s->cam.sensor_resets, ( unsigned long ) s->cam.wakeups, ( unsigned long ) awake_ms,
              ( unsigned long ) ( s->gov.admitted - prev->gov.admitted ), ( unsigned long ) ( s->gov.dropped - prev->gov.dropped ),
              ( unsigned long ) s->gov.admitted, ( unsigned long ) s->gov.dropped, s->gov.history,
              ( unsigned long ) ( s->gov.est_cost_us / 1000 ),
              ( unsigned long ) ( s->mot.gated - prev->mot.gated ), ( unsigned long ) ( s->mot.forwarded - prev->mot.forwarded ),
              ( unsigned long ) s->mot.gated, ( unsigned long ) s->mot.forwarded,
              ( unsigned long ) s->mot.rechecks, ( unsigned long ) s->mot.infer_busy,
              ( unsigned long ) ( s->inf.jobs - prev->inf.jobs ), ( unsigned long ) s->inf.jobs,
              ( unsigned long ) ( s->inf.last_device_us / 1000 ), ( unsigned long ) ( s->inf.last_host_us / 1000 ),
              ( unsigned long ) s->inf.rejected, ( unsigned long ) s->inf.restarts,
              ( double ) s->inf.peak[ CAT_PERSON ], ( double ) s->inf.peak[ CAT_VEHICLE ], ( double ) s->inf.peak[ CAT_ANIMAL ],
              ( unsigned long ) s->dec.events, ( unsigned long ) s->dec.events_by[ CAT_PERSON ],
              ( unsigned long ) s->dec.events_by[ CAT_VEHICLE ], ( unsigned long ) s->dec.events_by[ CAT_ANIMAL ],
              s->net.cable, s->net.online, ( unsigned long ) s->net.queued, ( unsigned long ) s->net.published,
              ( unsigned long ) s->net.dropped, ( unsigned long ) s->net.retries, ( unsigned long ) s->net.bytes_up,
              ( unsigned long ) s->net.bytes_up, streamed, always_on,
              ( streamed > 0.0 ) ? 100.0 * ( 1.0 - up / streamed ) : 100.0,
              ( always_on > 0.0 ) ? 100.0 * ( 1.0 - up / always_on ) : 100.0,
              s->pool,
              ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize() ),
              ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE - xPortGetMinimumEverFreeHeapSize() ),
              ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE ), ( unsigned long ) ev_late_allocs(),
              s->flt.mem_pressure_active, s->flt.cpu_overload_active );
}

static void prvTelemetryTask( void * param )
{
    TickType_t wake = xTaskGetTickCount();
    static snapshot_t now;
    static snapshot_t prev;
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
        vTaskDelayUntil( &wake, pdMS_TO_TICKS( 1000 ) );
        seconds++;
        prvTake( &now );

        LOG( "[%3lus] %-9s | cam %2lu/s ovr %lu | gov %2lu/%2lu [%s] est %3lu ms | gate %2lu/s | npu %2lu/s %3lu ms | best P%.2f V%.2f A%.2f | events %lu | net %s q%lu pub %lu | pool [%s] | heap %lu/%lu KB late %lu",
             ( unsigned long ) seconds,
             camera_is_streaming() ? "STREAMING" : "idle",
             ( unsigned long ) ( now.cam.frames_captured - prev.cam.frames_captured ),
             ( unsigned long ) ( now.hw.overruns - prev.hw.overruns ),
             ( unsigned long ) ( now.gov.admitted - prev.gov.admitted ),
             ( unsigned long ) ( now.gov.dropped - prev.gov.dropped ),
             now.gov.history,
             ( unsigned long ) ( now.gov.est_cost_us / 1000 ),
             ( unsigned long ) ( now.mot.gated - prev.mot.gated ),
             ( unsigned long ) ( now.inf.jobs - prev.inf.jobs ),
             ( unsigned long ) ( now.inf.last_device_us / 1000 ),
             ( double ) now.inf.peak[ CAT_PERSON ],
             ( double ) now.inf.peak[ CAT_VEHICLE ],
             ( double ) now.inf.peak[ CAT_ANIMAL ],
             ( unsigned long ) now.dec.events,
             !now.net.cable ? "nomod" : ( now.net.online ? "up" : "DOWN" ),
             ( unsigned long ) now.net.queued,
             ( unsigned long ) now.net.published,
             now.pool,
             ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize() ),
             ( unsigned long ) prvKb( configTOTAL_HEAP_SIZE ),
             ( unsigned long ) ev_late_allocs() );

        prvBuildJson( &now, &prev, seconds );
        network_send_telemetry( s_json );
        prev = now;

        if( ( s_run_seconds > 0 ) && ( seconds >= s_run_seconds ) )
        {
            LOG( "[done] %s", s_json );
            exit( 0 );
        }
    }
}

int telemetry_init( uint32_t run_seconds )
{
    s_run_seconds = run_seconds;
    return ( xTaskCreate( prvTelemetryTask, "Telemetry", TELEMETRY_STACK_WORDS, NULL, PRIO_TELEMETRY, NULL ) == pdPASS ) ? 0 : -1;
}
