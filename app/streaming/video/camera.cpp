#include "camera.h"

#include <Limelight.h>
#include <SDL.h>

#include <string>
#include <fstream>
#include <iterator>

namespace {
bool jpegDimensions(const std::vector<std::uint8_t>& jpeg, std::uint16_t& width, std::uint16_t& height)
{
    if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return false;
    std::size_t offset = 2;
    while (offset + 4 <= jpeg.size()) {
        if (jpeg[offset++] != 0xff) continue;
        while (offset < jpeg.size() && jpeg[offset] == 0xff) ++offset;
        if (offset >= jpeg.size()) return false;
        const auto marker = jpeg[offset++];
        if (marker == 0xd9 || marker == 0xda) return false;
        if (offset + 2 > jpeg.size()) return false;
        const auto length = static_cast<std::size_t>((jpeg[offset] << 8) | jpeg[offset + 1]);
        if (length < 2 || offset + length > jpeg.size()) return false;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
            if (length < 7) return false;
            height = static_cast<std::uint16_t>((jpeg[offset + 3] << 8) | jpeg[offset + 4]);
            width = static_cast<std::uint16_t>((jpeg[offset + 5] << 8) | jpeg[offset + 6]);
            return width != 0 && height != 0;
        }
        offset += length;
    }
    return false;
}
}

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

CameraCapturer::CameraCapturer() = default;

CameraCapturer::~CameraCapturer()
{
    m_Stopping = true;
    if (m_Worker.joinable()) {
        m_Worker.join();
    }
#ifdef __linux__
    if (m_Fd >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(m_Fd, VIDIOC_STREAMOFF, &type);
    }
    for (const auto& buffer : m_Buffers) {
        if (buffer.data != MAP_FAILED && buffer.data != nullptr) {
            munmap(buffer.data, buffer.size);
        }
    }
    if (m_Fd >= 0) {
        close(m_Fd);
    }
#endif
}

bool CameraCapturer::start()
{
    if (!(LiGetHostFeatureFlags() & LI_FF_CAMERA_UPLINK) ||
        LiGetPeerFeatureVersion(ML_FEATURE_CAMERA) == 0) {
        return false;
    }
#ifndef __linux__
    return false;
#else
    if (const char* fixturePath = SDL_getenv("MOONLIGHT_CAMERA_TEST_MJPEG")) {
        std::ifstream fixture(fixturePath, std::ios::binary);
        m_SyntheticFrame.assign(std::istreambuf_iterator<char>(fixture), {});
        if (m_SyntheticFrame.empty() || m_SyntheticFrame.size() > 4 * 1024 * 1024 ||
            !jpegDimensions(m_SyntheticFrame, m_Width, m_Height)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Invalid synthetic camera MJPEG fixture");
            m_SyntheticFrame.clear();
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "Synthetic camera uplink started: %ux%u MJPEG",
                    m_Width, m_Height);
        m_Worker = std::thread(&CameraCapturer::run, this);
        return true;
    }

    for (int index = 0; index < 64 && m_Fd < 0; ++index) {
        const auto path = std::string("/dev/video") + std::to_string(index);
        const int candidate = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (candidate < 0) continue;
        v4l2_capability capability{};
        if (ioctl(candidate, VIDIOC_QUERYCAP, &capability) == 0) {
            const auto caps = capability.device_caps ? capability.device_caps : capability.capabilities;
            if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
                m_Fd = candidate;
                break;
            }
        }
        close(candidate);
    }
    if (m_Fd < 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "Camera uplink unavailable: no V4L2 capture device");
        return false;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = 1280;
    format.fmt.pix.height = 720;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(m_Fd, VIDIOC_S_FMT, &format) < 0 || format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Camera does not provide MJPEG: %s", std::strerror(errno));
        return false;
    }
    m_Width = static_cast<std::uint16_t>(format.fmt.pix.width);
    m_Height = static_cast<std::uint16_t>(format.fmt.pix.height);

    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(m_Fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Camera buffer allocation failed: %s", std::strerror(errno));
        return false;
    }

    m_Buffers.resize(request.count);
    for (std::uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer{};
        buffer.type = request.type;
        buffer.memory = request.memory;
        buffer.index = index;
        if (ioctl(m_Fd, VIDIOC_QUERYBUF, &buffer) < 0) return false;
        m_Buffers[index].size = buffer.length;
        m_Buffers[index].data = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, m_Fd, buffer.m.offset);
        if (m_Buffers[index].data == MAP_FAILED || ioctl(m_Fd, VIDIOC_QBUF, &buffer) < 0) return false;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_Fd, VIDIOC_STREAMON, &type) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Camera stream start failed: %s", std::strerror(errno));
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "Camera uplink started: %ux%u MJPEG", m_Width, m_Height);
    m_Worker = std::thread(&CameraCapturer::run, this);
    return true;
#endif
}

void CameraCapturer::run()
{
#ifdef __linux__
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    if (!m_SyntheticFrame.empty()) {
        while (!m_Stopping) {
            if (LiSendCameraMjpeg(m_SyntheticFrame.data(),
                                  static_cast<std::uint32_t>(m_SyntheticFrame.size()), m_FrameId++,
                                  SDL_GetTicks(), m_Width, m_Height) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Synthetic camera uplink send failed");
            }
            SDL_Delay(100);
        }
        return;
    }
    while (!m_Stopping) {
        pollfd descriptor{m_Fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 100);
        if (ready <= 0 || !(descriptor.revents & POLLIN)) continue;

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl(m_Fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Camera dequeue failed: %s", std::strerror(errno));
            }
            continue;
        }
        if (buffer.index < m_Buffers.size() && buffer.bytesused > 0 &&
            buffer.bytesused <= m_Buffers[buffer.index].size) {
            const auto timestamp = SDL_GetTicks();
            if (LiSendCameraMjpeg(m_Buffers[buffer.index].data, buffer.bytesused,
                                  m_FrameId++, timestamp, m_Width, m_Height) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Camera uplink send failed");
            }
        }
        if (ioctl(m_Fd, VIDIOC_QBUF, &buffer) < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Camera requeue failed: %s", std::strerror(errno));
            break;
        }
    }
#endif
}
