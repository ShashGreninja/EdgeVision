#ifndef HAL_VIDEO_MEM_HPP
#define HAL_VIDEO_MEM_HPP

/* Video memory shared by the camera module's ISP and the NPU (hardware side
 * only, C++). The ISP writes a small colour copy of every frame here; the
 * NPU reads it back by sequence number. It is separate from the firmware's
 * 512 KB RAM, as on camera SoCs where the ISP/NPU have their own memory. */

#include <cstdint>

#include <opencv2/core.hpp>

constexpr int VIDEO_MEM_W = 640;
constexpr int VIDEO_MEM_H = 360;
constexpr int VIDEO_MEM_FRAMES = 8; /* ~266 ms of history at 30 fps. */

/* Copy the colour frame with this sequence number into out (BGR).
 * Returns false if it has already been overwritten. */
bool hal_video_mem_get( uint32_t seq, cv::Mat & out );

#endif /* HAL_VIDEO_MEM_HPP */
