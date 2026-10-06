#include "drivers/camera_driver.h"

#include "FreeRTOS.h"
#include "task.h"

#include "common/crc32.h"
#include "common/log.h"
#include "drivers/frame_pool.h"
#include "hal/hal_irq.h"
#include "hal/hal_sensor.h"
#include "services/governor.h"
#include "services/watchdog.h"

#define EV_MOTION           ( 1UL << 0 )
#define EV_DMA_DONE         ( 1UL << 1 )

#define FRAME_QUEUE_LEN     ( FRAME_POOL_SLOTS )
#define QUIET_TIMEOUT_MS    ( 5000 )
#define DMA_TIMEOUT_MS      ( 200 )
#define CLAIM_WAIT_MS       ( 100 )
#define CAMERA_STACK_WORDS  ( configMINIMAL_STACK_SIZE * 2 )

#define SENSOR_LOST_TIMEOUTS          3    /* DMA timeouts in a row = sensor lost. */
#define SENSOR_RESET_BACKOFF_MS       100
#define SENSOR_RESET_BACKOFF_MAX_MS   1600 /* Stays under the 2 s watchdog. */

static TaskHandle_t s_task;
static QueueHandle_t s_frame_queue;
static volatile camera_stats_t s_stats;
static volatile int s_streaming;
static int s_next_is_wake; /* The next captured frame is the first after wake-up. */
static uint32_t s_dma_timeout_streak;
static volatile TickType_t s_awake_ticks;  /* Completed streaming sessions. */
static volatile TickType_t s_session_start; /* Start of the current one.     */

/*-------------------------- Interrupt handlers ---------------------------*/

/* Handlers run in the port's simulated-interrupt context: *FromISR APIs only.
 * The return value tells the port whether a context switch is needed. */

static uint32_t prvMotionIsr( void )
{
    BaseType_t woken = pdFALSE;

    s_stats.motion_irqs++;
    xTaskNotifyFromISR( s_task, EV_MOTION, eSetBits, &woken );
    return ( uint32_t ) woken;
}

static uint32_t prvDmaDoneIsr( void )
{
    BaseType_t woken = pdFALSE;

    xTaskNotifyFromISR( s_task, EV_DMA_DONE, eSetBits, &woken );
    return ( uint32_t ) woken;
}

/*------------------------------ CameraTask -------------------------------*/

/* Wait up to DMA_TIMEOUT_MS for the DMA-complete event. Motion events that
 * arrive meanwhile refresh *last_motion. Returns 1 if the DMA completed. */
static int prvWaitForDma( TickType_t * last_motion )
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS( DMA_TIMEOUT_MS );

    for( ; ; )
    {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        uint32_t bits = 0;

        if( ( elapsed >= limit ) ||
            ( xTaskNotifyWait( 0, UINT32_MAX, &bits, limit - elapsed ) == pdFALSE ) )
        {
            return 0;
        }

        if( bits & EV_MOTION )
        {
            *last_motion = xTaskGetTickCount();
        }

        if( bits & EV_DMA_DONE )
        {
            return 1;
        }
    }
}

/* The sensor stopped delivering frames: power-cycle it until it answers,
 * backing off 100, 200, 400 ... ms (capped below the watchdog timeout). */
static void prvRecoverSensor( void )
{
    uint32_t backoff_ms = SENSOR_RESET_BACKOFF_MS;
    uint32_t attempts = 0;

    s_stats.sensor_lost++;
    LOG( "[camera] sensor not responding (%d DMA timeouts) -> resetting", SENSOR_LOST_TIMEOUTS );

    for( ; ; )
    {
        watchdog_kick( WD_CAMERA );
        hal_sensor_set_power( 0 );
        vTaskDelay( pdMS_TO_TICKS( backoff_ms ) );
        hal_sensor_set_power( 1 );
        attempts++;
        s_stats.sensor_resets++;

        if( hal_sensor_reset() == 0 )
        {
            break;
        }

        backoff_ms = ( backoff_ms * 2 > SENSOR_RESET_BACKOFF_MAX_MS ) ? SENSOR_RESET_BACKOFF_MAX_MS : backoff_ms * 2;
        LOG( "[camera] sensor reset attempt %lu failed, next in %lu ms", ( unsigned long ) attempts, ( unsigned long ) backoff_ms );
    }

    s_dma_timeout_streak = 0;
    LOG( "[camera] sensor recovered after %lu reset(s)", ( unsigned long ) attempts );
}

/* Capture one frame into a free slot and queue it. Returns when the frame
 * has been queued, dropped, or no slot was available. */
