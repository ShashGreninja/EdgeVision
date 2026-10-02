#include "pipeline/inference.h"

#include <string.h>

#include "task.h"

#include "hal/hal_irq.h"
#include "hal/hal_npu.h"
#include "services/governor.h"

#define INFERENCE_STACK_WORDS    ( configMINIMAL_STACK_SIZE * 2 )

static QueueHandle_t s_jobs;
static QueueHandle_t s_decisions;
static TaskHandle_t s_task;
static inference_stats_t s_stats;

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
        infer_job_t job;
        npu_result_t res;
        detections_t out;
        int accepted;

        xQueueReceive( s_jobs, &job, portMAX_DELAY );

        /* The NPU driver uses a host lock; don't get switched out holding it. */
        taskENTER_CRITICAL();
        accepted = ( hal_npu_submit( job.seq, &job.roi ) == 0 );
        taskEXIT_CRITICAL();

        if( !accepted || ( ulTaskNotifyTake( pdTRUE, pdMS_TO_TICKS( NPU_TIMEOUT_MS ) ) == 0 ) )
        {
            s_stats.timeouts++;
            governor_complete( job.reserved_us, job.cost_us, GOV_DROPPED );
            continue;
        }

        taskENTER_CRITICAL();
        hal_npu_get_result( &res );
        taskEXIT_CRITICAL();

        s_stats.last_device_us = res.device_us;
        s_stats.last_host_us = res.host_us;
        governor_complete( job.reserved_us, job.cost_us + res.device_us, GOV_FULL );

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

int inference_init( QueueHandle_t infer_queue, QueueHandle_t decision_queue )
{
    s_jobs = infer_queue;
    s_decisions = decision_queue;

    if( xTaskCreate( prvInferenceTask, "Inference", INFERENCE_STACK_WORDS, NULL, PRIO_INFERENCE, &s_task ) != pdPASS )
    {
        return -1;
    }

    vPortSetInterruptHandler( IRQ_NPU_DONE, prvNpuDoneIsr );
    return 0;
}

void inference_take_stats( inference_stats_t * out )
{
    taskENTER_CRITICAL();
    *out = s_stats;
    memset( s_stats.peak, 0, sizeof( s_stats.peak ) );
    taskEXIT_CRITICAL();
}
