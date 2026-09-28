#include "drivers/camera_driver.h"

#include "FreeRTOS.h"
#include "task.h"

#include "common/log.h"
#include "drivers/frame_pool.h"
#include "hal/hal_irq.h"
#include "hal/hal_sensor.h"

#define EV_MOTION           ( 1UL << 0 )
#define EV_DMA_DONE         ( 1UL << 1 )

#define FRAME_QUEUE_LEN     ( FRAME_POOL_SLOTS )
#define QUIET_TIMEOUT_MS    ( 5000 )
#define DMA_TIMEOUT_MS      ( 200 )
#define CLAIM_WAIT_MS       ( 100 )
#define CAMERA_STACK_WORDS  ( configMINIMAL_STACK_SIZE * 2 )

static TaskHandle_t s_task;
static QueueHandle_t s_frame_queue;
static volatile camera_stats_t s_stats;
static volatile int s_streaming;

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
            return;
        }
    }

    fb->seq = hal_sensor_last_seq();
    fb->captured_at = xTaskGetTickCount();
    frame_pool_move( idx, BUF_FILLING, BUF_READY );
    s_stats.frames_captured++;

    if( xQueueSend( s_frame_queue, &idx, 0 ) != pdPASS )
    {
        s_stats.queue_drops++;
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

        /* Idle: sensor off, sleep until the motion pin fires. Stale DMA
         * events from the previous session are discarded here. */
        xTaskNotifyWait( 0, UINT32_MAX, &bits, portMAX_DELAY );

        if( !( bits & EV_MOTION ) )
        {
            continue;
        }

        last_motion = xTaskGetTickCount();
        s_stats.wakeups++;
        s_streaming = 1;
        hal_sensor_set_power( 1 );
        LOG( "[camera] motion -> sensor on, streaming" );

        while( ( xTaskGetTickCount() - last_motion ) < pdMS_TO_TICKS( QUIET_TIMEOUT_MS ) )
        {
            prvCaptureOne( &last_motion );
        }

        hal_sensor_set_power( 0 );
        ( void ) hal_sensor_dma_abort();
        s_streaming = 0;
        LOG( "[camera] no motion for %d s -> sensor off, idle", QUIET_TIMEOUT_MS / 1000 );
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
    return s_frame_queue;
}

void camera_get_stats( camera_stats_t * out )
{
    *out = *( const camera_stats_t * ) &s_stats;
}

int camera_is_streaming( void )
{
    return s_streaming;
}
