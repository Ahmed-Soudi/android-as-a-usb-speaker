#include <aaudio/AAudio.h>
#include <alsa/asoundlib.h>
#include <android/log.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <climits>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AndroidUsbSpeaker", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AndroidUsbSpeaker", __VA_ARGS__)

namespace {
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kPeriodFrames = 240;                 // 5 ms; matches observed Samsung media period.
constexpr size_t kRingFrames = 16384;             // ~341 ms safety capacity; normal target stays much lower.
constexpr size_t kRingSamples = kRingFrames * kChannels;
constexpr size_t kTargetBufferedFrames = 1920;     // ~40 ms guard; decouples USB and AAudio clocks.

struct Ring {
    alignas(64) std::atomic<uint64_t> w{0};
    alignas(64) std::atomic<uint64_t> r{0};
    int16_t data[kRingSamples]{};

    size_t framesAvailable() const {
        return static_cast<size_t>((w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire)) / kChannels);
    }

    void clear() {
        auto ww = w.load(std::memory_order_relaxed);
        r.store(ww, std::memory_order_release);
    }

    void push(const int16_t* src, size_t frames) {
        uint64_t ww = w.load(std::memory_order_relaxed);
        uint64_t rr = r.load(std::memory_order_acquire);
        const uint64_t incoming = frames * kChannels;
        const uint64_t capacity = kRingSamples;
        if ((ww - rr) + incoming > capacity) {
            // Never let stale audio increase latency: discard oldest samples first.
            rr = ww + incoming - capacity;
            r.store(rr, std::memory_order_release);
        }
        for (uint64_t i = 0; i < incoming; ++i) data[(ww + i) % capacity] = src[i];
        w.store(ww + incoming, std::memory_order_release);
    }

    size_t pop(int16_t* dst, size_t frames) {
        uint64_t rr = r.load(std::memory_order_relaxed);
        uint64_t ww = w.load(std::memory_order_acquire);
        size_t samples = std::min<uint64_t>(frames * kChannels, ww - rr);
        for (size_t i = 0; i < samples; ++i) dst[i] = data[(rr + i) % kRingSamples];
        r.store(rr + samples, std::memory_order_release);
        return samples / kChannels;
    }

    void trimTo(size_t maxFrames) {
        uint64_t ww = w.load(std::memory_order_acquire);
        uint64_t rr = r.load(std::memory_order_relaxed);
        uint64_t maxSamples = maxFrames * kChannels;
        if (ww - rr > maxSamples) r.store(ww - maxSamples, std::memory_order_release);
    }
};

Ring gRing;
std::atomic<bool> gRunning{true};
std::atomic<uint64_t> gUnderruns{0};

// Zero-lookahead leveler. Adds CPU work but no additional audio buffering.
class Leveler {
    float gain_ = 1.0f;
public:
    void process(int16_t* s, size_t frames) {
        const size_t n = frames * kChannels;
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            float x = s[i] / 32768.0f;
            sum += static_cast<double>(x) * x;
        }
        float rms = std::sqrt(static_cast<float>(sum / std::max<size_t>(1, n)) + 1e-12f);
        constexpr float target = 0.12589254f; // -18 dBFS RMS
        constexpr float maxGain = 7.94328235f; // +18 dB
        float wanted = std::clamp(target / std::max(rms, 0.0005f), 1.0f, maxGain);

        // Reduce gain rapidly for loud events; raise it gradually to avoid pumping.
        float coeff = (wanted < gain_) ? 0.35f : 0.025f;
        gain_ += (wanted - gain_) * coeff;

        constexpr float ceiling = 0.89125094f; // -1 dBFS
        for (size_t i = 0; i < n; ++i) {
            float x = (s[i] / 32768.0f) * gain_;
            // Low-cost, zero-lookahead soft knee only when nearing the limiter.
            float ax = std::fabs(x);
            if (ax > 0.75f) {
                float sign = std::copysign(1.0f, x);
                float over = (ax - 0.75f) / (ceiling - 0.75f);
                over = std::max(0.0f, over);
                float y = 0.75f + (ceiling - 0.75f) * std::tanh(over);
                x = sign * std::min(y, ceiling);
            }
            x = std::clamp(x, -ceiling, ceiling);
            s[i] = static_cast<int16_t>(std::lrintf(x * 32767.0f));
        }
    }
};

