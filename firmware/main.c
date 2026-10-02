/* EdgeVision firmware entry point (FreeRTOS Windows simulator).
 *
 * Usage: edgevision [clip.mp4] [options]
 *   clip.mp4          video to feed the virtual sensor (default: synthetic scene)
 *   --run-seconds N   exit with a summary after N seconds (default: run forever)
 *   --auto-motion S   raise the MOTION interrupt every S seconds (default: off)
 *   --udp-port P      UDP port for MOTION datagrams from the gateway (default: 5055, 0 = off)
 *   --model M         fp32 | int8 | path to an .onnx file (default: fp32)
 *   --npu-ms N        emulated NPU latency per job (default: 75 for fp32, 30 for int8)
 *   --motion-thresh T per-cell brightness change that counts as motion (default: 20)
 *   --cpu-scale K     device CPU is K times slower than this PC (default: 20)
 *   --no-governor     admit every frame (to compare against the governor)
 *
 * Keys (in a console): m = motion, q = quit. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "common/log.h"
#include "drivers/camera_driver.h"
#include "drivers/frame_pool.h"
#include "hal/hal_input.h"
#include "hal/hal_npu.h"
#include "hal/hal_sensor.h"
#include "pipeline/decision.h"
#include "pipeline/inference.h"
#include "pipeline/motion.h"
#include "pipeline/pipeline_types.h"
#include "services/governor.h"
#include "services/telemetry.h"

#define MODEL_FP32    "object_detection_nanodet_2022nov.onnx"
#define MODEL_INT8    "object_detection_nanodet_2022nov_int8.onnx"

#define INFER_QUEUE_LEN       2
#define DECISION_QUEUE_LEN    4

static volatile int s_heap_locked;
static volatile uint32_t s_late_allocs;

static void prvFatal( const char * what )
{
    LOG( "[fatal] %s", what );
    exit( 1 );
}

/* The simulator's 1 ms tick relies on timeBeginPeriod(1). Windows 11 ignores
 * that request for processes it considers in the background (no visible
 * window), which makes every tick ~16 ms. Opt out of that throttling. */
static void prvHostKeepTimerResolution( void )
{
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    #define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION    0x4
#endif
    PROCESS_POWER_THROTTLING_STATE state;

    memset( &state, 0, sizeof( state ) );
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0; /* 0 = do not throttle, honour the timer resolution. */
    SetProcessInformation( GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof( state ) );
}

/* Find a model file: as given, or under models/ relative to the current
 * directory or its parents (so it works from the repo root or firmware/). */
static const char * prvFindModel( const char * name, char * buf, size_t len )
{
    static const char * const prefixes[] = { "", "models/", "../models/", "../../models/" };

    for( size_t i = 0; i < sizeof( prefixes ) / sizeof( prefixes[ 0 ] ); i++ )
    {
        snprintf( buf, len, "%s%s", prefixes[ i ], name );

        if( GetFileAttributesA( buf ) != INVALID_FILE_ATTRIBUTES )
        {
            return buf;
        }
    }

    return NULL;
}

