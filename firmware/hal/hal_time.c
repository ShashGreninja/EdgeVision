#include <windows.h>

#include "hal/hal_time.h"

uint64_t hal_time_us( void )
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if( freq.QuadPart == 0 )
    {
        QueryPerformanceFrequency( &freq );
    }

    QueryPerformanceCounter( &now );
    return ( uint64_t ) ( now.QuadPart / freq.QuadPart ) * 1000000ULL +
           ( uint64_t ) ( now.QuadPart % freq.QuadPart ) * 1000000ULL / ( uint64_t ) freq.QuadPart;
}