struct UacPcm {
    int card = -1;
    int device = -1;
};

UacPcm findUacCapturePcm() {
    std::ifstream f("/proc/asound/pcm");
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("UAC1_PCM") == std::string::npos) continue;
        if (line.find("capture ") == std::string::npos) continue;

        int card = -1, device = -1;
        if (std::sscanf(line.c_str(), "%d-%d:", &card, &device) == 2 &&
            card >= 0 && card < 32 && device >= 0 && device < 32) {
            return {card, device};
        }
    }
    return {};
}

bool gadgetConfigured() {
    std::ifstream f("/sys/class/udc/13600000.dwc3/state");
    std::string s;
    if (f && std::getline(f, s)) {
        // A configured USB device can legally enter USB suspend while the
        // audio interface remains bound. v0.11.1 treated "suspended" as a
        // disconnect, tearing down AAudio/ALSA and immediately reopening it.
        // Keep the bridge alive in both stable configured states.
        return s == "configured" || s == "suspended";
    }
    // Do not hard-fail on other kernels/UDC naming if ALSA card exists.
    return true;
}

// O31 proved that, once the Samsung UAC endpoint wedges, closing/reopening
// hw:3,0 succeeds but no frames ever return. At that point the kernel gadget
// itself must be re-presented to the host. This keeps the UAC configuration
// intact and only performs a short UDC unbind/bind -- the USB-side equivalent
// of a cable reconnect, without toggling the module Action.
bool softUdcRebind() {
    constexpr const char* kGadgetUdc = "/config/usb_gadget/g1/UDC";
    std::string ctl;
    {
        std::ifstream f(kGadgetUdc);
        std::getline(f, ctl);
    }
    if (ctl.empty()) ctl = "13600000.dwc3";

    auto writeSysfs = [](const char* path, const std::string& value) -> bool {
        int fd = open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) return false;
        std::string v = value + "\n";
        ssize_t n = write(fd, v.data(), v.size());
        close(fd);
        return n == static_cast<ssize_t>(v.size());
    };

    LOGE("UAC capture remained stalled; soft-rebinding UDC %s", ctl.c_str());
    if (!writeSysfs(kGadgetUdc, "")) {
        LOGE("UDC soft-unbind failed: %s", strerror(errno));
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    if (!writeSysfs(kGadgetUdc, ctl)) {
        LOGE("UDC soft-bind failed for %s: %s", ctl.c_str(), strerror(errno));
        return false;
    }
    LOGI("UDC soft-rebind complete; host will re-enumerate USB Audio");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return true;
}

aaudio_data_callback_result_t dataCallback(AAudioStream*, void*, void* audioData, int32_t numFrames) {
    auto* out = static_cast<int16_t*>(audioData);
    size_t got = gRing.pop(out, static_cast<size_t>(numFrames));
    if (got < static_cast<size_t>(numFrames)) {
        std::memset(out + got * kChannels, 0, (numFrames - got) * kChannels * sizeof(int16_t));
        gUnderruns.fetch_add(1, std::memory_order_relaxed);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void errorCallback(AAudioStream*, void*, aaudio_result_t error) {
    LOGE("AAudio error: %s", AAudio_convertResultToText(error));
    gRunning.store(false, std::memory_order_release);
}

AAudioStream* openOutput() {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return nullptr;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, kRate);
    AAudioStreamBuilder_setChannelCount(b, kChannels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);

    AAudioStream* stream = nullptr;
    aaudio_result_t rc = AAudioStreamBuilder_openStream(b, &stream);
    AAudioStreamBuilder_delete(b);
    if (rc != AAUDIO_OK || !stream) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(rc));
        return nullptr;
    }
    if (AAudioStream_getSampleRate(stream) != kRate || AAudioStream_getChannelCount(stream) != kChannels) {
        LOGE("Unexpected output format %d Hz / %d ch", AAudioStream_getSampleRate(stream), AAudioStream_getChannelCount(stream));
        AAudioStream_close(stream);
        return nullptr;
    }
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    // v0.11.4: two bursts was too fragile for a root-native bridge fed by a
    // separate USB clock. Keep ~40 ms inside AudioFlinger/AAudio so short USB
    // scheduling gaps do not turn into an output xrun storm.
    if (burst > 0) AAudioStream_setBufferSizeInFrames(stream, std::max(burst * 8, 1920));
    LOGI("AAudio output opened: burst=%d buffer=%d perf=%d",
         burst, AAudioStream_getBufferSizeInFrames(stream), AAudioStream_getPerformanceMode(stream));
    return stream;
}

