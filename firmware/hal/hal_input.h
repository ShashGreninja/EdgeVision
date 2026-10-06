#ifndef HAL_INPUT_H
#define HAL_INPUT_H

/* Virtual inputs that raise the MOTION interrupt line:
 *   - keyboard: 'm' raises motion, '1'-'7' inject faults, 'q' quits (only when
 *     stdin is a console)
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

/* Debug commands: "FAULT <name>" over UDP or keys 1-7 add one to a pending
 * count for that fault (a fault_id_t) and raise IRQ_FAULT. Returns how many
 * of that command are pending and clears the count. */
unsigned hal_input_take_fault( int id );

#endif /* HAL_INPUT_H */
