#ifndef EV_FAULT_IDS_H
#define EV_FAULT_IDS_H

/* Fault-injection commands, shared by the debug inputs (hal_input) and the
 * fault service. Sent as "FAULT <name>" over UDP, or keys 1-7 in a console. */

typedef enum
{
    FAULT_CAMERA_DISCONNECT = 0,
    FAULT_FRAME_CORRUPT,
    FAULT_MEM_PRESSURE,
    FAULT_INFERENCE_HANG,
    FAULT_NETWORK_DOWN,
    FAULT_CPU_OVERLOAD,
    FAULT_SELFTEST, /* Not a fault: sends a test event through the network path. */
    FAULT_COUNT
} fault_id_t;

static const char * const FAULT_NAMES[ FAULT_COUNT ] =
{
    "camera_disconnect", "frame_corrupt", "mem_pressure", "inference_hang", "network_down", "cpu_overload", "selftest"
};

#endif /* EV_FAULT_IDS_H */
