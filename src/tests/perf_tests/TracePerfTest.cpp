//
// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// TracePerf:
//   Performance test for ANGLE replaying traces.
//

#include <gtest/gtest.h>
#include "common/PackedEnums.h"
#include "common/system_utils.h"
#include "tests/perf_tests/ANGLEPerfTest.h"
#include "tests/perf_tests/DrawCallPerfParams.h"
#include "util/egl_loader_autogen.h"
#include "util/frame_capture_test_utils.h"
#include "util/png_utils.h"

#include "restricted_traces/restricted_traces_autogen.h"

#include <cassert>
#include <functional>
#include <sstream>

using namespace angle;
using namespace egl_platform;

namespace
{
struct TracePerfParams final : public RenderTestParams
{
    // Common default options
    TracePerfParams()
    {
        majorVersion = 3;
        minorVersion = 0;

        // Tracking GPU time adds overhead to native traces. http://anglebug.com/4879
        trackGpuTime = false;

        // Display the frame after every drawBenchmark invocation
        iterationsPerStep = 1;
    }

    std::string story() const override
    {
        std::stringstream strstr;
        strstr << RenderTestParams::story() << "_" << GetTraceInfo(testID).name;
        return strstr.str();
    }

    RestrictedTraceID testID;
};

std::ostream &operator<<(std::ostream &os, const TracePerfParams &params)
{
    os << params.backendAndStory().substr(1);
    return os;
}

class TracePerfTest : public ANGLERenderTest, public ::testing::WithParamInterface<TracePerfParams>
{
  public:
    TracePerfTest();

    void initializeBenchmark() override;
    void destroyBenchmark() override;
    void drawBenchmark() override;

    void onReplayFramebufferChange(GLenum target, GLuint framebuffer);

    uint32_t mStartFrame;
    uint32_t mEndFrame;

    double getHostTimeFromGLTime(GLint64 glTime);

  private:
    struct QueryInfo
    {
        GLuint beginTimestampQuery;
        GLuint endTimestampQuery;
        GLuint framebuffer;
    };

    struct TimeSample
    {
        GLint64 glTime;
        double hostTime;
    };

    void sampleTime();
    void saveScreenshot(const std::string &screenshotName) override;

    // For tracking RenderPass/FBO change timing.
    QueryInfo mCurrentQuery = {};
    std::vector<QueryInfo> mRunningQueries;
    std::vector<TimeSample> mTimeline;

