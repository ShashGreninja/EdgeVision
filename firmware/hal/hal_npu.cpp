#include "hal/hal_npu.h"
#include "hal/hal_irq.h"
#include "hal/hal_sensor.h"
#include "hal/hal_video_mem.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

/* Detector: NanoDet-Plus (OpenCV model zoo), 416x416 input, 80 COCO classes.
 * Pre/post-processing follows the zoo's reference implementation. */

namespace
{
constexpr int INPUT_SIZE = 416;
constexpr int NUM_CLASSES = 80;
constexpr int REG_MAX = 7; /* Box sides are distributions over 0..7 bins. */
constexpr float SCORE_THRESHOLD = 0.35f;
constexpr float NMS_THRESHOLD = 0.6f;

/* Region handling: grow the motion ROI for context, and never crop smaller
 * than this fraction of the frame height, so a moving hand still shows the
 * whole person. */
constexpr double ROI_GROW = 1.6;
constexpr double MIN_CROP_FRACTION = 0.6;

cv::dnn::Net g_net;
std::vector<std::string> g_out_names;
uint32_t g_latency_ms;

std::mutex g_mu;
std::condition_variable g_cv;
bool g_busy = false;
bool g_has_job = false;
uint32_t g_generation = 0; /* Bumped by every reset; stale jobs are discarded. */
bool g_hang_next = false;
uint32_t g_job_seq;
hal_roi_t g_job_roi;
npu_result_t g_result;

struct Letterbox
{
    cv::Rect crop; /* In video-memory coordinates. */
    double scale;  /* crop -> model input. */
    int left;
    int top;
};

cv::Mat prvPrepare( const cv::Mat & color, const hal_roi_t & roi, Letterbox & lb )
{
    const double sx = color.cols / static_cast<double>( FRAME_W );
    const double sy = color.rows / static_cast<double>( FRAME_H );
    const double cx = ( roi.x + roi.w / 2.0 ) * sx;
    const double cy = ( roi.y + roi.h / 2.0 ) * sy;
    const double min_side = color.rows * MIN_CROP_FRACTION;
    const double w = std::max( roi.w * sx * ROI_GROW, min_side );
    const double h = std::max( roi.h * sy * ROI_GROW, min_side );

    lb.crop = cv::Rect( static_cast<int>( cx - w / 2 ), static_cast<int>( cy - h / 2 ),
                        static_cast<int>( w ), static_cast<int>( h ) ) & cv::Rect( 0, 0, color.cols, color.rows );

    /* Letterbox: keep aspect ratio, pad to a square. */
    lb.scale = std::min( INPUT_SIZE / static_cast<double>( lb.crop.width ), INPUT_SIZE / static_cast<double>( lb.crop.height ) );

    const int nw = std::max( 1, static_cast<int>( std::lround( lb.crop.width * lb.scale ) ) );
    const int nh = std::max( 1, static_cast<int>( std::lround( lb.crop.height * lb.scale ) ) );

    lb.left = ( INPUT_SIZE - nw ) / 2;
    lb.top = ( INPUT_SIZE - nh ) / 2;

    cv::Mat rgb;
    cv::Mat sized;
    cv::Mat padded;
    cv::Mat input;

    cv::cvtColor( color( lb.crop ), rgb, cv::COLOR_BGR2RGB );
    cv::resize( rgb, sized, cv::Size( nw, nh ), 0, 0, cv::INTER_AREA );
    cv::copyMakeBorder( sized, padded, lb.top, INPUT_SIZE - nh - lb.top, lb.left, INPUT_SIZE - nw - lb.left,
                        cv::BORDER_CONSTANT, cv::Scalar( 0, 0, 0 ) );

    padded.convertTo( input, CV_32FC3 );
    input -= cv::Scalar( 103.53, 116.28, 123.675 );
    cv::multiply( input, cv::Scalar( 1.0 / 57.375, 1.0 / 57.12, 1.0 / 58.395 ), input );
    return cv::dnn::blobFromImage( input );
}

/* Decode one output scale: per anchor, best class score, then box sides from
 * the softmax-expected bin of each side's distribution. */
void prvDecodeLevel( const cv::Mat & cls, const cv::Mat & reg, int stride,
                     std::vector<cv::Rect2d> & boxes, std::vector<float> & scores, std::vector<int> & ids )
{
    const int rows = cls.size[ 1 ];
    const int fw = INPUT_SIZE / stride;
    const float * c = cls.ptr<float>();
    const float * r = reg.ptr<float>();

    for( int i = 0; i < rows; i++ )
    {
        const float * row = c + static_cast<size_t>( i ) * NUM_CLASSES;
        const int best = static_cast<int>( std::max_element( row, row + NUM_CLASSES ) - row );

        if( row[ best ] < SCORE_THRESHOLD )
        {
            continue;
        }

        const double ax = ( i % fw ) * stride + 0.5 * ( stride - 1 );
        const double ay = ( i / fw ) * stride + 0.5 * ( stride - 1 );
        double side[ 4 ];

        for( int k = 0; k < 4; k++ )
        {
            const float * bins = r + static_cast<size_t>( i ) * 4 * ( REG_MAX + 1 ) + k * ( REG_MAX + 1 );
            const float peak = *std::max_element( bins, bins + REG_MAX + 1 );
            double sum = 0.0;
            double expect = 0.0;

            for( int b = 0; b <= REG_MAX; b++ )
            {
                const double e = std::exp( bins[ b ] - peak );
                sum += e;
                expect += e * b;
            }

            side[ k ] = expect / sum * stride;
        }

        const double x1 = std::clamp( ax - side[ 0 ], 0.0, static_cast<double>( INPUT_SIZE ) );
        const double y1 = std::clamp( ay - side[ 1 ], 0.0, static_cast<double>( INPUT_SIZE ) );
        const double x2 = std::clamp( ax + side[ 2 ], 0.0, static_cast<double>( INPUT_SIZE ) );
        const double y2 = std::clamp( ay + side[ 3 ], 0.0, static_cast<double>( INPUT_SIZE ) );

        boxes.emplace_back( x1, y1, x2 - x1, y2 - y1 );
        scores.push_back( row[ best ] );
        ids.push_back( best );
    }
}

void prvRunJob( uint32_t seq, const hal_roi_t & roi, npu_result_t & res )
{
    cv::Mat color;
    Letterbox lb;
    std::vector<cv::Mat> outs;
    std::vector<cv::Rect2d> boxes;
    std::vector<float> scores;
    std::vector<int> ids;
    std::vector<int> keep;

    res = npu_result_t {};
    res.seq = seq;

    if( !hal_video_mem_get( seq, color ) )
    {
        res.status = NPU_ERR_FRAME_GONE;
        return;
    }

    g_net.setInput( prvPrepare( color, roi, lb ) );
    g_net.forward( outs, g_out_names );

    /* The model has class-score outputs (N x 80) and box outputs (N x 32),
     * one pair per scale. Pair them by N rather than trusting output order. */
    for( const cv::Mat & cls : outs )
    {
        if( ( cls.dims != 3 ) || ( cls.size[ 2 ] != NUM_CLASSES ) )
        {
            continue;
        }

        for( const cv::Mat & reg : outs )
        {
            if( ( reg.dims == 3 ) && ( reg.size[ 2 ] == 4 * ( REG_MAX + 1 ) ) && ( reg.size[ 1 ] == cls.size[ 1 ] ) )
            {
                const int stride = INPUT_SIZE / static_cast<int>( std::lround( std::sqrt( cls.size[ 1 ] ) ) );
                prvDecodeLevel( cls, reg, stride, boxes, scores, ids );
            }
        }
    }

    cv::dnn::NMSBoxes( boxes, scores, SCORE_THRESHOLD, NMS_THRESHOLD, keep );

    /* Map boxes back: model input -> crop -> video memory -> firmware frame. */
    const double fx = FRAME_W / static_cast<double>( color.cols );
    const double fy = FRAME_H / static_cast<double>( color.rows );

    for( int k : keep )
    {
        if( res.n >= NPU_MAX_DETS )
        {
            break;
        }

        const cv::Rect2d & b = boxes[ k ];
        const double x = ( ( b.x - lb.left ) / lb.scale + lb.crop.x ) * fx;
        const double y = ( ( b.y - lb.top ) / lb.scale + lb.crop.y ) * fy;
        const double w = b.width / lb.scale * fx;
        const double h = b.height / lb.scale * fy;
        npu_det_t & d = res.det[ res.n++ ];

        d.coco_class = static_cast<uint8_t>( ids[ k ] );
        d.score = scores[ k ];
        d.box.x = static_cast<uint16_t>( std::clamp( x, 0.0, FRAME_W - 1.0 ) );
        d.box.y = static_cast<uint16_t>( std::clamp( y, 0.0, FRAME_H - 1.0 ) );
        d.box.w = static_cast<uint16_t>( std::clamp( w, 1.0, static_cast<double>( FRAME_W ) - d.box.x ) );
        d.box.h = static_cast<uint16_t>( std::clamp( h, 1.0, static_cast<double>( FRAME_H ) - d.box.y ) );
    }

    res.status = NPU_OK;
}

void npu_thread()
{
    for( ; ; )
    {
        uint32_t seq;
        uint32_t generation;
        hal_roi_t roi;
        npu_result_t res;

        {
            std::unique_lock<std::mutex> lock( g_mu );
            g_cv.wait( lock, [] { return g_has_job; } );
            g_has_job = false;
            seq = g_job_seq;
            roi = g_job_roi;
            generation = g_generation;

            if( g_hang_next )
            {
                /* Injected fault: stuck until someone resets the NPU. */
                g_hang_next = false;
                std::printf( "[hw] npu: hung on frame %u\n", static_cast<unsigned>( seq ) );
                std::fflush( stdout );
                g_cv.wait( lock, [ generation ] { return g_generation != generation; } );
                std::printf( "[hw] npu: reset, job for frame %u abandoned\n", static_cast<unsigned>( seq ) );
                std::fflush( stdout );
                continue;
            }
        }

        const auto start = std::chrono::steady_clock::now();

        try
        {
            prvRunJob( seq, roi, res );
        }
        catch( const cv::Exception & e )
        {
            std::printf( "[hw] npu: job for frame %u failed: %s\n", static_cast<unsigned>( seq ), e.what() );
            std::fflush( stdout );
            res = npu_result_t {};
            res.seq = seq;
            res.status = NPU_ERR_MODEL;
        }

        const auto host = std::chrono::steady_clock::now() - start;
        const auto device = std::max<std::chrono::steady_clock::duration>( host, std::chrono::milliseconds( g_latency_ms ) );

        /* Emulate the device NPU: the job is not done before its latency. */
        std::this_thread::sleep_until( start + device );

        res.host_us = static_cast<uint32_t>( std::chrono::duration_cast<std::chrono::microseconds>( host ).count() );
        res.device_us = static_cast<uint32_t>( std::chrono::duration_cast<std::chrono::microseconds>( device ).count() );

        {
            std::lock_guard<std::mutex> lock( g_mu );

            if( generation != g_generation )
            {
                continue; /* The NPU was reset while this job ran: no result, no IRQ. */
            }

            g_result = res;
            g_busy = false;
        }

        vPortGenerateSimulatedInterruptFromWindowsThread( IRQ_NPU_DONE );
    }
}
} // namespace

