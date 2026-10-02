#include "pipeline/motion.h"

#include <string.h>

#include "task.h"

#include "drivers/frame_pool.h"
#include "hal/hal_sensor.h"
#include "hal/hal_time.h"
#include "pipeline/pipeline_types.h"
#include "services/governor.h"

#define MOTION_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )
#define GRID_CELLS            ( MOTION_GRID_W * MOTION_GRID_H )

static QueueHandle_t s_frames;
static QueueHandle_t s_jobs;
static TickType_t s_last_forward; /* When a frame last went to inference. */
static hal_roi_t s_focus;         /* Where something was last seen.      */
static TickType_t s_focus_at;
static int s_has_focus;
static uint32_t s_threshold;
static uint32_t s_cpu_scale;
static volatile motion_stats_t s_stats;

/* Working memory, allocated once from the RTOS heap. */
static uint8_t * s_prev; /* Previous frame's grid. */
static uint8_t * s_cur;  /* Current frame's grid.  */
static uint8_t * s_mask; /* 1 = cell changed.      */

static void prvShrink( const uint8_t * img, uint8_t * grid )
{
    for( int gy = 0; gy < MOTION_GRID_H; gy++ )
    {
        for( int gx = 0; gx < MOTION_GRID_W; gx++ )
        {
            const uint8_t * p = img + ( gy * MOTION_CELL ) * FRAME_W + gx * MOTION_CELL;
            uint32_t sum = 0;

            for( int y = 0; y < MOTION_CELL; y++, p += FRAME_W )
            {
                sum += p[ 0 ] + p[ 1 ] + p[ 2 ] + p[ 3 ];
            }

            grid[ gy * MOTION_GRID_W + gx ] = ( uint8_t ) ( sum / ( MOTION_CELL * MOTION_CELL ) );
        }
    }
}

/* Compare s_cur with s_prev. Returns the number of changed (non-isolated)
 * cells and their bounding box in frame coordinates. */
static uint32_t prvCompare( hal_roi_t * roi )
{
    int x0 = MOTION_GRID_W;
    int y0 = MOTION_GRID_H;
    int x1 = -1;
    int y1 = -1;
    uint32_t count = 0;

    for( int i = 0; i < GRID_CELLS; i++ )
    {
        const int d = ( int ) s_cur[ i ] - ( int ) s_prev[ i ];

        s_mask[ i ] = ( uint8_t ) ( ( d > ( int ) s_threshold ) || ( -d > ( int ) s_threshold ) );
    }

    for( int gy = 0; gy < MOTION_GRID_H; gy++ )
    {
        for( int gx = 0; gx < MOTION_GRID_W; gx++ )
        {
            const int i = gy * MOTION_GRID_W + gx;

            if( !s_mask[ i ] )
            {
                continue;
            }

            const int neighbour = ( ( gx > 0 ) && s_mask[ i - 1 ] ) ||
                                  ( ( gx < MOTION_GRID_W - 1 ) && s_mask[ i + 1 ] ) ||
                                  ( ( gy > 0 ) && s_mask[ i - MOTION_GRID_W ] ) ||
                                  ( ( gy < MOTION_GRID_H - 1 ) && s_mask[ i + MOTION_GRID_W ] );

            if( !neighbour )
            {
                continue; /* Isolated cell: sensor noise. */
            }

            count++;
            x0 = ( gx < x0 ) ? gx : x0;
            y0 = ( gy < y0 ) ? gy : y0;
            x1 = ( gx > x1 ) ? gx : x1;
            y1 = ( gy > y1 ) ? gy : y1;
        }
    }

    if( count > 0 )
    {
        roi->x = ( uint16_t ) ( x0 * MOTION_CELL );
        roi->y = ( uint16_t ) ( y0 * MOTION_CELL );
        roi->w = ( uint16_t ) ( ( x1 - x0 + 1 ) * MOTION_CELL );
        roi->h = ( uint16_t ) ( ( y1 - y0 + 1 ) * MOTION_CELL );
    }

    return count;
}

