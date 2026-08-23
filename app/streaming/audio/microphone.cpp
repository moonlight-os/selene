#include "microphone.h"

#include <array>

#include <Limelight.h>

namespace {
constexpr int SampleRate = 48000;
constexpr int Channels = 1;
constexpr int FrameSamples = 960; // 20 ms
constexpr int Bitrate = 24000;
}

MicrophoneCapturer::MicrophoneCapturer() = default;

MicrophoneCapturer::~MicrophoneCapturer()
{
    m_Stopping = true;
    if (m_Device != 0) {
        SDL_PauseAudioDevice(m_Device, 1);
    }
    if (m_Worker.joinable()) {
        m_Worker.join();
    }
    if (m_Device != 0) {
        SDL_CloseAudioDevice(m_Device);
    }
    if (m_Encoder != nullptr) {
        opus_encoder_destroy(m_Encoder);
    }
}

bool MicrophoneCapturer::start()
{
    if (!(LiGetHostFeatureFlags() & LI_FF_MICROPHONE_UPLINK) ||
        LiGetPeerFeatureVersion(ML_FEATURE_MICROPHONE) == 0) {
        return false;
    }

    int error = OPUS_OK;
    m_Encoder = opus_encoder_create(SampleRate, Channels, OPUS_APPLICATION_VOIP, &error);
    if (m_Encoder == nullptr || error != OPUS_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Failed to create microphone Opus encoder: %s",
                     opus_strerror(error));
        return false;
    }
    opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(Bitrate));
    opus_encoder_ctl(m_Encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_Encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(m_Encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_Encoder, OPUS_SET_PACKET_LOSS_PERC(10));

    SDL_AudioSpec wanted, obtained;
    SDL_zero(wanted);
    wanted.freq = SampleRate;
    wanted.format = AUDIO_F32SYS;
    wanted.channels = Channels;
    wanted.samples = FrameSamples;
    m_Device = SDL_OpenAudioDevice(nullptr, 1, &wanted, &obtained, 0);
    if (m_Device == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Microphone is unavailable: %s", SDL_GetError());
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO, "Microphone uplink started: %d Hz, mono, 20 ms Opus",
                obtained.freq);
    SDL_PauseAudioDevice(m_Device, 0);
    m_Worker = std::thread(&MicrophoneCapturer::run, this);
    return true;
}

void MicrophoneCapturer::run()
{
    std::array<float, FrameSamples> pcm;
    std::array<unsigned char, 1200> encoded;
    const auto frameBytes = static_cast<Uint32>(pcm.size() * sizeof(float));

    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    while (!m_Stopping) {
        if (SDL_GetQueuedAudioSize(m_Device) < frameBytes) {
            SDL_Delay(2);
            continue;
        }
        if (SDL_DequeueAudio(m_Device, pcm.data(), frameBytes) != frameBytes) {
            continue;
        }
        const int length = opus_encode_float(m_Encoder, pcm.data(), FrameSamples,
                                             encoded.data(), encoded.size());
        if (length < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Microphone Opus encode failed: %s",
                        opus_strerror(length));
            continue;
        }
        if (LiSendMicrophoneOpus(encoded.data(), static_cast<std::uint16_t>(length),
                                 m_Timestamp, FrameSamples, Channels) != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Microphone uplink send failed");
            SDL_Delay(20);
        }
        m_Timestamp += FrameSamples;
    }
}