    std::string mStartingDirectory;
    bool mUseTimestampQueries = false;
};

class TracePerfTest;
TracePerfTest *gCurrentTracePerfTest = nullptr;

// Don't forget to include KHRONOS_APIENTRY in override methods. Neccessary on Win/x86.
void KHRONOS_APIENTRY BindFramebufferProc(GLenum target, GLuint framebuffer)
{
    glBindFramebuffer(target, framebuffer);
    gCurrentTracePerfTest->onReplayFramebufferChange(target, framebuffer);
}

angle::GenericProc KHRONOS_APIENTRY TraceLoadProc(const char *procName)
{
    if (strcmp(procName, "glBindFramebuffer") == 0)
    {
        return reinterpret_cast<angle::GenericProc>(BindFramebufferProc);
    }
    return gCurrentTracePerfTest->getGLWindow()->getProcAddress(procName);
}

TracePerfTest::TracePerfTest()
    : ANGLERenderTest("TracePerf", GetParam(), "ms"), mStartFrame(0), mEndFrame(0)
{
    const TracePerfParams &param = GetParam();

    // TODO: http://anglebug.com/4533 This fails after the upgrade to the 26.20.100.7870 driver.
    if (IsWindows() && IsIntel() && param.getRenderer() == EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE &&
        param.testID == RestrictedTraceID::manhattan_10)
    {
        mSkipTest = true;
    }

    // TODO: http://anglebug.com/4731 Fails on older Intel drivers. Passes in newer.
    if (IsWindows() && IsIntel() && param.driver != GLESDriverType::AngleEGL &&
        param.testID == RestrictedTraceID::angry_birds_2_1500)
    {
        mSkipTest = true;
    }

    if (param.testID == RestrictedTraceID::cod_mobile)
    {
        // TODO: http://anglebug.com/4967 Vulkan: GL_EXT_color_buffer_float not supported on Pixel 2
        // The COD:Mobile trace uses a framebuffer attachment with:
        //   format = GL_RGB
        //   type = GL_UNSIGNED_INT_10F_11F_11F_REV
        // That combination is only renderable if GL_EXT_color_buffer_float is supported.
        // It happens to not be supported on Pixel 2's Vulkan driver.
        addExtensionPrerequisite("GL_EXT_color_buffer_float");

        // TODO: http://anglebug.com/4731 This extension is missing on older Intel drivers.
        addExtensionPrerequisite("GL_OES_EGL_image_external");
    }

    // We already swap in TracePerfTest::drawBenchmark, no need to swap again in the harness.
    disableTestHarnessSwap();

    gCurrentTracePerfTest = this;
}

void TracePerfTest::initializeBenchmark()
{
    const auto &params = GetParam();

    mStartingDirectory = angle::GetCWD().value();

    // To load the trace data path correctly we set the CWD to the executable dir.
    if (!IsAndroid())
    {
        std::string exeDir = angle::GetExecutableDirectory();
        angle::SetCWD(exeDir.c_str());
    }

    trace_angle::LoadGLES(TraceLoadProc);

    const TraceInfo &traceInfo = GetTraceInfo(params.testID);
    mStartFrame                = traceInfo.startFrame;
    mEndFrame                  = traceInfo.endFrame;
    SetBinaryDataDecompressCallback(params.testID, DecompressBinaryData);

    setStepsPerRunLoopStep(mEndFrame - mStartFrame + 1);

    std::stringstream testDataDirStr;
    testDataDirStr << ANGLE_TRACE_DATA_DIR << "/" << traceInfo.name;
    std::string testDataDir = testDataDirStr.str();
    SetBinaryDataDir(params.testID, testDataDir.c_str());

    if (IsAndroid())
    {
        // On Android, set the orientation used by the app, based on width/height
        getWindow()->setOrientation(mTestParams.windowWidth, mTestParams.windowHeight);
    }

    // Potentially slow. Can load a lot of resources.
    SetupReplay(params.testID);
    glFinish();

    ASSERT_TRUE(mEndFrame > mStartFrame);

    getWindow()->setVisible(true);
}

#undef TRACE_TEST_CASE

void TracePerfTest::destroyBenchmark()
{
    // In order for the next test to load, restore the working directory
    angle::SetCWD(mStartingDirectory.c_str());
}

void TracePerfTest::sampleTime()
{
    if (mUseTimestampQueries)
    {
        GLint64 glTime;
        // glGetInteger64vEXT is exported by newer versions of the timer query extensions.
        // Unfortunately only the core EP is exposed by some desktop drivers (e.g. NVIDIA).
        if (glGetInteger64vEXT)
        {
            glGetInteger64vEXT(GL_TIMESTAMP_EXT, &glTime);
        }
        else
        {
            glGetInteger64v(GL_TIMESTAMP_EXT, &glTime);
        }
        mTimeline.push_back({glTime, angle::GetHostTimeSeconds()});
    }
}

void TracePerfTest::drawBenchmark()
{
    // Add a time sample from GL and the host.
    sampleTime();

    startGpuTimer();

    for (uint32_t frame = mStartFrame; frame <= mEndFrame; ++frame)
    {
        char frameName[32];
        sprintf(frameName, "Frame %u", frame);
        beginInternalTraceEvent(frameName);

        ReplayFrame(GetParam().testID, frame);
        getGLWindow()->swap();

        endInternalTraceEvent(frameName);

        // Check for abnormal exit.
        if (!mRunning)
        {
            return;
        }
    }

    ResetReplay(GetParam().testID);

    // Process any running queries once per iteration.
    for (size_t queryIndex = 0; queryIndex < mRunningQueries.size();)
    {
        const QueryInfo &query = mRunningQueries[queryIndex];

        GLuint endResultAvailable = 0;
        glGetQueryObjectuivEXT(query.endTimestampQuery, GL_QUERY_RESULT_AVAILABLE,
                               &endResultAvailable);

        if (endResultAvailable == GL_TRUE)
        {
            char fboName[32];
            sprintf(fboName, "FBO %u", query.framebuffer);

            GLint64 beginTimestamp = 0;
            glGetQueryObjecti64vEXT(query.beginTimestampQuery, GL_QUERY_RESULT, &beginTimestamp);
            glDeleteQueriesEXT(1, &query.beginTimestampQuery);
            double beginHostTime = getHostTimeFromGLTime(beginTimestamp);
            beginGLTraceEvent(fboName, beginHostTime);

            GLint64 endTimestamp = 0;
            glGetQueryObjecti64vEXT(query.endTimestampQuery, GL_QUERY_RESULT, &endTimestamp);
            glDeleteQueriesEXT(1, &query.endTimestampQuery);
            double endHostTime = getHostTimeFromGLTime(endTimestamp);
            endGLTraceEvent(fboName, endHostTime);

            mRunningQueries.erase(mRunningQueries.begin() + queryIndex);
        }
        else
        {
            queryIndex++;
        }
    }

    stopGpuTimer();
}

// Converts a GL timestamp into a host-side CPU time aligned with "GetHostTimeSeconds".
// This check is necessary to line up sampled trace events in a consistent timeline.
// Uses a linear interpolation from a series of samples. We do a blocking call to sample
// both host and GL time once per swap. We then find the two closest GL timestamps and
// interpolate the host times between them to compute our result. If we are past the last
// GL timestamp we sample a new data point pair.
double TracePerfTest::getHostTimeFromGLTime(GLint64 glTime)
{
    // Find two samples to do a lerp.
    size_t firstSampleIndex = mTimeline.size() - 1;
    while (firstSampleIndex > 0)
    {
        if (mTimeline[firstSampleIndex].glTime < glTime)
        {
            break;
        }
        firstSampleIndex--;
    }

    // Add an extra sample if we're missing an ending sample.
    if (firstSampleIndex == mTimeline.size() - 1)
    {
        sampleTime();
    }

    const TimeSample &start = mTimeline[firstSampleIndex];
    const TimeSample &end   = mTimeline[firstSampleIndex + 1];

    // Note: we have observed in some odd cases later timestamps producing values that are
    // smaller than preceding timestamps. This bears further investigation.

    // Compute the scaling factor for the lerp.
    double glDelta = static_cast<double>(glTime - start.glTime);
    double glRange = static_cast<double>(end.glTime - start.glTime);
    double t       = glDelta / glRange;

    // Lerp(t1, t2, t)
    double hostRange = end.hostTime - start.hostTime;
    return mTimeline[firstSampleIndex].hostTime + hostRange * t;
}

// Triggered when the replay calls glBindFramebuffer.
void TracePerfTest::onReplayFramebufferChange(GLenum target, GLuint framebuffer)
{
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER)
        return;