static void prvMotionTask( void * param )
{
    ( void ) param;

    for( ; ; )
    {
        int idx;
        infer_job_t job;
        uint32_t cells = 0;
        uint8_t * swap;

        xQueueReceive( s_frames, &idx, portMAX_DELAY );
        frame_pool_move( idx, BUF_READY, BUF_PROCESSING );

        frame_buf_t * fb = frame_pool_get( idx );
        const uint64_t start = hal_time_us();

        memset( &job, 0, sizeof( job ) );
        job.seq = fb->seq;
        job.wake = fb->wake;
        job.reserved_us = fb->gov_reserved_us;

        prvShrink( fb->data, s_cur );

        if( !fb->wake )
        {
            cells = prvCompare( &job.roi );
        }

        /* No motion region, but look anyway when:
         * - the camera just woke (no previous frame; Ring already saw motion), or
         * - nothing has gone to inference for a while (someone standing still). */
        const int recheck = !fb->wake && ( cells < MOTION_MIN_CELLS ) &&
                            ( ( fb->captured_at - s_last_forward ) >= pdMS_TO_TICKS( MOTION_RECHECK_MS ) );

        if( fb->wake || recheck )
        {
            taskENTER_CRITICAL();
            const int focus = s_has_focus && !fb->wake &&
                              ( ( fb->captured_at - s_focus_at ) <= pdMS_TO_TICKS( MOTION_FOCUS_MS ) );
            const hal_roi_t focus_box = s_focus;
            taskEXIT_CRITICAL();

            job.wake = 1;

            if( focus )
            {
                job.roi = focus_box; /* Look where it was last seen. */
            }
            else
            {
                job.roi.x = 0;
                job.roi.y = 0;
                job.roi.w = FRAME_W;
                job.roi.h = FRAME_H;
            }
        }

        swap = s_prev;
        s_prev = s_cur;
        s_cur = swap;

        job.cost_us = GOV_FRAME_BASE_US + ( uint32_t ) ( hal_time_us() - start ) * s_cpu_scale;

        /* The pixels are no longer needed: the NPU reads its own colour copy
         * of this frame from video memory. Free the slot for the camera now. */
        frame_pool_move( idx, BUF_PROCESSING, BUF_FREE );

        s_stats.frames++;
        s_stats.last_cells = cells;
        s_stats.last_cost_us = job.cost_us;

        if( !job.wake && ( cells < MOTION_MIN_CELLS ) )
        {
            s_stats.gated++;
            governor_complete( job.reserved_us, job.cost_us, GOV_GATED );
        }
        else if( xQueueSend( s_jobs, &job, 0 ) == pdPASS )
        {
            s_stats.forwarded++;
            s_stats.rechecks += ( uint32_t ) recheck;
            s_last_forward = fb->captured_at;
        }
        else
        {
            s_stats.infer_busy++;
            governor_complete( job.reserved_us, job.cost_us, GOV_DROPPED );
        }
    }
}

int motion_init( QueueHandle_t frame_queue, QueueHandle_t infer_queue, uint32_t threshold, uint32_t cpu_scale )
{
    s_frames = frame_queue;
    s_jobs = infer_queue;
    s_threshold = threshold;
    s_cpu_scale = cpu_scale;
    s_prev = pvPortMalloc( GRID_CELLS );
    s_cur = pvPortMalloc( GRID_CELLS );
    s_mask = pvPortMalloc( GRID_CELLS );

    if( ( s_prev == NULL ) || ( s_cur == NULL ) || ( s_mask == NULL ) )
    {
        return -1;
    }

    memset( s_prev, 0, GRID_CELLS );
    return ( xTaskCreate( prvMotionTask, "Motion", MOTION_STACK_WORDS, NULL, PRIO_MOTION, NULL ) == pdPASS ) ? 0 : -1;
}

void motion_set_focus( const hal_roi_t * box )
{
    taskENTER_CRITICAL();
    s_focus = *box;
    s_focus_at = xTaskGetTickCount();
    s_has_focus = 1;
    taskEXIT_CRITICAL();
}

void motion_get_stats( motion_stats_t * out )
{
    *out = *( const motion_stats_t * ) &s_stats;
}
