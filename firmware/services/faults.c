#include "services/faults.h"

#include "FreeRTOS.h"
#include "task.h"

#include "common/log.h"
#include "drivers/frame_pool.h"
#include "hal/hal_input.h"
#include "hal/hal_irq.h"
#include "hal/hal_npu.h"
#include "hal/hal_sensor.h"
#include "pipeline/decision.h"
#include "services/governor.h"
#include "services/network.h"

#define FAULTS_STACK_WORDS        ( configMINIMAL_STACK_SIZE * 2 )
#define PRIO_FAULTS               ( PRIO_NETWORK )

#define DISCONNECT_MS             3000
#define CORRUPT_FRAMES            5
#define MEM_PRESSURE_MS           5000
#define NETWORK_DOWN_S            10
#define CPU_OVERLOAD_MS           5000
#define CPU_OVERLOAD_PERCENT      400

static TaskHandle_t s_task;
static faults_stats_t s_stats;

/* Pending restorations (0 = nothing pending). */
static int s_reserved_slot = -1;
static TickType_t s_mem_restore_at;
static TickType_t s_cpu_restore_at;

static uint32_t prvFaultIsr( void )
{
    BaseType_t woken = pdFALSE;

    vTaskNotifyGiveFromISR( s_task, &woken );
    return ( uint32_t ) woken;
}

static void prvInject( fault_id_t id )
{
    const TickType_t now = xTaskGetTickCount();

    s_stats.injected[ id ]++;

    switch( id )
    {
        case FAULT_CAMERA_DISCONNECT:
            LOG( "[fault] camera_disconnect: sensor stops delivering frames for %d s", DISCONNECT_MS / 1000 );
            hal_sensor_fault_disconnect( DISCONNECT_MS );
            break;

        case FAULT_FRAME_CORRUPT:
            LOG( "[fault] frame_corrupt: next %d transfers corrupted on the bus", CORRUPT_FRAMES );
            hal_sensor_fault_corrupt( CORRUPT_FRAMES );
            break;

        case FAULT_MEM_PRESSURE:
            if( s_reserved_slot >= 0 )
            {
                LOG( "[fault] mem_pressure: already active" );
                break;
            }

            s_reserved_slot = frame_pool_reserve( pdMS_TO_TICKS( 1000 ) );

            if( s_reserved_slot < 0 )
            {
                LOG( "[fault] mem_pressure: no slot became free within 1 s" );
                break;
            }

            s_mem_restore_at = now + pdMS_TO_TICKS( MEM_PRESSURE_MS );
            s_stats.mem_pressure_active = 1;
            LOG( "[fault] mem_pressure: frame slot %d lent to another subsystem for %d s, pipeline runs on %d slots",
                 s_reserved_slot, MEM_PRESSURE_MS / 1000, FRAME_POOL_SLOTS - 1 );
            break;

        case FAULT_INFERENCE_HANG:
            LOG( "[fault] inference_hang: the NPU will hang on its next job" );
            taskENTER_CRITICAL();
            hal_npu_fault_hang();
            taskEXIT_CRITICAL();
            break;

        case FAULT_NETWORK_DOWN:
            LOG( "[fault] network_down: Wi-Fi module goes offline for %d s", NETWORK_DOWN_S );
            network_simulate_outage( NETWORK_DOWN_S );
            break;

        case FAULT_CPU_OVERLOAD:
            governor_set_load_percent( CPU_OVERLOAD_PERCENT );
            s_cpu_restore_at = now + pdMS_TO_TICKS( CPU_OVERLOAD_MS );
            s_stats.cpu_overload_active = 1;
            LOG( "[fault] cpu_overload: every frame costs %dx for %d s", CPU_OVERLOAD_PERCENT / 100, CPU_OVERLOAD_MS / 1000 );
            break;

        case FAULT_SELFTEST:
            decision_selftest();
            break;

        default:
            break;
    }
}

static void prvRestore( void )
{
    const TickType_t now = xTaskGetTickCount();

    if( ( s_reserved_slot >= 0 ) && ( ( int32_t ) ( now - s_mem_restore_at ) >= 0 ) )
    {
        frame_pool_move( s_reserved_slot, BUF_RESERVED, BUF_FREE );
        LOG( "[fault] mem_pressure over: slot %d back in rotation", s_reserved_slot );
        s_reserved_slot = -1;
        s_stats.mem_pressure_active = 0;
    }

    if( s_stats.cpu_overload_active && ( ( int32_t ) ( now - s_cpu_restore_at ) >= 0 ) )
    {
        governor_set_load_percent( 100 );
        s_stats.cpu_overload_active = 0;
        LOG( "[fault] cpu_overload over: normal frame cost" );
    }
}

static void prvFaultTask( void * param )
{
    ( void ) param;

    for( ; ; )
    {
        ( void ) ulTaskNotifyTake( pdTRUE, pdMS_TO_TICKS( 100 ) );

        const unsigned pending = hal_input_take_faults();

        for( int id = 0; id < FAULT_COUNT; id++ )
        {
            if( pending & ( 1u << id ) )
            {
                prvInject( ( fault_id_t ) id );
            }
        }

        prvRestore();
    }
}

int faults_init( void )
{
    if( xTaskCreate( prvFaultTask, "Faults", FAULTS_STACK_WORDS, NULL, PRIO_FAULTS, &s_task ) != pdPASS )
    {
        return -1;
    }

    vPortSetInterruptHandler( IRQ_FAULT, prvFaultIsr );
    return 0;
}

void faults_get_stats( faults_stats_t * out )
{
    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}