static void prvCaptureOne( TickType_t * last_motion )
{
    uint32_t bits = 0;
    const int idx = frame_pool_claim( pdMS_TO_TICKS( CLAIM_WAIT_MS ) );

    /* Pick up any motion events that arrived while we were waiting. */
    if( ( xTaskNotifyWait( 0, EV_MOTION, &bits, 0 ) == pdTRUE ) && ( bits & EV_MOTION ) )
    {
        *last_motion = xTaskGetTickCount();
    }

    if( idx < 0 )
    {
        /* Every slot stayed busy downstream. The sensor keeps running and the
         * hardware counts the frames it cannot deliver as overruns. */
        s_stats.no_buffer++;
        return;
    }

    frame_buf_t * fb = frame_pool_get( idx );

    hal_sensor_dma_arm( fb->data );

    if( !prvWaitForDma( last_motion ) )
    {
        s_stats.dma_timeouts++;

        /* If the transfer really never started, take the buffer back.
         * Otherwise it finished just now and its interrupt is on the way. */
        if( hal_sensor_dma_abort() || !prvWaitForDma( last_motion ) )
        {
            frame_pool_move( idx, BUF_FILLING, BUF_FREE );

            if( ++s_dma_timeout_streak >= SENSOR_LOST_TIMEOUTS )
            {
                prvRecoverSensor();
            }

            return;
        }
    }

    s_dma_timeout_streak = 0;

    /* Integrity: the ISP's CRC must match what landed in RAM. */
    if( crc32_compute( fb->data, FRAME_BYTES ) != hal_sensor_last_crc() )
    {
        s_stats.corrupt_frames++;
        frame_pool_move( idx, BUF_FILLING, BUF_FREE );
        return;
    }

    fb->seq = hal_sensor_last_seq();
    fb->captured_at = xTaskGetTickCount();
    fb->wake = ( uint8_t ) s_next_is_wake;
    frame_pool_move( idx, BUF_FILLING, BUF_READY );
    s_stats.frames_captured++;

    /* The first frame after wake-up is always checked; after that the
     * governor decides whether the budget allows this frame. */
    if( !governor_admit( fb->wake, &fb->gov_reserved_us ) )
    {
        frame_pool_move( idx, BUF_READY, BUF_FREE );
        return;
    }

    s_next_is_wake = 0;

    if( xQueueSend( s_frame_queue, &idx, 0 ) != pdPASS )
    {
        s_stats.queue_drops++;
        governor_complete( fb->gov_reserved_us, GOV_FRAME_BASE_US, GOV_DROPPED );
        frame_pool_move( idx, BUF_READY, BUF_FREE );
    }
}

static void prvCameraTask( void * param )
{
    ( void ) param;

    for( ; ; )
    {
        uint32_t bits = 0;
        TickType_t last_motion;

        /* Idle: sensor off, sleep until the motion pin fires (waking every
         * 500 ms to kick the watchdog). Stale DMA events from the previous
         * session are discarded here. */
        watchdog_kick( WD_CAMERA );

        if( ( xTaskNotifyWait( 0, UINT32_MAX, &bits, pdMS_TO_TICKS( 500 ) ) == pdFALSE ) || !( bits & EV_MOTION ) )
        {
            continue;
        }

        last_motion = xTaskGetTickCount();
        s_stats.wakeups++;
        s_session_start = last_motion;
        s_streaming = 1;
        s_next_is_wake = 1;
        hal_sensor_set_power( 1 );
        LOG( "[camera] motion -> sensor on, streaming" );

        while( ( xTaskGetTickCount() - last_motion ) < pdMS_TO_TICKS( QUIET_TIMEOUT_MS ) )
        {
            watchdog_kick( WD_CAMERA );
            prvCaptureOne( &last_motion );
        }

        hal_sensor_set_power( 0 );
        ( void ) hal_sensor_dma_abort();
        taskENTER_CRITICAL();
        s_awake_ticks += xTaskGetTickCount() - s_session_start;
        s_streaming = 0;
        taskEXIT_CRITICAL();
        LOG( "[camera] no activity for %d s -> sensor off, idle", QUIET_TIMEOUT_MS / 1000 );
    }
}

/*--------------------------------- API -----------------------------------*/

QueueHandle_t camera_driver_init( void )
{
    s_frame_queue = xQueueCreate( FRAME_QUEUE_LEN, sizeof( int ) );

    if( ( s_frame_queue == NULL ) ||
        ( xTaskCreate( prvCameraTask, "Camera", CAMERA_STACK_WORDS, NULL, PRIO_CAMERA, &s_task ) != pdPASS ) )
    {
        return NULL;
    }

    vQueueAddToRegistry( s_frame_queue, "FrameQ" );
    vPortSetInterruptHandler( IRQ_MOTION, prvMotionIsr );
    vPortSetInterruptHandler( IRQ_DMA_DONE, prvDmaDoneIsr );
    watchdog_register( WD_CAMERA, "CameraTask", 2000, NULL );
    return s_frame_queue;
}

void camera_note_activity( void )
{
    if( s_task != NULL )
    {
        xTaskNotify( s_task, EV_MOTION, eSetBits );
    }
}

void camera_get_stats( camera_stats_t * out )
{
    *out = *( const camera_stats_t * ) &s_stats;
}

int camera_is_streaming( void )
{
    return s_streaming;
}

uint32_t camera_awake_ms( void )
{
    TickType_t ticks;

    taskENTER_CRITICAL();
    ticks = s_awake_ticks + ( s_streaming ? ( xTaskGetTickCount() - s_session_start ) : 0 );
    taskEXIT_CRITICAL();
    return ( uint32_t ) ( ticks * portTICK_PERIOD_MS );
}