struct AlsaCapture {
    snd_pcm_t* pcm = nullptr;
    unsigned int periodFrames = kPeriodFrames;
    unsigned int bufferFrames = kPeriodFrames * 4;
};

static void logAlsaState(snd_pcm_t* pcm, const char* where) {
    if (!pcm) return;
    snd_pcm_status_t* st = nullptr;
    if (snd_pcm_status_malloc(&st) < 0 || !st) return;
    if (snd_pcm_status(pcm, st) >= 0) {
        LOGI("ALSA %s state=%s avail=%ld delay=%ld",
             where, snd_pcm_state_name(snd_pcm_status_get_state(st)),
             static_cast<long>(snd_pcm_status_get_avail(st)),
             static_cast<long>(snd_pcm_status_get_delay(st)));
    }
    snd_pcm_status_free(st);
}

static bool openAlsaCapture(int card, int device, AlsaCapture& cap) {
    char name[32];
    std::snprintf(name, sizeof(name), "hw:%d,%d", card, device);

    snd_pcm_t* pcm = nullptr;
    int rc = snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (rc < 0 || !pcm) {
        LOGE("alsa-lib snd_pcm_open(%s) failed: %s", name, snd_strerror(rc));
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_sw_params_t* sw = nullptr;
    if (snd_pcm_hw_params_malloc(&hw) < 0 || !hw ||
        snd_pcm_sw_params_malloc(&sw) < 0 || !sw) {
        LOGE("alsa-lib parameter allocation failed");
        if (hw) snd_pcm_hw_params_free(hw);
        if (sw) snd_pcm_sw_params_free(sw);
        snd_pcm_close(pcm);
        return false;
    }

    auto fail = [&](const char* what, int err) {
        LOGE("alsa-lib %s failed: %s (%d)", what, snd_strerror(err), err);
        snd_pcm_hw_params_free(hw);
        snd_pcm_sw_params_free(sw);
        snd_pcm_close(pcm);
        return false;
    };

    if ((rc = snd_pcm_hw_params_any(pcm, hw)) < 0) return fail("hw_params_any", rc);
    if ((rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        return fail("set_access", rc);
    if ((rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0)
        return fail("set_format", rc);
    if ((rc = snd_pcm_hw_params_set_channels(pcm, hw, kChannels)) < 0)
        return fail("set_channels", rc);

    unsigned int rate = kRate;
    int dir = 0;
    if ((rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir)) < 0)
        return fail("set_rate_near", rc);

    snd_pcm_uframes_t period = kPeriodFrames;
    dir = 0;
    if ((rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir)) < 0)
        return fail("set_period_size_near", rc);

    snd_pcm_uframes_t buffer = kPeriodFrames * 4;
    if ((rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0)
        return fail("set_buffer_size_near", rc);

    if ((rc = snd_pcm_hw_params(pcm, hw)) < 0) return fail("hw_params", rc);

    snd_pcm_uframes_t gotPeriod = 0, gotBuffer = 0;
    dir = 0;
    snd_pcm_hw_params_get_period_size(hw, &gotPeriod, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &gotBuffer);
    if (rate != kRate || gotPeriod != kPeriodFrames || gotBuffer != kPeriodFrames * 4) {
        LOGE("alsa-lib negotiated unexpected geometry rate=%u period=%lu buffer=%lu",
             rate, static_cast<unsigned long>(gotPeriod), static_cast<unsigned long>(gotBuffer));
        snd_pcm_hw_params_free(hw);
        snd_pcm_sw_params_free(sw);
        snd_pcm_close(pcm);
        return false;
    }

    // Do not reproduce alsa-lib's kernel ABI by hand.  O25/O26 proved that
    // this exact library path can run the Samsung UAC capture continuously.
    // Ask alsa-lib itself to build/apply the software params and manage the
    // mmap-control/SYNC_PTR fallback on this kernel.
    if ((rc = snd_pcm_sw_params_current(pcm, sw)) < 0) return fail("sw_params_current", rc);
    if ((rc = snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_NONE)) < 0)
        return fail("set_tstamp_mode", rc);
    if ((rc = snd_pcm_sw_params_set_avail_min(pcm, sw, kPeriodFrames)) < 0)
        return fail("set_avail_min", rc);
    if ((rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, 1)) < 0)
        return fail("set_start_threshold", rc);
    if ((rc = snd_pcm_sw_params_set_stop_threshold(pcm, sw, kPeriodFrames * 4)) < 0)
        return fail("set_stop_threshold", rc);
    if ((rc = snd_pcm_sw_params(pcm, sw)) < 0) return fail("sw_params", rc);
    if ((rc = snd_pcm_prepare(pcm)) < 0) return fail("prepare", rc);

    snd_pcm_hw_params_free(hw);
    snd_pcm_sw_params_free(sw);

    cap.pcm = pcm;
    cap.periodFrames = static_cast<unsigned int>(gotPeriod);
    cap.bufferFrames = static_cast<unsigned int>(gotBuffer);
    LOGI("ALSA capture opened through alsa-lib: %s 48k S16 stereo period=%u buffer=%u",
         name, cap.periodFrames, cap.bufferFrames);
    logAlsaState(pcm, "prepared");
    return true;
}

static int readAlsaFrames(AlsaCapture& cap, int16_t* dst, unsigned int frames) {
    if (!cap.pcm) return -ENODEV;

    for (;;) {
        snd_pcm_sframes_t n = snd_pcm_readi(cap.pcm, dst, frames);
        if (n >= 0) return static_cast<int>(n);

        if (n == -EAGAIN) {
            int wr = snd_pcm_wait(cap.pcm, 250);
            if (wr > 0) continue;
            if (wr == 0) {
                // v0.11.5 treated an ALSA wait timeout as success and looped
                // forever. O30 showed capturedFrames freezing permanently while
                // AAudio continued writing silence. Return a timeout so the
                // capture thread can reopen the UAC PCM automatically.
                return -ETIMEDOUT;
            }
            if (wr == -EINTR) continue;

            // O32: snd_pcm_wait itself can report -EPIPE/XRUN even when
            // snd_pcm_readi did not. v0.11.8 treated that as fatal, which
            // tore down and reopened the whole bridge and caused the audible
            // cut/work cycle. Recover wait-side XRUNs in place just like
            // read-side XRUNs.
            int rr = snd_pcm_recover(cap.pcm, wr, 1);
            if (rr >= 0) {
                LOGI("alsa-lib recovered wait-side capture from %s", snd_strerror(wr));
                continue;
            }

            LOGE("alsa-lib snd_pcm_wait failed: %s (%d), recover=%s (%d)",
                 snd_strerror(wr), wr, snd_strerror(rr), rr);
            logAlsaState(cap.pcm, "wait-error");
            return wr;
        }
        if (n == -EINTR) continue;

        int rr = snd_pcm_recover(cap.pcm, static_cast<int>(n), 1);
        if (rr >= 0) {
            LOGI("alsa-lib recovered capture from %s", snd_strerror(static_cast<int>(n)));
            continue;
        }

        LOGE("alsa-lib snd_pcm_readi failed: %s (%ld), recover=%s (%d)",
             snd_strerror(static_cast<int>(n)), static_cast<long>(n),
             snd_strerror(rr), rr);
        logAlsaState(cap.pcm, "read-error");
        return static_cast<int>(n);
    }
}

int runConnected(int card, int device) {
    LOGI("Opening ALSA UAC capture card=%d device=%d 48k S16 stereo period=240 buffer=960", card, device);

    AlsaCapture in;
    if (!openAlsaCapture(card, device, in)) return 2;

    AAudioStream* out = openOutput();
    if (!out) { snd_pcm_close(in.pcm); return 3; }

    // v0.11.4: keep capture and playback on separate blocking threads.
    // v0.11.3 performed read -> write serially, coupling two independent 48 kHz
    // clocks. O29 showed the bridge still capturing/writing valid non-zero audio
    // while AAudio xruns climbed from 15 to 195 in ~2.5 s. A small jitter/drift
    // buffer prevents a late USB read from starving AAudio.
    gRing.clear();
    gRunning.store(true, std::memory_order_release);
    gUnderruns.store(0, std::memory_order_relaxed);

    std::atomic<uint64_t> capturedFrames{0};
    std::atomic<uint64_t> writtenFrames{0};
    std::atomic<int> peak{0};
    std::atomic<uint64_t> ringDrops{0};

    std::thread captureThread([&]() {
        Leveler leveler;
        std::vector<int16_t> block(kPeriodFrames * kChannels);
        int consecutiveTimeouts = 0;
        bool reopenedOnce = false;
        while (gRunning.load(std::memory_order_acquire) && gadgetConfigured()) {
            int frames = readAlsaFrames(in, block.data(), kPeriodFrames);
            if (frames == -ETIMEDOUT) {
                ++consecutiveTimeouts;

                // One ordinary ALSA reopen is cheap and can recover a local PCM
                // hiccup. O31 showed that endlessly reopening the PCM cannot
                // recover a wedged USB gadget endpoint, so do it only once.
                if (!reopenedOnce) {
                    LOGE("ALSA capture stalled for 250 ms; reopening hw:%d,%d once", card, device);
                    if (in.pcm) {
                        snd_pcm_drop(in.pcm);
                        snd_pcm_close(in.pcm);
                        in.pcm = nullptr;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    if (!openAlsaCapture(card, device, in)) {
                        LOGE("ALSA capture reopen failed; ending run for supervisor retry");
                        gRunning.store(false, std::memory_order_release);
                        break;
                    }
                    reopenedOnce = true;
                    LOGI("ALSA capture reopened after stall; waiting for host frames");
                    continue;
                }

                // About three seconds with no frames after a successful stream is
                // long enough to distinguish a scheduler hiccup from the permanent
                // stall seen in O31. Re-present the existing UAC gadget to the host.
                if (consecutiveTimeouts >= 12 &&
                    capturedFrames.load(std::memory_order_relaxed) > 0) {
                    if (in.pcm) {
                        snd_pcm_drop(in.pcm);
                        snd_pcm_close(in.pcm);
                        in.pcm = nullptr;
                    }
                    bool rebound = softUdcRebind();
                    LOGI("UDC recovery result=%s; restarting bridge discovery", rebound ? "success" : "failed");
                    gRunning.store(false, std::memory_order_release);
                    break;
                }
                continue;
            }
            if (frames < 0) {
                gRunning.store(false, std::memory_order_release);
                break;
            }
            if (frames == 0) continue;

            consecutiveTimeouts = 0;
            reopenedOnce = false;
            capturedFrames.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
            int localPeak = 0;
            for (int i = 0; i < frames * kChannels; ++i) {
                int v = block[static_cast<size_t>(i)];
                int a = v == INT16_MIN ? 32767 : std::abs(v);
                if (a > localPeak) localPeak = a;
            }
            int old = peak.load(std::memory_order_relaxed);
            while (localPeak > old && !peak.compare_exchange_weak(old, localPeak, std::memory_order_relaxed)) {}

            leveler.process(block.data(), static_cast<size_t>(frames));
            gRing.push(block.data(), static_cast<size_t>(frames));

            // If the USB clock runs slightly faster than the speaker clock,
            // bound latency instead of allowing the queue to grow forever.
            size_t avail = gRing.framesAvailable();
            if (avail > kTargetBufferedFrames + 1920) {
                gRing.trimTo(kTargetBufferedFrames + 960);
                ringDrops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Pre-fill before starting playback. This was missing from the old callback
    // attempt and is the key difference: AAudio starts with a real jitter cushion.
    auto prefillDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (gRunning.load(std::memory_order_acquire) && gadgetConfigured() &&
           gRing.framesAvailable() < kTargetBufferedFrames &&
           std::chrono::steady_clock::now() < prefillDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    aaudio_result_t rc = AAudioStream_requestStart(out);
    if (rc != AAUDIO_OK) {
        LOGE("AAudio start failed: %s", AAudio_convertResultToText(rc));
        gRunning.store(false, std::memory_order_release);
        if (captureThread.joinable()) captureThread.join();
        AAudioStream_close(out);
        snd_pcm_close(in.pcm);
        return 4;
    }

    std::vector<int16_t> play(kPeriodFrames * kChannels, 0);
    std::vector<int16_t> last(kPeriodFrames * kChannels, 0);
    uint64_t softUnderflows = 0;
    auto lastStats = std::chrono::steady_clock::now();
    LOGI("LIVE card=%d device=%d alsa-lib -> buffered blocking AAudio MEDIA 48k S16 stereo prefill=%zu",
         card, device, gRing.framesAvailable());

    while (gRunning.load(std::memory_order_acquire) && gadgetConfigured()) {
        size_t got = gRing.pop(play.data(), kPeriodFrames);
        if (got < kPeriodFrames) {
            // Do not starve AAudio. A short USB scheduling gap is concealed by
            // repeating the most recent frame for the missing tail; the 40 ms
            // AAudio buffer normally makes this path rare.
            int16_t l = 0, r = 0;
            if (got > 0) {
                l = play[(got - 1) * kChannels];
                r = play[(got - 1) * kChannels + 1];
            } else if (!last.empty()) {
                l = last[(kPeriodFrames - 1) * kChannels];
                r = last[(kPeriodFrames - 1) * kChannels + 1];
            }
            for (size_t f = got; f < kPeriodFrames; ++f) {
                play[f * kChannels] = l;
                play[f * kChannels + 1] = r;
            }
            ++softUnderflows;
        }
        last = play;

        int32_t done = 0;
        while (done < kPeriodFrames && gRunning.load(std::memory_order_acquire)) {
            aaudio_result_t wr = AAudioStream_write(
                out,
                play.data() + static_cast<size_t>(done) * kChannels,
                kPeriodFrames - done,
                100000000LL);
            if (wr > 0) {
                done += static_cast<int32_t>(wr);
                writtenFrames.fetch_add(static_cast<uint64_t>(wr), std::memory_order_relaxed);
                continue;
            }
            if (wr == 0 || wr == AAUDIO_ERROR_TIMEOUT) continue;
            LOGE("AAudio write failed: %s (%d), state=%d",
                 AAudio_convertResultToText(wr), wr, AAudioStream_getState(out));
            gRunning.store(false, std::memory_order_release);
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastStats >= std::chrono::seconds(2)) {
            LOGI("STREAM captured=%llu written=%llu peak=%d ring=%zu softUnder=%llu drops=%llu aaudioState=%d xrun=%d",
                 (unsigned long long)capturedFrames.load(std::memory_order_relaxed),
                 (unsigned long long)writtenFrames.load(std::memory_order_relaxed),
                 peak.exchange(0, std::memory_order_relaxed),
                 gRing.framesAvailable(),
                 (unsigned long long)softUnderflows,
                 (unsigned long long)ringDrops.load(std::memory_order_relaxed),
                 AAudioStream_getState(out),
                 AAudioStream_getXRunCount(out));
            lastStats = now;
        }
    }

    gRunning.store(false, std::memory_order_release);
    if (captureThread.joinable()) captureThread.join();
    AAudioStream_requestStop(out);
    AAudioStream_close(out);
    if (in.pcm) { snd_pcm_drop(in.pcm); snd_pcm_close(in.pcm); in.pcm = nullptr; }
    LOGI("Disconnected/stopped; captured=%llu written=%llu softUnder=%llu drops=%llu",
         (unsigned long long)capturedFrames.load(),
         (unsigned long long)writtenFrames.load(),
         (unsigned long long)softUnderflows,
         (unsigned long long)ringDrops.load());
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    LOGI("AndroidUsbSpeaker supervisor started uid=%d", getuid());
    bool warnedMissingCapture = false;
    while (true) {
        UacPcm uac = findUacCapturePcm();
        if (uac.card >= 0 && gadgetConfigured()) {
            warnedMissingCapture = false;
            runConnected(uac.card, uac.device);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        } else {
            if (!warnedMissingCapture) {
                std::ifstream cards("/proc/asound/cards");
                std::string all((std::istreambuf_iterator<char>(cards)), std::istreambuf_iterator<char>());
                if (all.find("UAC1") != std::string::npos) {
                    LOGE("UAC1 card exists but no capture-capable UAC1_PCM is published");
                    warnedMissingCapture = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
        }
    }
}
