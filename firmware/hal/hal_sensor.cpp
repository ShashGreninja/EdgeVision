#include "hal/hal_sensor.h"
#include "common/crc32.h"
#include "hal/hal_irq.h"
#include "hal/hal_video_mem.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace
{
/* Colour ring in video memory, written by the ISP, read by the NPU. */
struct VideoSlot
{
    uint32_t seq = 0;
    /* Parentheses, not braces: cv::Mat{ a, b, c } is a 3-element list. */
    cv::Mat bgr = cv::Mat( VIDEO_MEM_H, VIDEO_MEM_W, CV_8UC3 );
};

std::mutex g_video_mu;
VideoSlot g_video[ VIDEO_MEM_FRAMES ];

std::atomic<uint8_t *> g_dma_dst { nullptr };
std::atomic<int> g_powered { 0 };
std::atomic<uint32_t> g_last_seq { 0 };
std::atomic<uint32_t> g_produced { 0 };
std::atomic<uint32_t> g_transferred { 0 };
std::atomic<uint32_t> g_overruns { 0 };
std::atomic<uint32_t> g_last_crc { 0 };
std::atomic<int64_t> g_disconnected_until { 0 }; /* steady_clock ms; 0 = connected. */
std::atomic<uint32_t> g_corrupt_next { 0 };
std::string g_clip;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

bool disconnected()
{
    return now_ms() < g_disconnected_until.load();
}

/* Fit a clip frame of any shape into the sensor's 16:9 frame without
 * distorting it (black bars at the sides or top/bottom). */
void fit_to_sensor( const cv::Mat & src, cv::Mat & native )
{
    const double scale = std::min( SENSOR_NATIVE_W / static_cast<double>( src.cols ),
                                   SENSOR_NATIVE_H / static_cast<double>( src.rows ) );
    const int w = std::max( 1, static_cast<int>( src.cols * scale ) );
    const int h = std::max( 1, static_cast<int>( src.rows * scale ) );
    cv::Mat sized;

    cv::resize( src, sized, cv::Size( w, h ), 0, 0, cv::INTER_AREA );
    native.setTo( cv::Scalar( 0, 0, 0 ) );
    sized.copyTo( native( cv::Rect( ( SENSOR_NATIVE_W - w ) / 2, ( SENSOR_NATIVE_H - h ) / 2, w, h ) ) );
}

/* Synthetic scene: a bright block walks across a dim background. */
void synth_frame( cv::Mat & bgr, uint32_t seq )
{
    const int w = 160;
    const int h = 360;
    const int x = static_cast<int>( ( seq * 12 ) % ( SENSOR_NATIVE_W + w ) ) - w;

    bgr.setTo( cv::Scalar( 40, 45, 50 ) );
    cv::rectangle( bgr, cv::Rect( x, 500, w, h ), cv::Scalar( 200, 190, 180 ), cv::FILLED );
}

void sensor_thread()
{
    cv::VideoCapture cap;
    cv::Mat clip_frame;
    cv::Mat native( SENSOR_NATIVE_H, SENSOR_NATIVE_W, CV_8UC3 );
    cv::Mat gray;
    cv::Mat out;
    uint32_t seq = 0;
    const auto period = std::chrono::microseconds( 1000000 / SENSOR_FPS );
    auto next = std::chrono::steady_clock::now();

    if( !g_clip.empty() )
    {
        if( cap.open( g_clip ) )
        {
            std::printf( "[hw] sensor: streaming from clip '%s'\n", g_clip.c_str() );
        }
        else
        {
            std::printf( "[hw] sensor: cannot open clip '%s', using synthetic scene\n", g_clip.c_str() );
        }
    }
    else
    {
        std::printf( "[hw] sensor: no clip given, using synthetic scene\n" );
    }

    for( ; ; )
    {
        next += period;
        std::this_thread::sleep_until( next );

        if( !g_powered.load() || disconnected() )
        {
            continue;
        }

        /* Capture at native resolution. */
        if( cap.isOpened() )
        {
            if( !cap.read( clip_frame ) || clip_frame.empty() )
            {
                cap.set( cv::CAP_PROP_POS_FRAMES, 0 ); /* Loop the clip. */

                if( !cap.read( clip_frame ) || clip_frame.empty() )
                {
                    continue;
                }
            }

            fit_to_sensor( clip_frame, native );
        }
        else
        {
            synth_frame( native, seq );
        }

        ++seq;
        ++g_produced;

        /* ISP output 1: small colour copy into video memory, for the NPU. */
        {
            std::lock_guard<std::mutex> lock( g_video_mu );
            VideoSlot & slot = g_video[ seq % VIDEO_MEM_FRAMES ];

            cv::resize( native, slot.bgr, slot.bgr.size(), 0, 0, cv::INTER_AREA );
            slot.seq = seq;
        }

        /* ISP output 2: grayscale QVGA for the firmware, so a full 1080p
         * frame never reaches firmware RAM. */
        cv::cvtColor( native, gray, cv::COLOR_BGR2GRAY );
        cv::resize( gray, out, cv::Size( FRAME_W, FRAME_H ), 0, 0, cv::INTER_AREA );

        /* DMA: one-shot transfer into whatever buffer the firmware armed. */
        uint8_t * dst = g_dma_dst.exchange( nullptr );

        if( dst == nullptr )
        {
            ++g_overruns;
            continue;
        }

        g_last_crc.store( crc32_compute( out.data, FRAME_BYTES ) );
        std::memcpy( dst, out.data, FRAME_BYTES );

        /* Injected bus noise: flip bytes in the copy, after the CRC. */
        if( g_corrupt_next.load() > 0 )
        {
            g_corrupt_next--;

            for( int i = 0; i < 16; i++ )
            {
                dst[ ( seq * 7919u + static_cast<uint32_t>( i ) * 4801u ) % FRAME_BYTES ] ^= 0x5A;
            }
        }

        g_last_seq.store( seq );
        ++g_transferred;
        vPortGenerateSimulatedInterruptFromWindowsThread( IRQ_DMA_DONE );
    }
}
} // namespace

