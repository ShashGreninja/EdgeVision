#include <stdarg.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "common/log.h"

void ev_log( const char * fmt, ... )
{
    va_list ap;
    const int in_scheduler = ( xTaskGetSchedulerState() == taskSCHEDULER_RUNNING );

    va_start( ap, fmt );

    if( in_scheduler )
    {
        taskENTER_CRITICAL();
    }

    vprintf( fmt, ap );
    fputc( '\n', stdout );
    fflush( stdout );

    if( in_scheduler )
    {
        taskEXIT_CRITICAL();
    }

    va_end( ap );
}
