#include <windows.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#define MINIMP3_IMPLEMENTATION
#pragma warning(push, 0)
#include <minimp3/minimp3_ex.h>
#pragma warning(pop)

namespace {
class Player {
public:
    ~Player() {
        resetVoice();
        if (master) master->DestroyVoice();
        mp3dec_ex_close(&decoder);
    }

    bool open(const wchar_t* path) {
        if (mp3dec_ex_open_w(&decoder, path, MP3D_SEEK_TO_SAMPLE) || !decoder.samples ||
            decoder.info.hz <= 0 || (decoder.info.channels != 1 && decoder.info.channels != 2))
            return false;
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(decoder.info.channels);
        format.nSamplesPerSec = decoder.info.hz;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * sizeof(int16_t);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        // Build the VBR frame index during the background open, even when a Xing tag gave us the duration.
        if (mp3dec_ex_seek(&decoder, format.nChannels)) return false;
        frames = decoder.samples / format.nChannels;
        for (auto& buffer : buffers)
            buffer.resize(format.nSamplesPerSec / 5 * format.nChannels);
        return seek(0);
    }

    double duration() const { return double(frames) / format.nSamplesPerSec; }

    double position() const {
        XAUDIO2_VOICE_STATE state{};
        if (voice) voice->GetState(&state);
        return double(std::min(frames, start + state.SamplesPlayed)) / format.nSamplesPerSec;
    }

    bool seek(double seconds) {
        const auto target = uint64_t(std::llround(std::clamp(seconds, 0.0, duration()) * format.nSamplesPerSec));
        if (mp3dec_ex_seek(&decoder, target * format.nChannels)) return false;
        resetVoice();
        start = target;
        submitted = target;
        next = 0;
        return true;
    }

    bool setPlaying(bool value) {
        if (!value) return !voice || SUCCEEDED(voice->Stop());
        if (!engine) {
            if (FAILED(XAudio2Create(&engine)) || FAILED(engine->CreateMasteringVoice(&master))) {
                master = nullptr;
                engine.Reset();
                return false;
            }
        }
        if (!voice && FAILED(engine->CreateSourceVoice(&voice, &format, XAUDIO2_VOICE_NOPITCH))) return false;
        return SUCCEEDED(voice->SetVolume(volume)) && refill() && SUCCEEDED(voice->Start());
    }

    bool refill() {
        if (!voice) return true;
        XAUDIO2_VOICE_STATE state{};
        voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (state.BuffersQueued < buffers.size() && submitted < frames) {
            auto& pcm = buffers[next];
            const auto count = mp3dec_ex_read(&decoder, pcm.data(), pcm.size());
            if (!count || count % format.nChannels != 0) return false;
            XAUDIO2_BUFFER buffer{};
            // Keep the sample counter continuous, including after the last buffer drains.
            buffer.AudioBytes = static_cast<UINT32>(count * sizeof(int16_t));
            buffer.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());
            if (FAILED(voice->SubmitSourceBuffer(&buffer))) return false;
            submitted += count / format.nChannels;
            next = (next + 1) % buffers.size();
            ++state.BuffersQueued;
        }
        return true;
    }

    void setVolume(float value) {
        volume = value;
        if (voice) voice->SetVolume(value);
    }

private:
    void resetVoice() {
        if (voice) voice->DestroyVoice();
        voice = nullptr;
    }

    mp3dec_ex_t decoder{};
    WAVEFORMATEX format{};
    Microsoft::WRL::ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* master{};
    IXAudio2SourceVoice* voice{};
    std::array<std::vector<int16_t>, 5> buffers;
    size_t next = 0;
    uint64_t frames = 0, start = 0, submitted = 0;
    float volume = 0.75f;
};
}

#define AUDIO_API extern "C" __declspec(dllexport)
AUDIO_API Player* BookOpen(const wchar_t* path) noexcept {
    try {
        auto player = std::make_unique<Player>();
        return player->open(path) ? player.release() : nullptr;
    } catch (...) { return nullptr; }
}
AUDIO_API void BookClose(Player* player) { delete player; }
AUDIO_API double BookDuration(Player* player) { return player->duration(); }
AUDIO_API double BookPosition(Player* player) { return player->position(); }
AUDIO_API int BookSeek(Player* player, double seconds) { return player->seek(seconds); }
AUDIO_API int BookPlaying(Player* player, int value) { return player->setPlaying(value != 0); }
AUDIO_API int BookPump(Player* player) { return player->refill(); }
AUDIO_API void BookVolume(Player* player, float value) { player->setVolume(value); }