    if (!mUseTimestampQueries)
        return;

    // We have at most one active timestamp query at a time. This code will end the current
    // query and immediately start a new one.
    if (mCurrentQuery.beginTimestampQuery != 0)
    {
        glGenQueriesEXT(1, &mCurrentQuery.endTimestampQuery);
        glQueryCounterEXT(mCurrentQuery.endTimestampQuery, GL_TIMESTAMP_EXT);
        mRunningQueries.push_back(mCurrentQuery);
        mCurrentQuery = {};
    }

    ASSERT(mCurrentQuery.beginTimestampQuery == 0);

    glGenQueriesEXT(1, &mCurrentQuery.beginTimestampQuery);
    glQueryCounterEXT(mCurrentQuery.beginTimestampQuery, GL_TIMESTAMP_EXT);
    mCurrentQuery.framebuffer = framebuffer;
}

void TracePerfTest::saveScreenshot(const std::string &screenshotName)
{
    // Render a single frame.
    RestrictedTraceID testID   = GetParam().testID;
    const TraceInfo &traceInfo = GetTraceInfo(testID);
    ReplayFrame(testID, traceInfo.startFrame);

    // RGBA 4-byte data.
    uint32_t pixelCount = mTestParams.windowWidth * mTestParams.windowHeight;
    std::vector<uint8_t> pixelData(pixelCount * 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, mTestParams.windowWidth, mTestParams.windowHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixelData.data());

    // Convert to RGB and flip y.
    std::vector<uint8_t> rgbData(pixelCount * 3);
    for (EGLint y = 0; y < mTestParams.windowHeight; ++y)
    {
        for (EGLint x = 0; x < mTestParams.windowWidth; ++x)
        {
            EGLint srcPixel = x + y * mTestParams.windowWidth;
            EGLint dstPixel = x + (mTestParams.windowHeight - y - 1) * mTestParams.windowWidth;
            memcpy(&rgbData[dstPixel * 3], &pixelData[srcPixel * 4], 3);
        }
    }

    if (!angle::SavePNGRGB(screenshotName.c_str(), "ANGLE Screenshot", mTestParams.windowWidth,
                           mTestParams.windowHeight, rgbData))
    {
        FAIL() << "Error saving screenshot: " << screenshotName;
    }
    else
    {
        printf("Saved screenshot: '%s'\n", screenshotName.c_str());
    }

    // Finish the frame loop.
    for (uint32_t nextFrame = traceInfo.startFrame + 1; nextFrame <= traceInfo.endFrame;
         ++nextFrame)
    {
        ReplayFrame(testID, nextFrame);
    }
    ResetReplay(testID);
    getGLWindow()->swap();
    glFinish();
}

TEST_P(TracePerfTest, Run)
{
    run();
}

TracePerfParams CombineTestID(const TracePerfParams &in, RestrictedTraceID id)
{
    const TraceInfo &traceInfo = GetTraceInfo(id);

    TracePerfParams out = in;
    out.testID          = id;
    out.windowWidth     = traceInfo.drawSurfaceWidth;
    out.windowHeight    = traceInfo.drawSurfaceHeight;
    return out;
}

bool NoAndroidMockICD(const TracePerfParams &in)
{
    return in.eglParameters.deviceType != EGL_PLATFORM_ANGLE_DEVICE_TYPE_NULL_ANGLE || !IsAndroid();
}

using namespace params;
using P = TracePerfParams;

std::vector<P> gTestsWithID =
    CombineWithValues({P()}, AllEnums<RestrictedTraceID>(), CombineTestID);
std::vector<P> gTestsWithRenderer =
    CombineWithFuncs(gTestsWithID, {Vulkan<P>, VulkanMockICD<P>, Native<P>});
std::vector<P> gTestsWithoutMockICD = FilterWithFunc(gTestsWithRenderer, NoAndroidMockICD);
ANGLE_INSTANTIATE_TEST_ARRAY(TracePerfTest, gTestsWithoutMockICD);

}  // anonymous namespace
