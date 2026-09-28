/* EdgeVision firmware entry point (FreeRTOS Windows simulator).
 *
 * Usage: edgevision [clip.mp4] [--run-seconds N] [--auto-motion S] [--cost-ms M] [--udp-port P]
 *   clip.mp4        video to feed the virtual sensor (default: synthetic scene)
 *   --run-seconds   exit with a summary after N seconds (default: run forever)
 *   --auto-motion   raise the MOTION interrupt every S seconds (default: off)
 *   --cost-ms       simulated per-frame processing cost in MotionTask (default: 20)
 *   --udp-port      UDP port for MOTION datagrams from the gateway (default: 5055, 0 = off)
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
#include "hal/hal_sensor.h"
#include "pipeline/motion_task.h"
#include "services/telemetry.h"

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

int main( int argc, char ** argv )
{
    const char * clip = NULL;
    uint32_t run_seconds = 0;
    uint32_t cost_ms = 20;
    hal_input_cfg_t input = { .keyboard = 1, .udp_port = 5055, .auto_motion_s = 0 };
    QueueHandle_t frame_queue;

    for( int i = 1; i < argc; i++ )
    {
        if( ( strcmp( argv[ i ], "--run-seconds" ) == 0 ) && ( i + 1 < argc ) )
        {
            run_seconds = ( uint32_t ) atoi( argv[ ++i ] );
        }
        else if( ( strcmp( argv[ i ], "--auto-motion" ) == 0 ) && ( i + 1 < argc ) )
        {
            input.auto_motion_s = atoi( argv[ ++i ] );
        }
        else if( ( strcmp( argv[ i ], "--cost-ms" ) == 0 ) && ( i + 1 < argc ) )
        {
            cost_ms = ( uint32_t ) atoi( argv[ ++i ] );
        }
        else if( ( strcmp( argv[ i ], "--udp-port" ) == 0 ) && ( i + 1 < argc ) )
        {
            input.udp_port = atoi( argv[ ++i ] );
        }
        else
        {
            clip = argv[ i ];
        }
    }

    prvHostKeepTimerResolution();

    LOG( "EdgeVision firmware | RAM budget %u KB | sensor %dx%d@%d -> ISP %dx%d gray (%d bytes/frame) | %d frame slots",
         ( unsigned ) ( configTOTAL_HEAP_SIZE / 1024 ), SENSOR_NATIVE_W, SENSOR_NATIVE_H, SENSOR_FPS,
         FRAME_W, FRAME_H, FRAME_BYTES, FRAME_POOL_SLOTS );

    if( frame_pool_init() != 0 )
    {
        prvFatal( "frame pool does not fit in RAM" );
    }

    frame_queue = camera_driver_init();

    if( ( frame_queue == NULL ) ||
        ( motion_task_init( frame_queue, cost_ms ) != 0 ) ||
        ( telemetry_init( run_seconds ) != 0 ) )
    {
        prvFatal( "could not create tasks" );
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