int main( int argc, char ** argv )
{
    const char * clip = NULL;
    const char * model = "fp32";
    uint32_t run_seconds = 0;
    uint32_t npu_ms = 0;
    uint32_t motion_thresh = 20;
    uint32_t cpu_scale = 20;
    int governor = 1;
    hal_input_cfg_t input = { .keyboard = 1, .udp_port = 5055, .auto_motion_s = 0 };
    QueueHandle_t frame_queue;
    QueueHandle_t infer_queue;
    QueueHandle_t decision_queue;
    char model_path[ MAX_PATH ];

    for( int i = 1; i < argc; i++ )
    {
        const int has_value = ( i + 1 < argc );

        if( has_value && ( strcmp( argv[ i ], "--run-seconds" ) == 0 ) )
        {
            run_seconds = ( uint32_t ) atoi( argv[ ++i ] );
        }
        else if( has_value && ( strcmp( argv[ i ], "--auto-motion" ) == 0 ) )
        {
            input.auto_motion_s = atoi( argv[ ++i ] );
        }
        else if( has_value && ( strcmp( argv[ i ], "--udp-port" ) == 0 ) )
        {
            input.udp_port = atoi( argv[ ++i ] );
        }
        else if( has_value && ( strcmp( argv[ i ], "--model" ) == 0 ) )
        {
            model = argv[ ++i ];
        }
        else if( has_value && ( strcmp( argv[ i ], "--npu-ms" ) == 0 ) )
        {
            npu_ms = ( uint32_t ) atoi( argv[ ++i ] );
        }
        else if( has_value && ( strcmp( argv[ i ], "--motion-thresh" ) == 0 ) )
        {
            motion_thresh = ( uint32_t ) atoi( argv[ ++i ] );
        }
        else if( has_value && ( strcmp( argv[ i ], "--cpu-scale" ) == 0 ) )
        {
            cpu_scale = ( uint32_t ) atoi( argv[ ++i ] );
        }
        else if( strcmp( argv[ i ], "--no-governor" ) == 0 )
        {
            governor = 0;
        }
        else
        {
            clip = argv[ i ];
        }
    }

    /* Model: the INT8 variant runs faster on a real NPU (not on a desktop CPU). */
    if( npu_ms == 0 )
    {
        npu_ms = ( strcmp( model, "int8" ) == 0 ) ? 30 : 75;
    }

    if( strcmp( model, "fp32" ) == 0 )
    {
        model = MODEL_FP32;
    }
    else if( strcmp( model, "int8" ) == 0 )
    {
        model = MODEL_INT8;
    }

    prvHostKeepTimerResolution();

    LOG( "EdgeVision firmware | RAM budget %u KB | sensor %dx%d@%d -> ISP %dx%d gray (%d bytes/frame) | %d frame slots",
         ( unsigned ) ( configTOTAL_HEAP_SIZE / 1024 ), SENSOR_NATIVE_W, SENSOR_NATIVE_H, SENSOR_FPS,
         FRAME_W, FRAME_H, FRAME_BYTES, FRAME_POOL_SLOTS );
    LOG( "[boot] governor %s (budget %u ms/s) | motion threshold %lu | cpu scale x%lu | NPU latency %lu ms",
         governor ? "on" : "OFF", GOV_BUDGET_US_PER_S / 1000u, ( unsigned long ) motion_thresh,
         ( unsigned long ) cpu_scale, ( unsigned long ) npu_ms );

    if( prvFindModel( model, model_path, sizeof( model_path ) ) == NULL )
    {
        LOG( "[fatal] model '%s' not found (looked in ., models/, ../models/)", model );
        return 1;
    }

    if( frame_pool_init() != 0 )
    {
        prvFatal( "frame pool does not fit in RAM" );
    }

    governor_init( governor );

    /* Pipeline wiring: Camera -> frame queue -> Motion -> infer queue ->
     * Inference -> decision queue -> Decision. */
    frame_queue = camera_driver_init();
    infer_queue = xQueueCreate( INFER_QUEUE_LEN, sizeof( infer_job_t ) );
    decision_queue = xQueueCreate( DECISION_QUEUE_LEN, sizeof( detections_t ) );

    if( ( frame_queue == NULL ) || ( infer_queue == NULL ) || ( decision_queue == NULL ) ||
        ( motion_init( frame_queue, infer_queue, motion_thresh, cpu_scale ) != 0 ) ||
        ( inference_init( infer_queue, decision_queue ) != 0 ) ||
        ( decision_init( decision_queue ) != 0 ) ||
        ( telemetry_init( run_seconds ) != 0 ) )
    {
        prvFatal( "could not create tasks" );
    }

    vQueueAddToRegistry( infer_queue, "InferQ" );
    vQueueAddToRegistry( decision_queue, "DecisionQ" );

    if( hal_npu_init( model_path, npu_ms ) != 0 )
    {
        prvFatal( "NPU could not load the model" );
    }

    hal_sensor_init( clip );
    hal_input_init( &input );

    LOG( "[boot] press 'm' for motion, 'q' to quit (or send UDP \"MOTION\" to 127.0.0.1:%d)", input.udp_port );
    vTaskStartScheduler();

    prvFatal( "scheduler returned" );
    return 1;
}

/*---------------------- Heap accounting (see FreeRTOSConfig.h) -----------------------*/

void ev_trace_malloc( size_t xSize )
{
    ( void ) xSize;

    if( s_heap_locked )
    {
        s_late_allocs++;
    }
}

void ev_heap_lock( void )
{
    s_heap_locked = 1;
}

uint32_t ev_late_allocs( void )
{
    return s_late_allocs;
}

/*------------------------------ FreeRTOS hooks --------------------------------------*/

void vApplicationMallocFailedHook( void )
{
    printf( "[fatal] out of RAM: the %u KB heap is exhausted\n", ( unsigned ) ( configTOTAL_HEAP_SIZE / 1024 ) );
    fflush( stdout );
    exit( 1 );
}

void vAssertCalled( const char * pcFile, unsigned long ulLine )
{
    printf( "[fatal] assert failed at %s:%lu\n", pcFile, ulLine );
    fflush( stdout );
    exit( 2 );
}
