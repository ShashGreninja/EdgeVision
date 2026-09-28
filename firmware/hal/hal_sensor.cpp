#include "hal/hal_sensor.h"
#include "hal/hal_irq.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace
{
std::atomic<uint8_t *> g_dma_dst { nullptr };
std::atomic<int> g_powered { 0 };
std::atomic<uint32_t> g_last_seq { 0 };
std::atomic<uint32_t> g_produced { 0 };
std::atomic<uint32_t> g_transferred { 0 };
std::atomic<uint32_t> g_overruns { 0 };
std::string g_clip;

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

        if( !g_powered.load() )
        {
            continue;
        }

        /* Capture at native resolution. */
        if( cap.isOpened() )
        {
            if( !cap.read( native ) || native.empty() )
            {
                cap.set( cv::CAP_PROP_POS_FRAMES, 0 ); /* Loop the clip. */

                if( !cap.read( native ) || native.empty() )
                {
                    continue;
                }
            }
        }
        else
        {
            synth_frame( native, seq );
        }

        ++seq;
        ++g_produced;

        /* On-module ISP: grayscale + downscale, so a full 1080p frame never
         * reaches firmware RAM. */
        cv::cvtColor( native, gray, cv::COLOR_BGR2GRAY );
        cv::resize( gray, out, cv::Size( FRAME_W, FRAME_H ), 0, 0, cv::INTER_AREA );

        /* DMA: one-shot transfer into whatever buffer the firmware armed. */
        uint8_t * dst = g_dma_dst.exchange( nullptr );

        if( dst == nullptr )
        {
            ++g_overruns;
            continue;
        }

        std::memcpy( dst, out.data, FRAME_BYTES );
        g_last_seq.store( seq );
        ++g_transferred;
        vPortGenerateSimulatedInterruptFromWindowsThread( IRQ_DMA_DONE );
    }
}
} // namespace

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