extern "C" int hal_npu_init( const char * model_path, uint32_t device_latency_ms )
{
    try
    {
        g_net = cv::dnn::readNet( model_path );
        g_out_names = g_net.getUnconnectedOutLayersNames();
    }
    catch( const cv::Exception & e )
    {
        std::printf( "[hw] npu: cannot load model '%s': %s\n", model_path, e.what() );
        return -1;
    }

    g_latency_ms = device_latency_ms;
    std::printf( "[hw] npu: loaded '%s', device latency %u ms\n", model_path, static_cast<unsigned>( device_latency_ms ) );
    std::thread( npu_thread ).detach();
    return 0;
}

extern "C" int hal_npu_submit( uint32_t seq, const hal_roi_t * roi )
{
    {
        std::lock_guard<std::mutex> lock( g_mu );

        if( g_busy )
        {
            return -1;
        }

        g_busy = true;
        g_has_job = true;
        g_job_seq = seq;
        g_job_roi = *roi;
    }

    g_cv.notify_one();
    return 0;
}

extern "C" void hal_npu_get_result( npu_result_t * out )
{
    std::lock_guard<std::mutex> lock( g_mu );
    *out = g_result;
}

extern "C" void hal_npu_reset( void )
{
    {
        std::lock_guard<std::mutex> lock( g_mu );
        g_generation++;
        g_busy = false;
        g_has_job = false;
    }

    g_cv.notify_all();
}

extern "C" void hal_npu_fault_hang( void )
{
    std::lock_guard<std::mutex> lock( g_mu );
    g_hang_next = true;
}
