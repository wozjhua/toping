#include "audio_receiver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mirror_net.h"

std::atomic<int> g_audioVolumePercent{DEFAULT_AUDIO_VOLUME_PERCENT};

class PcmWaveOutPlayer {
public:
    ~PcmWaveOutPlayer() { close(); }

    bool open() {
        if (waveOut_) return true;

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = 48000;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        MMRESULT r = waveOutOpen(&waveOut_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR || waveOut_ == nullptr) return false;
        applyVolume(true);
        return true;
    }

    void close() {
        if (!waveOut_) return;
        waveOutReset(waveOut_);
        cleanupDone(true);
        waveOutClose(waveOut_);
        waveOut_ = nullptr;
        blocks_.clear();
    }

    void submit(const uint8_t* data, size_t size) {
        if (!data || size == 0) return;
        if (!open()) return;
        applyVolume(false);

        cleanupDone(false);

        // Keep latency bounded, but do not reset so aggressively that normal
        // scheduling jitter turns into audible crackle.  With 20ms Android
        // packets, five queued blocks are about 100ms of PC-side audio.
        if (blocks_.size() >= kMaxQueuedBlocks) {
            waveOutReset(waveOut_);
            cleanupDone(true);
        }

        auto block = std::make_unique<Block>();
        block->data.resize(size);
        std::memcpy(block->data.data(), data, size);
        std::memset(&block->hdr, 0, sizeof(block->hdr));
        block->hdr.lpData = block->data.data();
        block->hdr.dwBufferLength = static_cast<DWORD>(block->data.size());

        if (waveOutPrepareHeader(waveOut_, &block->hdr, sizeof(block->hdr)) != MMSYSERR_NOERROR) {
            return;
        }
        block->prepared = true;

        if (waveOutWrite(waveOut_, &block->hdr, sizeof(block->hdr)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(waveOut_, &block->hdr, sizeof(block->hdr));
            return;
        }

        blocks_.push_back(std::move(block));
    }

private:
    struct Block {
        WAVEHDR hdr{};
        std::vector<char> data;
        bool prepared = false;
    };

    static constexpr size_t kMaxQueuedBlocks = 5;
    HWAVEOUT waveOut_ = nullptr;
    std::deque<std::unique_ptr<Block>> blocks_;
    int lastVolumePercent_ = -1;

    void applyVolume(bool force) {
        if (!waveOut_) return;
        int volume = g_audioVolumePercent.load(std::memory_order_acquire);
        volume = (std::max)(0, (std::min)(100, volume));
        if (!force && volume == lastVolumePercent_) return;
        const DWORD v = static_cast<DWORD>((volume * 0xFFFF) / 100);
        const DWORD stereo = (v & 0xFFFFu) | (v << 16);
        waveOutSetVolume(waveOut_, stereo);
        lastVolumePercent_ = volume;
    }

    void cleanupDone(bool force) {
        while (!blocks_.empty()) {
            auto& b = blocks_.front();
            if (!force && (b->hdr.dwFlags & WHDR_DONE) == 0) break;
            if (b->prepared) {
                waveOutUnprepareHeader(waveOut_, &b->hdr, sizeof(b->hdr));
                b->prepared = false;
            }
            blocks_.pop_front();
        }
    }
};

class AudioReceiver::Impl {
public:
    explicit Impl(SharedState& state) : state_(state) {}
    ~Impl() { stop(); }

    void start() {
        stopped_.store(false);
        audioThread_ = std::thread([this] { runAudio(); });
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        closesocket(sock_);
        if (audioThread_.joinable()) audioThread_.join();
    }

private:
    SharedState& state_;
    std::thread audioThread_;
    std::atomic<bool> stopped_{false};
    SOCKET sock_ = INVALID_SOCKET;

    void runAudio() {
        std::vector<uint8_t> lenBuf(4);
        std::vector<uint8_t> pcm;
        PcmWaveOutPlayer player;

        while (!stopped_.load() && !state_.stop.load()) {
            const AdbSetupResult adb = RunAdbAudioForward();
            if (!adb.ok) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            BOOL noDelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            int audioRcvBuf = 32 * 1024;
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&audioRcvBuf), sizeof(audioRcvBuf));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(AUDIO_PORT);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");

            if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                closesocket(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            sock_ = s;
            player.open();

            while (!stopped_.load() && !state_.stop.load()) {
                if (!RecvAll(s, lenBuf.data(), 4)) break;
                const int32_t n = ReadBE32(lenBuf.data());
                if (n <= 0 || n > 256 * 1024) break;
                pcm.resize(static_cast<size_t>(n));
                if (!RecvAll(s, pcm.data(), n)) break;
                player.submit(pcm.data(), pcm.size());
            }

            closesocket(s);
            sock_ = INVALID_SOCKET;
            player.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        player.close();
    }
};





AudioReceiver::AudioReceiver(SharedState& state)
    : impl_(std::make_unique<Impl>(state)) {}

AudioReceiver::~AudioReceiver() = default;

void AudioReceiver::start() {
    impl_->start();
}

void AudioReceiver::stop() {
    impl_->stop();
}
