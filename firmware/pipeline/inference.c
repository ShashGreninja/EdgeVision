#include "pipeline/inference.h"

#include <string.h>

#include "task.h"

#include "common/log.h"
#include "hal/hal_irq.h"
#include "hal/hal_npu.h"
#include "services/governor.h"
#include "services/watchdog.h"

#define INFERENCE_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )
#define INFERENCE_WD_MS          2000

static QueueHandle_t s_jobs;
static QueueHandle_t s_decisions;
static TaskHandle_t s_task;
static inference_stats_t s_stats;

/* The task's memory is allocated once, so a watchdog restart reuses it
 * instead of allocating after start-up. */
static StaticTask_t * s_tcb;
static StackType_t * s_stack;

/* The job the NPU is working on, so a restart can settle its budget. */
static infer_job_t s_current;
static volatile int s_busy;

static uint32_t prvNpuDoneIsr( void )
{
    BaseType_t woken = pdFALSE;

    vTaskNotifyGiveFromISR( s_task, &woken );
    return ( uint32_t ) woken;
}

static void prvInferenceTask( void * param )
{
    ( void ) param;

    for( ; ; )
    {
        npu_result_t res;
        detections_t out;
        int accepted;

        watchdog_kick( WD_INFERENCE );

        if( xQueueReceive( s_jobs, &s_current, pdMS_TO_TICKS( 500 ) ) != pdPASS )
        {
            continue;
        }

        /* The NPU driver uses a host lock; don't get switched out holding it. */
        taskENTER_CRITICAL();
        accepted = ( hal_npu_submit( s_current.seq, &s_current.roi ) == 0 );
        taskEXIT_CRITICAL();

        if( !accepted )
        {
            s_stats.rejected++;
            governor_complete( s_current.reserved_us, s_current.cost_us, GOV_DROPPED );
            continue;
        }

        /* Like a simple driver, wait for the completion interrupt with no
         * timeout. If the NPU never answers, this task stops kicking and the
         * watchdog restarts it (see prvRestart). */
        s_busy = 1;
        ( void ) ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
        s_busy = 0;

        taskENTER_CRITICAL();
        hal_npu_get_result( &res );
        taskEXIT_CRITICAL();

        s_stats.last_device_us = res.device_us;
        s_stats.last_host_us = res.host_us;
        governor_complete( s_current.reserved_us, s_current.cost_us + res.device_us, GOV_FULL );

        if( res.status != NPU_OK )
        {
            s_stats.frame_gone++;
            continue;
        }

        s_stats.jobs++;
        memset( &out, 0, sizeof( out ) );
        out.seq = res.seq;
        out.at = xTaskGetTickCount();

        for( uint32_t i = 0; i < res.n; i++ )
        {
            const int cat = category_from_coco( res.det[ i ].coco_class );

            if( ( cat >= 0 ) && ( res.det[ i ].score > out.score[ cat ] ) )
            {
                out.score[ cat ] = res.det[ i ].score;
                out.box[ cat ] = res.det[ i ].box;
            }
        }

        taskENTER_CRITICAL();

        for( int c = 0; c < CAT_COUNT; c++ )
        {
            if( out.score[ c ] > s_stats.peak[ c ] )
            {
                s_stats.peak[ c ] = out.score[ c ];
            }
        }

        taskEXIT_CRITICAL();

        /* Every result goes on, including empty ones: the decision logic
         * needs the misses too. */
        ( void ) xQueueSend( s_decisions, &out, pdMS_TO_TICKS( 10 ) );
    }
}

static int prvStart( void )
{
    s_task = xTaskCreateStatic( prvInferenceTask, "Inference", INFERENCE_STACK_WORDS, NULL, PRIO_INFERENCE, s_stack, s_tcb );
    return ( s_task != NULL ) ? 0 : -1;
}

/* Called by the watchdog when InferenceTask has gone silent. */
static void prvRestart( void )
{
    vTaskDelete( s_task );

    /* Reset the accelerator so it drops whatever it was stuck on. */
    taskENTER_CRITICAL();
    hal_npu_reset();
    taskEXIT_CRITICAL();

    if( s_busy )
    {
        governor_complete( s_current.reserved_us, s_current.cost_us, GOV_DROPPED );
        s_busy = 0;
    }

    s_stats.restarts++;
    ( void ) prvStart();
    LOG( "[inference] NPU reset and InferenceTask restarted" );
}

int inference_init( QueueHandle_t infer_queue, QueueHandle_t decision_queue )
{
    s_jobs = infer_queue;
    s_decisions = decision_queue;
    s_tcb = pvPortMalloc( sizeof( StaticTask_t ) );
    s_stack = pvPortMalloc( INFERENCE_STACK_WORDS * sizeof( StackType_t ) );

    if( ( s_tcb == NULL ) || ( s_stack == NULL ) || ( prvStart() != 0 ) )
    {
        return -1;
    }

    vPortSetInterruptHandler( IRQ_NPU_DONE, prvNpuDoneIsr );
    watchdog_register( WD_INFERENCE, "InferenceTask", INFERENCE_WD_MS, prvRestart );
    return 0;
}

void inference_take_stats( inference_stats_t * out )
{
    taskENTER_CRITICAL();
    *out = s_stats;
    memset( s_stats.peak, 0, sizeof( s_stats.peak ) );
    taskEXIT_CRITICAL();
}
