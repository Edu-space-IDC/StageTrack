#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    static constexpr int maxTracks = 64;
    static constexpr int maxSongs = 32;
    static constexpr int peakCount = 2048;
    static constexpr int maxOuts = 4;           // salidas por pista
    static constexpr int maxDeviceChannels = 128;
    struct Track {
        std::unique_ptr<juce::AudioBuffer<float>> audio;
        juce::String name;
        int songIndex = -1;
        double sourceRate = 44100.0;
        uint64_t decodedBytes = 0;
        std::array<float, peakCount> peaks {};
        std::atomic<float> gain { 1.0f };
        std::atomic<bool> mute { false }, solo { false };
        // Routing: hasta maxOuts salidas fisicas por pista (par estereo o canal mono). ch = canal FISICO del
        // dispositivo (base 0); -1 = ranura libre. Todo atomico: la web lo cambia mientras suena.
        struct HwOut {
            std::atomic<int> ch { -1 };
            std::atomic<bool> mono { false }, mute { false };
            std::atomic<float> vol { 1.0f }, pan { 0.0f };
        };
        std::array<HwOut, maxOuts> outs;       // salidas DIRECTAS de la pista (ademas o en vez del Master)
        std::atomic<bool> masterSend { true }; // envio al bus Master
        std::atomic<bool> ready { false };
        std::atomic<float> pan { 0.0f };   // -1 (L) .. +1 (R), balance on the output pair
        std::atomic<float> peak { 0.0f };  // last block peak, written by the audio thread
        std::atomic<int> color { 0 };      // 0xRRGGBB, 0 = default
    };

    // Resultado de decodificar (o leer de la cache) un archivo. Se puede producir en varios hilos a la vez;
    // luego se "commitea" en orden con commitPrepared(). Si no se commitea, libera su reserva de RAM.
    struct Prepared
    {
        Prepared() = default;
        Prepared(Prepared&& o) noexcept { *this = std::move(o); }
        Prepared& operator=(Prepared&& o) noexcept
        {
            if (this != &o) {
                release();
                data = std::move(o.data); name = std::move(o.name); fileBase = std::move(o.fileBase);
                error = std::move(o.error); sourceRate = o.sourceRate; peaks = o.peaks;
                ok = o.ok; fromCache = o.fromCache; total = o.total; bytes = o.bytes; o.bytes = 0;
            }
            return *this;
        }
        ~Prepared() { release(); }
        void release() noexcept { if (bytes != 0 && total != nullptr) total->fetch_sub(bytes, std::memory_order_relaxed); bytes = 0; }

        std::unique_ptr<juce::AudioBuffer<float>> data;
        juce::String name, fileBase, error;
        double sourceRate = 44100.0;
        std::array<float, peakCount> peaks {};
        bool ok = false, fromCache = false;
        std::atomic<uint64_t>* total = nullptr;
        uint64_t bytes = 0;
    };

    AudioEngine();
    ~AudioEngine() override;
    void setPad(int semitone, bool enabled, float gain, int bank) noexcept;
    int getActivePad() const noexcept { const int active = activePad.load(std::memory_order_acquire); return active < 0 ? -1 : active % 12; }
    int getActivePadBank() const noexcept { const int active = activePad.load(std::memory_order_acquire); return active < 0 ? -1 : active / 12; }
    Prepared prepareFile(const juce::File&, juce::String trackName = {});            // thread-safe
    bool commitPrepared(Prepared&&, int songIndex, juce::String& error);             // rapido, en orden
    static juce::File cacheDir();
    static void pruneCache(uint64_t maxBytes = 6ULL << 30);                           // borra .tmp y lo mas viejo
    bool importFile(const juce::File&, int songIndex, juce::String& error,juce::String trackName = {});
    int getTotalTrackCount() const noexcept { return trackCount.load(std::memory_order_acquire); }
    int getTrackCount() const noexcept;
    int getSongTrackCount(int song) const noexcept;
    Track& getTrack(int i) noexcept;
    const Track& getTrack(int i) const noexcept;
    int getActiveSong() const noexcept { return activeSong.load(std::memory_order_acquire); }
    int getPlaybackSong() const noexcept { return activeSong.load(std::memory_order_acquire); }
    void setActiveSong(int song) noexcept;
    void setPlaying(bool b) noexcept;
    void fadeStopAndRewind(double seconds = 0.65) noexcept;
    bool isPlaying() const noexcept { return transports[(size_t)activeSong.load(std::memory_order_acquire)].playing.load(std::memory_order_acquire); }
    bool isSongPlaying(int song) const noexcept { return song >= 0 && song < maxSongs && transports[(size_t)song].playing.load(std::memory_order_acquire); }
    bool getRepeat() const noexcept { return transports[(size_t)activeSong.load(std::memory_order_acquire)].repeat.load(std::memory_order_acquire); }
    void setRepeat(bool enabled) noexcept { transports[(size_t)activeSong.load(std::memory_order_acquire)].repeat.store(enabled, std::memory_order_release); }
    bool getSongRepeat(int song) const noexcept { return song >= 0 && song < maxSongs && transports[(size_t)song].repeat.load(std::memory_order_acquire); }
    void setSongRepeat(int song, bool enabled) noexcept { if (song >= 0 && song < maxSongs) transports[(size_t)song].repeat.store(enabled, std::memory_order_release); }
    void setSectionPlayback(double startSeconds, double endSeconds, bool loop, bool enabled) noexcept
    {
        auto& t = transports[(size_t)activeSong.load(std::memory_order_acquire)];
        t.generation.fetch_add(1, std::memory_order_acq_rel);
        t.sectionStart.store(juce::jmax(0.0, startSeconds), std::memory_order_relaxed);
        t.sectionEnd.store(juce::jmax(0.0, endSeconds), std::memory_order_relaxed);
        t.sectionLoop.store(loop, std::memory_order_relaxed);
        t.sectionPlayback.store(enabled, std::memory_order_release);
    }
    void queueSectionJump(double boundarySeconds, double startSeconds, double endSeconds) noexcept
    {
        auto& t = transports[(size_t)activeSong.load(std::memory_order_acquire)];
        t.generation.fetch_add(1, std::memory_order_acq_rel);
        t.queuedJumpStart.store(juce::jmax(0.0, startSeconds), std::memory_order_relaxed);
        t.queuedJumpEnd.store(juce::jmax(0.0, endSeconds), std::memory_order_relaxed);
        t.queuedJumpAt.store(juce::jmax(0.0, boundarySeconds), std::memory_order_relaxed);
        t.queuedJump.store(endSeconds > startSeconds, std::memory_order_release);
    }
    void cancelQueuedSectionJump() noexcept { auto& t = transports[(size_t)activeSong.load(std::memory_order_acquire)]; t.generation.fetch_add(1, std::memory_order_acq_rel); t.queuedJump.store(false, std::memory_order_release); }
    void stopAndRewind() noexcept;
    void stopSongAndRewind(int song) noexcept;
    void setPositionSeconds(double seconds) noexcept;
    void removeTrack(int indexInActiveSong) noexcept;  // hides the track and silences it
    void releaseSong(int slot, bool resetTransport = true) noexcept; // releases decoded audio and hides every track
    double getTrackSeconds(const Track&) const noexcept;
    float getMasterPeak() const noexcept { return masterPeak.load(std::memory_order_relaxed); }
    float getPadPeak() const noexcept { return padPeak.load(std::memory_order_relaxed); }
    void setMaster(float v) noexcept { master.store(v); }
    void clearSongDuration(int slot) noexcept { if (slot >= 0 && slot < maxSongs) durations[(size_t)slot].store(0.0); }
    float getMaster() const noexcept { return master.load(); }
    double getPositionSeconds() const noexcept;
    double getSongPositionSeconds(int song) const noexcept;
    void setSongPositionSeconds(int song, double seconds) noexcept;
    double getDurationSeconds() const noexcept;
    double getSampleRate() const noexcept { return sampleRate.load(); }
    std::atomic<double> bpm { 92.0 };
    std::atomic<float> clickGain { 0.65f };
    std::atomic<bool> clickEnabled { true };
    struct SongTransport {
        std::atomic<bool> playing { false }, repeat { false }, fadeResetRequested { false };
        std::atomic<int64_t> position { 0 };
        std::atomic<uint64_t> generation { 0 };
        std::atomic<bool> sectionPlayback { false }, sectionLoop { false }, queuedJump { false }, fadeStopRequested { false };
        std::atomic<double> sectionStart { 0.0 }, sectionEnd { 0.0 }, queuedJumpAt { 0.0 }, queuedJumpStart { 0.0 }, queuedJumpEnd { 0.0 }, fadeStopSeconds { 0.65 };
        float fadeGain = 1.0f, fadeStep = 0.0f;
        int fadeRemaining = 0;
    };
    uint64_t getDecodedBytes() const noexcept { return decodedBytes.load(std::memory_order_relaxed); }

    // Routing / salidas del dispositivo. Los nombres vienen del driver (todos los canales, activos o no).
    juce::StringArray getOutputNames() const;
    int getDeviceOutputCount() const noexcept { return deviceOutputs.load(std::memory_order_acquire); }
    // Salidas del bus Master y del click (cada una hasta maxOuts salidas fisicas, par estereo o mono)
    std::array<Track::HwOut, maxOuts> masterOuts, clickOuts;
    static juce::String outsToString(const std::array<Track::HwOut, maxOuts>&);
    static void outsFromString(std::array<Track::HwOut, maxOuts>&, const juce::String&);
    static void resetRouting(Track&) noexcept;                       // Master activo, sin salidas directas
    static int activeOutCount(const Track&) noexcept;
    // Texto "ch|mono|vol|pan|mute;..." (solo salidas activas) para guardar/restaurar
    static juce::String routingToString(const Track&);
    static void routingFromString(Track&, const juce::String&);
    Track* findTrack(int song, int indexInSong) noexcept;            // indice dentro de la cancion (0 based)

    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int,
                                           int, const juce::AudioIODeviceCallbackContext&) override;
