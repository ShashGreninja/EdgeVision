#ifndef HAL_NPU_H
#define HAL_NPU_H

/* Virtual neural accelerator (NPU).
 *
 * Runs as a Windows thread ("hardware"). The firmware submits a job naming
 * a frame (by sensor sequence number) and a region of interest; the NPU
 * reads that frame's colour copy from video memory, runs the object
 * detector on the region, and raises IRQ_NPU_DONE. It takes at least the
 * configured device latency, to emulate a small camera NPU rather than the
 * host CPU. One job at a time. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rectangle in firmware frame coordinates (FRAME_W x FRAME_H). */
typedef struct
{
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} hal_roi_t;

#define NPU_MAX_DETS    16

typedef struct
{
    uint8_t coco_class; /* 0-based COCO class id (0 = person). */
    float score;        /* 0..1 */
    hal_roi_t box;      /* Firmware frame coordinates. */
} npu_det_t;

enum
{
    NPU_OK = 0,
    NPU_ERR_FRAME_GONE = -1, /* Frame was overwritten in video memory before the job ran. */
    NPU_ERR_MODEL = -2       /* The model failed on this input. */
};

typedef struct
{
    uint32_t seq;
    int32_t status;
    uint32_t device_us; /* Emulated device time for this job. */
    uint32_t host_us;   /* Time the host actually spent. */
    uint32_t n;
    npu_det_t det[ NPU_MAX_DETS ];
} npu_result_t;

/* Load the model and start the NPU thread. device_latency_ms is the minimum
 * time a job takes. Returns 0 on success. */
int hal_npu_init( const char * model_path, uint32_t device_latency_ms );

/* Start a job. Returns 0 if accepted, -1 if the NPU is still busy. */
int hal_npu_submit( uint32_t seq, const hal_roi_t * roi );

/* Read the result of the last job (valid after IRQ_NPU_DONE). */
void hal_npu_get_result( npu_result_t * out );

/* Reset the NPU: abandon the current job (no IRQ will follow) and accept
 * new jobs again. */
void hal_npu_reset( void );

/* Fault injection: the next job hangs and never completes until a reset. */
void hal_npu_fault_hang( void );

#ifdef __cplusplus
}
#endif

#endif /* HAL_NPU_H */
