#ifndef HAL_INPUT_H
#define HAL_INPUT_H

/* Virtual inputs that raise the MOTION interrupt line:
 *   - keyboard: 'm' raises motion, 'q' quits (only when stdin is a console)
 *   - UDP:      a datagram starting with "MOTION" on 127.0.0.1:<udp_port>
 *               (this is what the Ring gateway will send on Day 3)
 *   - auto:     raise motion every N seconds, for unattended test runs */

typedef struct
{
    int keyboard;      /* Non-zero to enable the keyboard thread. */
    int udp_port;      /* 0 disables UDP.                         */
    int auto_motion_s; /* 0 disables automatic motion.            */
} hal_input_cfg_t;

void hal_input_init( const hal_input_cfg_t * cfg );

#endif /* HAL_INPUT_H */