bool hal_video_mem_get( uint32_t seq, cv::Mat & out )
{
    std::lock_guard<std::mutex> lock( g_video_mu );
    const VideoSlot & slot = g_video[ seq % VIDEO_MEM_FRAMES ];

    if( ( seq == 0 ) || ( slot.seq != seq ) )
    {
        return false;
    }

    slot.bgr.copyTo( out );
    return true;
}

extern "C" int hal_sensor_init( const char * clip_path )
{
    g_clip = ( clip_path != nullptr ) ? clip_path : "";
    std::thread( sensor_thread ).detach();
    return 0;
}

extern "C" void hal_sensor_set_power( int on )
{
    g_powered.store( on ? 1 : 0 );
}

extern "C" void hal_sensor_dma_arm( uint8_t * dst )
{
    g_dma_dst.store( dst );
}

extern "C" int hal_sensor_dma_abort( void )
{
    return ( g_dma_dst.exchange( nullptr ) != nullptr ) ? 1 : 0;
}

extern "C" uint32_t hal_sensor_last_seq( void )
{
    return g_last_seq.load();
}

extern "C" void hal_sensor_get_stats( hal_sensor_stats_t * out )
{
    out->produced = g_produced.load();
    out->transferred = g_transferred.load();
    out->overruns = g_overruns.load();
}

extern "C" uint32_t hal_sensor_last_crc( void )
{
    return g_last_crc.load();
}

extern "C" int hal_sensor_reset( void )
{
    return disconnected() ? -1 : 0;
}

extern "C" void hal_sensor_fault_disconnect( uint32_t ms )
{
    g_disconnected_until.store( now_ms() + ms );
}

extern "C" void hal_sensor_fault_corrupt( uint32_t n )
{
    g_corrupt_next.store( n );
}
