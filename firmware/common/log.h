#ifndef EV_LOG_H
#define EV_LOG_H

/* Logging for FreeRTOS tasks (and for code that runs before the scheduler).
 *
 * On the Windows simulator a task must not be switched out while it holds a
 * Windows lock (such as the C runtime's stdout lock), so each line is printed
 * inside a critical section. Do NOT call this from simulated ISRs or from the
 * virtual-hardware Windows threads; those use plain printf with a "[hw]" tag. */
void ev_log( const char * fmt, ... ) __attribute__( ( format( printf, 1, 2 ) ) );

#define LOG( ... )    ev_log( __VA_ARGS__ )

#endif /* EV_LOG_H */