private:
    bool reserveBytes(uint64_t required, Prepared&, juce::String& error);
    juce::CriticalSection commitLock;
    std::atomic<int> callbackReaders { 0 };
    std::atomic<bool> trackMaintenance { false };
    std::array<std::unique_ptr<Track>, maxTracks> tracks;
    std::atomic<int> trackCount { 0 };
    std::atomic<float> master { 0.82f };
    std::atomic<double> sampleRate { 44100.0 };
    std::array<SongTransport, maxSongs> transports;
    std::array<std::atomic<double>, maxSongs> durations {};
    std::atomic<int> activeSong { 0 };
    std::atomic<uint64_t> decodedBytes { 0 };
    std::atomic<float> masterPeak { 0.0f };
    std::atomic<float> padPeak { 0.0f };
    void routeTo(const Track::HwOut&, const float* l, const float* r, float* const* out, int outputs, int off, int n, float extra) const noexcept;
    void mixPads(float* const* out, int outputs, int frames) noexcept;
    struct PadVoice {
        std::unique_ptr<juce::BufferingAudioSource> buffered;
        std::unique_ptr<juce::ResamplingAudioSource> resampled;
        juce::AudioBuffer<float> block;
        double sourceRate = 44100.0;
        std::atomic<float> targetGain { 0.0f };
        std::atomic<float> rampSeconds { 5.0f };
        float currentGain = 0.0f, rampTarget = 0.0f, rampStep = 0.0f;
        int rampRemaining = 0;
    };
    juce::TimeSliceThread padReadAhead { "StageTracks pad audio" };
    std::array<PadVoice, 24> padVoices;
    std::atomic<int> activePad { -1 };
    std::atomic<int> restartPad { -1 };
    int mapOut(int physical) const noexcept
    {
        return (physical >= 0 && physical < maxDeviceChannels) ? physToCompact[(size_t) physical].load(std::memory_order_relaxed) : -1;
    }
    std::array<std::atomic<int>, maxDeviceChannels> physToCompact;   // canal fisico -> indice en el buffer del callback (-1 = inactivo)
    std::atomic<int> deviceOutputs { 0 };
    mutable juce::CriticalSection namesLock;
    juce::StringArray outputNames;
    std::vector<float> scratchL, scratchR, busL, busR;                          // solo hilo de audio
    std::vector<float> songFadeBuffer;
    double clickPhase = 0.0; // audio thread only
    int64_t clickSample = -1;
};
