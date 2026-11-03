#include "AudioEngine.h"
#include <algorithm>
#include <limits>
#include <vector>

AudioEngine::AudioEngine()
{
    for(auto& d:durations)d.store(0.0,std::memory_order_relaxed);
    for (auto& t : tracks) { t = std::make_unique<Track>(); resetRouting(*t); }
    for (int i = 0; i < maxDeviceChannels; ++i) physToCompact[(size_t) i].store(i, std::memory_order_relaxed);
    scratchL.assign(16384, 0.0f); scratchR.assign(16384, 0.0f); busL.assign(16384, 0.0f); busR.assign(16384, 0.0f); songFadeBuffer.assign(16384, 1.0f);
    for (auto* arr : { &masterOuts, &clickOuts })
        for (auto& o : *arr) { o.ch.store(-1); o.mono.store(false); o.mute.store(false); o.vol.store(1.0f); o.pan.store(0.0f); }
    masterOuts[0].ch.store(0);                                   // Master -> 1/2
    clickOuts[0].mono.store(true); clickOuts[0].ch.store(2);     // click -> canal 3 (mono)

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto padDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("Pads");
    const juce::StringArray names { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    padReadAhead.startThread();
    for (int bank = 0; bank < 2; ++bank) {
        const auto bankDir = padDir.getChildFile(bank == 0 ? "Soft" : "Default");
        const auto extension = bank == 0 ? ".wav" : ".mp3";
        for (int i = 0; i < names.size(); ++i) {
            const auto file = bankDir.getChildFile("Pads Worship " + names[i] + extension);
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
            if (!reader) continue;
            const double sourceRate = reader->sampleRate;
            auto readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
            readerSource->setLooping(true);
            auto& voice = padVoices[(size_t) (bank * 12 + i)];
            voice.sourceRate = sourceRate;
            voice.buffered = std::make_unique<juce::BufferingAudioSource>(readerSource.release(), padReadAhead, true, 32768, 2, false);
            voice.resampled = std::make_unique<juce::ResamplingAudioSource>(voice.buffered.get(), false, 2);
            voice.resampled->setResamplingRatio(sourceRate / 44100.0);
        }
    }
}

AudioEngine::~AudioEngine()
{
    padReadAhead.stopThread(5000);
}

void AudioEngine::setPad(int semitone, bool enabled, float gain, int bank) noexcept
{
    if (semitone < 0 || semitone >= 12 || bank < 0 || bank >= 2) return;
    const int voiceIndex = bank * 12 + semitone;
    auto& voice = padVoices[(size_t) voiceIndex];
    const bool samePad = activePad.load(std::memory_order_acquire) == voiceIndex;
    voice.rampSeconds.store(enabled && samePad ? 0.12f : 5.0f, std::memory_order_relaxed);
    voice.targetGain.store(enabled ? juce::jlimit(0.0f, 4.0f, gain) : 0.0f, std::memory_order_release);
    if (enabled) {
        const int previous = activePad.load(std::memory_order_acquire);
        if (previous >= 0 && previous != voiceIndex) {
            auto& previousVoice = padVoices[(size_t) previous];
            previousVoice.rampSeconds.store(5.0f, std::memory_order_relaxed);
            previousVoice.targetGain.store(0.0f, std::memory_order_release);
        }
        if (!samePad)
            restartPad.store(voiceIndex, std::memory_order_release);
        activePad.store(voiceIndex, std::memory_order_release);
    } else {
        int expected = voiceIndex;
        activePad.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
    }
}

// ------------------------------------------------------------------ routing
void AudioEngine::resetRouting(Track& t) noexcept
{
    for (auto& o : t.outs) { o.ch.store(-1); o.mono.store(false); o.mute.store(false); o.vol.store(1.0f); o.pan.store(0.0f); }
    t.masterSend.store(true);
}

int AudioEngine::activeOutCount(const Track& t) noexcept
{
    int n = 0;
    for (const auto& o : t.outs) if (o.ch.load(std::memory_order_relaxed) >= 0) ++n;
    return n;
}

juce::String AudioEngine::outsToString(const std::array<Track::HwOut, maxOuts>& outs)
{
    juce::StringArray rows;
    for (const auto& o : outs) {
        const int ch = o.ch.load();
        if (ch < 0) continue;
        rows.add(juce::String(ch) + "|" + (o.mono.load() ? "1" : "0") + "|" + juce::String(o.vol.load(), 4) + "|"
                 + juce::String(o.pan.load(), 4) + "|" + (o.mute.load() ? "1" : "0"));
    }
    return rows.joinIntoString(";");
}

void AudioEngine::outsFromString(std::array<Track::HwOut, maxOuts>& outs, const juce::String& text)
{
    for (auto& o : outs) o.ch.store(-1);
    size_t slot = 0;
    for (const auto& row : juce::StringArray::fromTokens(text, ";", "")) {
        if (slot >= outs.size()) break;
        const auto f = juce::StringArray::fromTokens(row, "|", "");
        if (f.size() < 5) continue;
        auto& o = outs[slot++];
        o.mono.store(f[1].getIntValue() != 0);
        o.vol.store(juce::jlimit(0.0f, 4.0f, f[2].getFloatValue()));
        o.pan.store(juce::jlimit(-1.0f, 1.0f, f[3].getFloatValue()));
        o.mute.store(f[4].getIntValue() != 0);
        o.ch.store(juce::jlimit(0, maxDeviceChannels - 1, f[0].getIntValue()));   // al final: la salida ya esta completa
    }
}

// "M1;<salidas>" (M1/M0 = envio al Master). Formato anterior sin "M": la pista salia directo, sin Master.
juce::String AudioEngine::routingToString(const Track& t)
{
    const auto rows = outsToString(t.outs);
    return juce::String(t.masterSend.load() ? "M1" : "M0") + (rows.isEmpty() ? juce::String() : ";" + rows);
}

void AudioEngine::routingFromString(Track& t, const juce::String& text)
{
    if (text.isEmpty()) return;                       // sin dato guardado: se queda el ruteo por defecto
    if (text.startsWith("M")) {
        t.masterSend.store(text[1] == '1');
        outsFromString(t.outs, text.fromFirstOccurrenceOf(";", false, false));
    } else {
        t.masterSend.store(false);
        outsFromString(t.outs, text == "none" ? juce::String() : text);
    }
}

AudioEngine::Track* AudioEngine::findTrack(int song, int indexInSong) noexcept
{
    const int total = trackCount.load(std::memory_order_acquire);
    for (int i = 0; i < total; ++i)
        if (tracks[(size_t) i]->songIndex == song && indexInSong-- == 0) return tracks[(size_t) i].get();
    return nullptr;
}

juce::StringArray AudioEngine::getOutputNames() const
{
    const juce::ScopedLock sl(namesLock);
    return outputNames;
}

namespace {
constexpr int kCacheMagic = 0x31435453;   // "STC1"
constexpr int kCacheVersion = 1;
constexpr uint64_t kCacheHeaderBytes = 4 + 4 + 4 + 4 + 8 + (uint64_t)AudioEngine::peakCount * 4;

// WAV/AIFF ya son PCM: leerlos directo es tan rapido como leer una cache (que seria mas grande), asi que no se cachean.
bool isUncompressedPcm(const juce::File& f)
{
    return f.hasFileExtension("wav") || f.hasFileExtension("wave") || f.hasFileExtension("bwf")
        || f.hasFileExtension("aif") || f.hasFileExtension("aiff");
}

juce::String cacheKey(const juce::File& f)
{
    const auto s = f.getFullPathName().toLowerCase() + "|" + juce::String(f.getSize()) + "|"
                 + juce::String(f.getLastModificationTime().toMilliseconds()) + "|v" + juce::String(kCacheVersion);
    // JUCE 8 no longer provides juce::MD5. Keep cache filenames deterministic.
    return juce::String::toHexString(s.hashCode64());
}

bool readFully(juce::InputStream& in, void* dst, uint64_t bytes)
{
    auto* p = static_cast<char*>(dst);
    while (bytes > 0) {
        const int chunk = (int)juce::jmin<uint64_t>(bytes, 8u << 20);
        if (in.read(p, chunk) != chunk) return false;
        p += chunk; bytes -= (uint64_t)chunk;
    }
    return true;
}

bool writeFully(juce::OutputStream& out, const void* src, uint64_t bytes)
{
    auto* p = static_cast<const char*>(src);
    while (bytes > 0) {
        const int chunk = (int)juce::jmin<uint64_t>(bytes, 8u << 20);
        if (!out.write(p, (size_t)chunk)) return false;
        p += chunk; bytes -= (uint64_t)chunk;
    }
    return true;
}

void computePeaks(const juce::AudioBuffer<float>& b, std::array<float, AudioEngine::peakCount>& out)
{
    const int64_t n = b.getNumSamples();
    for (int bucket = 0; bucket < AudioEngine::peakCount; ++bucket) {
        // multiplicar en 64 bits antes de reducir a int (archivos largos desbordarian)
        const int from = (int)((int64_t)bucket * n / AudioEngine::peakCount);
        const int to = (int)((int64_t)(bucket + 1) * n / AudioEngine::peakCount);
        float peak = 0.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch) {
            const float* d = b.getReadPointer(ch);
            for (int s = from; s < to; ++s) peak = juce::jmax(peak, std::abs(d[s]));
        }
        out[(size_t)bucket] = peak;
    }
}

void writeCache(const juce::File& cacheFile, const juce::AudioBuffer<float>& b, double rate,
                const std::array<float, AudioEngine::peakCount>& peaks)
{
    try {
        const uint64_t need = kCacheHeaderBytes + (uint64_t)b.getNumChannels() * (uint64_t)b.getNumSamples() * 4ULL;
        const auto dir = AudioEngine::cacheDir();
        dir.createDirectory();
        const int64_t freeBytes = dir.getBytesFreeOnVolume();
        if (freeBytes > 0 && (uint64_t)freeBytes < need + (1ULL << 30)) return;   // deja 1 GB libre
        const auto tmp = cacheFile.withFileExtension("tmp");
        tmp.deleteFile();
        bool good = false;
        {
            juce::FileOutputStream out(tmp);
            if (!out.openedOk()) return;
            out.writeInt(kCacheMagic); out.writeInt(kCacheVersion);
            out.writeInt(b.getNumChannels()); out.writeInt(b.getNumSamples());
            out.writeDouble(rate);
            good = writeFully(out, peaks.data(), peaks.size() * sizeof(float));
            for (int ch = 0; good && ch < b.getNumChannels(); ++ch)
                good = writeFully(out, b.getReadPointer(ch), (uint64_t)b.getNumSamples() * 4ULL);
            out.flush();
            good = good && !out.getStatus().failed();
        }
        // se renombra al final: un cierre a medias nunca deja una cache corrupta
        if (good && tmp.moveFileTo(cacheFile)) return;
        tmp.deleteFile();
    } catch (...) {}
}
} // namespace

juce::File AudioEngine::cacheDir()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("StageTracks").getChildFile("Cache");
}

void AudioEngine::pruneCache(uint64_t maxBytes)
{
    struct Entry { juce::File file; juce::int64 time; uint64_t size; };
    std::vector<Entry> entries;
    uint64_t total = 0;
    for (const auto& f : cacheDir().findChildFiles(juce::File::findFiles, false, "*")) {
        if (f.hasFileExtension("tmp")) { f.deleteFile(); continue; }
        if (!f.hasFileExtension("stc")) continue;
        entries.push_back({ f, f.getLastModificationTime().toMilliseconds(), (uint64_t)f.getSize() });
        total += (uint64_t)f.getSize();
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const auto& e : entries) {
        if (total <= maxBytes) break;
        if (e.file.deleteFile()) total -= e.size;
    }
}

bool AudioEngine::reserveBytes(uint64_t required, Prepared& p, juce::String& error)
{
    const uint64_t systemBytes = (uint64_t)juce::jmax(1024, juce::SystemStats::getMemorySizeInMegabytes()) * 1024ULL * 1024ULL;
    // Las pistas decodificadas permanecen en memoria para reproducir sin cortes.
    // El limite anterior (maximo 2 GB) rechazaba paquetes multipista aun en PCs
    // con mucha RAM disponible. Permitimos hasta el 40% de la RAM instalada,
    // con un tope de 8 GB para conservar margen para el sistema y la interfaz.
    const uint64_t budget = juce::jlimit<uint64_t>(512ULL << 20, 8ULL << 30, systemBytes * 2 / 5);
    uint64_t used = decodedBytes.load(std::memory_order_relaxed);
    if (required > budget || used > budget - required) {
        error = "No se pudo cargar la pista sin poner en riesgo la memoria disponible. Libera RAM o importa menos pistas a la vez.";
        return false;
    }
    while (!decodedBytes.compare_exchange_weak(used, used + required, std::memory_order_acq_rel)) {
        if (used > budget - required) { error = "No hay memoria disponible suficiente para cargar esta pista de forma segura."; return false; }
    }
    p.total = &decodedBytes; p.bytes = required;
    return true;
}

AudioEngine::Prepared AudioEngine::prepareFile(const juce::File& file, juce::String trackName)
{
    Prepared p;
    p.fileBase = file.getFileNameWithoutExtension();
    p.name = trackName.isNotEmpty() ? trackName : p.fileBase;
    try {
        if (!file.existsAsFile()) { p.error = "No se encontró el archivo de audio."; return p; }

        // 1) cache en disco (audio ya decodificado + picos): evita decodificar al reabrir el programa
        const bool useCache = !isUncompressedPcm(file);
        const auto cacheFile = useCache ? cacheDir().getChildFile(cacheKey(file) + ".stc") : juce::File();
        if (useCache && cacheFile.existsAsFile()) {
            juce::FileInputStream in(cacheFile);
            if (in.openedOk() && in.readInt() == kCacheMagic && in.readInt() == kCacheVersion) {
                const int ch = in.readInt(), ns = in.readInt();
                const double rate = in.readDouble();
                if (ch >= 1 && ch <= 2 && ns > 0 && rate > 0.0
                    && (uint64_t)cacheFile.getSize() == kCacheHeaderBytes + (uint64_t)ch * (uint64_t)ns * 4ULL) {
                    if (!reserveBytes((uint64_t)ch * (uint64_t)ns * sizeof(float), p, p.error)) return p;
                    auto data = std::make_unique<juce::AudioBuffer<float>>(ch, ns);
                    bool good = readFully(in, p.peaks.data(), p.peaks.size() * sizeof(float));
                    for (int c = 0; good && c < ch; ++c)
                        good = readFully(in, data->getWritePointer(c), (uint64_t)ns * 4ULL);
                    if (good) {
                        p.data = std::move(data); p.sourceRate = rate; p.ok = true; p.fromCache = true;
                        cacheFile.setLastModificationTime(juce::Time::getCurrentTime());   // para el orden LRU
                        return p;
                    }
                    p.release();   // cache danada: se descarta y se decodifica normal
                }
            }
            cacheFile.deleteFile();
        }

        // 2) decodificar el archivo original
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (!reader) { p.error = "No se pudo decodificar este audio. Prueba con WAV, AIFF, FLAC o un MP3 compatible."; return p; }
        if (reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max()
            || reader->sampleRate <= 0.0 || reader->numChannels == 0) {
            p.error = "El archivo de audio tiene información de duración o formato no válida."; return p;
        }
        const uint64_t channels = juce::jmin<uint32_t>(reader->numChannels, 2);
        const uint64_t required = (uint64_t)reader->lengthInSamples * channels * sizeof(float);
        if (!reserveBytes(required, p, p.error)) return p;
        auto data = std::make_unique<juce::AudioBuffer<float>>((int)channels, (int)reader->lengthInSamples);
        if (!reader->read(data.get(), 0, (int)reader->lengthInSamples, 0, true, true)) {
            p.error = "No se pudo leer el archivo de audio completo."; p.release(); return p;
        }
        computePeaks(*data, p.peaks);
        p.sourceRate = reader->sampleRate;
        if (useCache) writeCache(cacheFile, *data, p.sourceRate, p.peaks);
        p.data = std::move(data);
        p.ok = true;
        return p;
    } catch (const std::bad_alloc&) {
        p.error = "Windows se quedó sin memoria al decodificar este audio. No se agregó la pista.";
    } catch (const std::exception& ex) {
        p.error = "La importación se detuvo para proteger el programa: " + juce::String(ex.what());
    } catch (...) {
        p.error = "La importación falló por un error desconocido del decodificador.";
    }
    p.release(); p.data.reset(); p.ok = false;
    return p;
}

bool AudioEngine::commitPrepared(Prepared&& p, int songIndex, juce::String& error)
{
    if (!p.ok || p.data == nullptr) { error = p.error.isNotEmpty() ? p.error : juce::String("Import failed."); return false; }
    if (songIndex < 0 || songIndex >= maxSongs) { error = "Reached the 32-song limit."; return false; }
    const juce::ScopedLock sl(commitLock);
    const int slot = trackCount.load(std::memory_order_acquire);
    if (slot >= maxTracks) { error = "Reached the 64-track limit."; return false; }
    for (int i = 0; i < slot; ++i)
        if (tracks[(size_t)i]->songIndex == songIndex && tracks[(size_t)i]->name == p.fileBase) {
            error = "A track with this filename is already in this song."; return false;
        }
    auto& t = *tracks[(size_t)slot];
    t.name = p.name;
    t.songIndex = songIndex;
    t.sourceRate = p.sourceRate;
    t.peaks = p.peaks;
    t.audio = std::move(p.data);
    t.decodedBytes = p.bytes;
    p.bytes = 0;   // la reserva de RAM pasa a ser de la pista
    t.gain.store(1.0f); t.mute.store(false); t.solo.store(false); resetRouting(t);
    const double sec = (double)t.audio->getNumSamples() / p.sourceRate;
    double old = durations[(size_t)songIndex].load();
    while (sec > old && !durations[(size_t)songIndex].compare_exchange_weak(old, sec)) {}
    t.ready.store(true, std::memory_order_release);
    trackCount.store(slot + 1, std::memory_order_release);
    return true;
}

bool AudioEngine::importFile(const juce::File& file, int songIndex, juce::String& error, juce::String trackName)
{
    auto p = prepareFile(file, std::move(trackName));
    if (!p.ok) { error = p.error; return false; }
    return commitPrepared(std::move(p), songIndex, error);
}

int AudioEngine::getTrackCount() const noexcept
{
    return getSongTrackCount(activeSong.load(std::memory_order_acquire));
}

int AudioEngine::getSongTrackCount(int song) const noexcept
{
    const int total=trackCount.load(std::memory_order_acquire);
    int count=0;for(int i=0;i<total;++i)if(tracks[(size_t)i]->songIndex==song)++count;return count;
}

AudioEngine::Track& AudioEngine::getTrack(int index) noexcept
{
    const int song=activeSong.load(std::memory_order_acquire),total=trackCount.load(std::memory_order_acquire);
    for(int i=0;i<total;++i)if(tracks[(size_t)i]->songIndex==song&&index--==0)return *tracks[(size_t)i];
    return *tracks[0];
}

const AudioEngine::Track& AudioEngine::getTrack(int index) const noexcept
{
    const int song=activeSong.load(std::memory_order_acquire),total=trackCount.load(std::memory_order_acquire);
    for(int i=0;i<total;++i)if(tracks[(size_t)i]->songIndex==song&&index--==0)return *tracks[(size_t)i];
    return *tracks[0];
}

void AudioEngine::setActiveSong(int song) noexcept
{
    if(song<0||song>=maxSongs)return;
    activeSong.store(song, std::memory_order_release);
}

void AudioEngine::stopAndRewind() noexcept
{
    stopSongAndRewind(activeSong.load(std::memory_order_acquire));
}

void AudioEngine::stopSongAndRewind(int song) noexcept
{
    if (song < 0 || song >= maxSongs) return;
    auto& t = transports[(size_t)song];
    t.generation.fetch_add(1, std::memory_order_acq_rel);
    t.sectionPlayback.store(false, std::memory_order_release);
    t.queuedJump.store(false, std::memory_order_release);
    t.playing.store(false, std::memory_order_release);
    t.position.store(0, std::memory_order_release);
    t.fadeStopRequested.store(false, std::memory_order_release);
    t.fadeResetRequested.store(true, std::memory_order_release);
}

void AudioEngine::setPlaying(bool shouldPlay) noexcept
{
    auto& t = transports[(size_t)activeSong.load(std::memory_order_acquire)];
    t.generation.fetch_add(1, std::memory_order_acq_rel);
    if (shouldPlay) {
        t.fadeStopRequested.store(false, std::memory_order_release);
        t.fadeResetRequested.store(true, std::memory_order_release);
    }
    t.playing.store(shouldPlay, std::memory_order_release);
}

void AudioEngine::fadeStopAndRewind(double seconds) noexcept
{
    auto& t = transports[(size_t)activeSong.load(std::memory_order_acquire)];
    t.generation.fetch_add(1, std::memory_order_acq_rel);
    if (!t.playing.load(std::memory_order_acquire)) { stopAndRewind(); return; }
    t.fadeStopSeconds.store(juce::jlimit(0.08, 3.0, seconds), std::memory_order_relaxed);
    t.fadeStopRequested.store(true, std::memory_order_release);
}

void AudioEngine::setPositionSeconds(double seconds) noexcept
{
    auto& t = transports[(size_t)activeSong.load(std::memory_order_acquire)];
    t.generation.fetch_add(1, std::memory_order_acq_rel);
    const double d = getDurationSeconds();
    seconds = juce::jlimit(0.0, d > 0.0 ? d : 0.0, seconds);
    t.position.store((int64_t)(seconds * sampleRate.load()), std::memory_order_release);
}

double AudioEngine::getTrackSeconds(const Track& t) const noexcept
{
    return (t.audio != nullptr && t.sourceRate > 0.0) ? (double)t.audio->getNumSamples() / t.sourceRate : 0.0;
}

void AudioEngine::removeTrack(int index) noexcept
{
    const int song = activeSong.load(std::memory_order_acquire), total = trackCount.load(std::memory_order_acquire);
    for (int i = 0; i < total; ++i)
        if (tracks[(size_t)i]->songIndex == song && index-- == 0) {
            tracks[(size_t)i]->ready.store(false, std::memory_order_release);
            tracks[(size_t)i]->songIndex = -1;
            break;
        }
    // duration = longest remaining track of the song
    double longest = 0.0;
    for (int i = 0; i < total; ++i)
        if (tracks[(size_t)i]->songIndex == song) longest = juce::jmax(longest, getTrackSeconds(*tracks[(size_t)i]));
    durations[(size_t)song].store(longest);
}

void AudioEngine::releaseSong(int slot) noexcept
{
    if (slot >= 0 && slot < maxSongs) {
        auto& t = transports[(size_t)slot];
        t.playing.store(false, std::memory_order_release);
        t.generation.fetch_add(1, std::memory_order_acq_rel);
        t.position.store(0, std::memory_order_release);
        t.sectionPlayback.store(false, std::memory_order_release);
        t.queuedJump.store(false, std::memory_order_release);
    }
    const int total = trackCount.load(std::memory_order_acquire);
    for (int i = 0; i < total; ++i)
        if (tracks[(size_t)i]->songIndex == slot) {
            tracks[(size_t)i]->ready.store(false, std::memory_order_release);
            tracks[(size_t)i]->songIndex = -1;
        }
    clearSongDuration(slot);
}
double AudioEngine::getPositionSeconds() const noexcept { return (double)transports[(size_t)activeSong.load(std::memory_order_acquire)].position.load(std::memory_order_acquire) / sampleRate.load(); }
double AudioEngine::getDurationSeconds() const noexcept
{
    const int song = activeSong.load(std::memory_order_acquire);
    return durations[(size_t)song].load();
}
void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* d)
{
    if (d == nullptr) return;
    sampleRate.store(d->getCurrentSampleRate(), std::memory_order_release);
    // Mapa canal fisico -> posicion en el buffer del callback (el dispositivo solo entrega los canales activos, en orden).
    const auto active = d->getActiveOutputChannels();
    const auto names = d->getOutputChannelNames();
    int compact = 0;
    for (int c = 0; c < maxDeviceChannels; ++c)
        physToCompact[(size_t) c].store(active[c] ? compact++ : -1, std::memory_order_relaxed);
    deviceOutputs.store(juce::jmin(names.size(), maxDeviceChannels), std::memory_order_release);
    {
        const juce::ScopedLock sl(namesLock);
        outputNames = names;
    }
    const size_t need = (size_t) juce::jmax(4096, d->getCurrentBufferSizeSamples() * 2);
    if (scratchL.size() < need) { scratchL.assign(need, 0.0f); scratchR.assign(need, 0.0f); busL.assign(need, 0.0f); busR.assign(need, 0.0f); songFadeBuffer.assign(need, 1.0f); }
    for (auto& voice : padVoices) if (voice.resampled) {
        voice.block.setSize(2, (int) need, false, false, true);
        voice.resampled->setResamplingRatio(voice.sourceRate / sampleRate.load(std::memory_order_relaxed));
        voice.resampled->prepareToPlay((int) need, sampleRate.load(std::memory_order_relaxed));
    }
}
void AudioEngine::audioDeviceStopped()
{
    for (auto& t : transports) t.playing.store(false, std::memory_order_release);
    padPeak.store(0.0f, std::memory_order_relaxed);
    for (auto& voice : padVoices) if (voice.resampled) voice.resampled->releaseResources();
}

void AudioEngine::mixPads(float* const* out, int outputs, int frames) noexcept
{
    const int restart = restartPad.exchange(-1, std::memory_order_acq_rel);
    if (restart >= 0 && restart < (int) padVoices.size()) {
        auto& voice = padVoices[(size_t) restart];
        if (voice.resampled) {
            voice.buffered->setNextReadPosition(0);
            voice.resampled->flushBuffers();
        }
    }
    const double sr = sampleRate.load(std::memory_order_relaxed);
    const float masterGain = master.load(std::memory_order_relaxed);
    float combinedPeak = 0.0f;
    for (auto& voice : padVoices) {
        const float target = voice.targetGain.load(std::memory_order_acquire);
        if (std::abs(target - voice.rampTarget) > 0.000001f) {
            voice.rampTarget = target;
            const int fadeSamples = juce::jmax(1, (int) (sr * voice.rampSeconds.load(std::memory_order_relaxed)));
            voice.rampRemaining = fadeSamples;
            voice.rampStep = (target - voice.currentGain) / (float) fadeSamples;
        }
        if (voice.resampled == nullptr || voice.block.getNumSamples() < frames) continue;
        if (voice.currentGain <= 0.000001f && target <= 0.000001f) continue;
        voice.block.clear(0, frames);
        voice.resampled->getNextAudioBlock({ &voice.block, 0, frames });
        auto* left = voice.block.getWritePointer(0);
        auto* right = voice.block.getWritePointer(1);
        for (int i = 0; i < frames; ++i) {
            if (voice.rampRemaining > 0) {
                voice.currentGain += voice.rampStep;
                if (--voice.rampRemaining == 0) voice.currentGain = voice.rampTarget;
            }
            left[i] *= voice.currentGain;
            right[i] *= voice.currentGain;
        }
        const float blockPeak = juce::jmax(
            juce::jmax(juce::FloatVectorOperations::findMaximum(left, frames), -juce::FloatVectorOperations::findMinimum(left, frames)),
            juce::jmax(juce::FloatVectorOperations::findMaximum(right, frames), -juce::FloatVectorOperations::findMinimum(right, frames)));
        combinedPeak = juce::jmax(combinedPeak, blockPeak);
        for (const auto& destination : masterOuts) routeTo(destination, left, right, out, outputs, 0, frames, masterGain);
    }
    padPeak.store(combinedPeak, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out,
                                                    int outputs, int frames,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < outputs; ++c) if (out[c] != nullptr) juce::FloatVectorOperations::clear(out[c], frames);
    if (outputs == 0) {
        masterPeak.store(0.0f, std::memory_order_relaxed);
        padPeak.store(0.0f, std::memory_order_relaxed);
        const int cnt = trackCount.load(std::memory_order_acquire);
        for (int i = 0; i < cnt; ++i) tracks[(size_t)i]->peak.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const auto sr = sampleRate.load(std::memory_order_relaxed);
    const int count = trackCount.load(std::memory_order_acquire);
    const int activeSlot = activeSong.load(std::memory_order_acquire);
    const int64_t clickStart = transports[(size_t)activeSlot].position.load(std::memory_order_relaxed);
    for (int song = 0; song < maxSongs; ++song) {
    auto& transport = transports[(size_t)song];
    if (!transport.playing.load(std::memory_order_acquire)) continue;
    const auto callbackGeneration = transport.generation.load(std::memory_order_acquire);
    if (transport.fadeResetRequested.exchange(false, std::memory_order_acq_rel)) {
        transport.fadeGain = 1.0f; transport.fadeStep = 0.0f; transport.fadeRemaining = 0;
    }
    if (transport.fadeStopRequested.exchange(false, std::memory_order_acq_rel)) {
        transport.fadeRemaining = juce::jmax(1, (int) (sr * transport.fadeStopSeconds.load(std::memory_order_relaxed)));
        transport.fadeStep = -transport.fadeGain / (float) transport.fadeRemaining;
    }
    bool fadeFinished = false;
    for (int i = 0; i < frames; ++i) {
        if (transport.fadeRemaining > 0) {
            transport.fadeGain += transport.fadeStep;
            if (--transport.fadeRemaining == 0) {
                transport.fadeGain = 0.0f;
                fadeFinished = true;
            }
        }
        songFadeBuffer[(size_t) i] = transport.fadeGain;
    }
    const auto start = transport.position.load(std::memory_order_relaxed);
    int64_t cursor = start;
    bool sectionRepeats = transport.sectionLoop.load(std::memory_order_relaxed);
    int64_t sectionFrom = (int64_t)(transport.sectionStart.load(std::memory_order_relaxed) * sr);
    int64_t sectionTo = (int64_t)(transport.sectionEnd.load(std::memory_order_relaxed) * sr);
    bool validSection = transport.sectionPlayback.load(std::memory_order_acquire) && sectionTo > sectionFrom;
    bool sectionFinished = false;
    bool anySolo = false;
    for (int i = 0; i < count; ++i) if(tracks[(size_t)i]->songIndex==song) anySolo |= tracks[(size_t)i]->solo.load(std::memory_order_relaxed);
    const int chunkMax = (int) scratchL.size();
    for (int off = 0; off < frames;) {
        if (transport.generation.load(std::memory_order_acquire) != callbackGeneration) break;
        const bool hasQueuedJump = transport.queuedJump.load(std::memory_order_acquire);
        const int64_t queuedAt = hasQueuedJump ? (int64_t)(transport.queuedJumpAt.load(std::memory_order_relaxed) * sr) : 0;
        if (hasQueuedJump && cursor >= queuedAt) {
            const auto targetStart = transport.queuedJumpStart.load(std::memory_order_relaxed);
            const auto targetEnd = transport.queuedJumpEnd.load(std::memory_order_relaxed);
            transport.queuedJump.store(false, std::memory_order_release);
            if (targetEnd > targetStart) {
                cursor = (int64_t)(targetStart * sr);
                validSection = false;
                sectionRepeats = false;
                transport.sectionLoop.store(false, std::memory_order_relaxed);
                transport.sectionPlayback.store(false, std::memory_order_release);
                transport.repeat.store(false, std::memory_order_relaxed);
            }
        }
        if (validSection && cursor >= sectionTo) {
            if (sectionRepeats) cursor = sectionFrom;
            else {
                cursor = sectionTo;
                sectionFinished = true;
                break;
            }
        }
        int n = juce::jmin(chunkMax, frames - off);
        if (validSection) {
            const int64_t boundary = cursor < sectionFrom ? sectionFrom : sectionTo;
            n = (int)juce::jmin<int64_t>(n, juce::jmax<int64_t>(1, boundary - cursor));
        }
        if (hasQueuedJump && cursor < queuedAt)
            n = (int)juce::jmin<int64_t>(n, juce::jmax<int64_t>(1, queuedAt - cursor));
        float* bl = scratchL.data(); float* br = scratchR.data();
        float* ml = busL.data(); float* mr = busR.data();
        juce::FloatVectorOperations::clear(ml, n); juce::FloatVectorOperations::clear(mr, n);
        const int64_t base = cursor;
        for (int i = 0; i < count; ++i) {
            const auto& t = *tracks[(size_t)i];
            if(t.songIndex!=song)continue;
            if (!t.ready.load(std::memory_order_acquire) || !t.audio || t.mute.load(std::memory_order_relaxed)
                || (anySolo && !t.solo.load(std::memory_order_relaxed))) continue;
            const auto& src = *t.audio;
            const int ns = src.getNumSamples();
            const double step = t.sourceRate / sr;
            const float gain = t.gain.load(std::memory_order_relaxed);   // el fader Master actua sobre el bus Master
            const float pan = t.pan.load(std::memory_order_relaxed);
            const float gL = (pan > 0.0f ? 1.0f - pan : 1.0f) * gain, gR = (pan < 0.0f ? 1.0f + pan : 1.0f) * gain;
            const float* srcL = src.getReadPointer(0);
            const float* srcR = src.getNumChannels() > 1 ? src.getReadPointer(1) : srcL;
            bool any = false;
            if (std::abs(step - 1.0) < 1e-9) {
                if (base < ns) {
                    const int cnt = (int) juce::jmin<int64_t>(n, (int64_t) ns - base);
                    juce::FloatVectorOperations::copyWithMultiply(bl, srcL + base, gL, cnt);
                    juce::FloatVectorOperations::copyWithMultiply(br, srcR + base, gR, cnt);
                    if (cnt < n) { juce::FloatVectorOperations::clear(bl + cnt, n - cnt); juce::FloatVectorOperations::clear(br + cnt, n - cnt); }
                    any = true;
                }
            } else {
                for (int k = 0; k < n; ++k) {
                    const double si = (double)(base + k) * step;
                    if (si >= ns) { bl[k] = br[k] = 0.0f; continue; }
                    const int a = (int) si, b2 = juce::jmin(a + 1, ns - 1);
                    const float f = (float)(si - a);
                    bl[k] = (srcL[a] * (1.0f - f) + srcL[b2] * f) * gL;
                    br[k] = (srcR[a] * (1.0f - f) + srcR[b2] * f) * gR;
                    any = true;
                }
            }
            if (!any) continue;
            for (int k = 0; k < n; ++k) {
                const float fade = songFadeBuffer[(size_t) (off + k)];
                bl[k] *= fade;
                br[k] *= fade;
            }
            float blockPeak = juce::jmax(0.0f, juce::FloatVectorOperations::findMaximum(bl, n), -juce::FloatVectorOperations::findMinimum(bl, n));
            blockPeak = juce::jmax(blockPeak, juce::FloatVectorOperations::findMaximum(br, n), -juce::FloatVectorOperations::findMinimum(br, n));
            if (t.masterSend.load(std::memory_order_relaxed)) {            // envio al bus Master
                juce::FloatVectorOperations::add(ml, bl, n);
                juce::FloatVectorOperations::add(mr, br, n);
            }
            for (const auto& o : t.outs) routeTo(o, bl, br, out, outputs, off, n, 1.0f);   // salidas directas de la pista
            const float old = t.peak.load(std::memory_order_relaxed);
            const_cast<Track&>(t).peak.store(blockPeak > old ? blockPeak : old * 0.85f, std::memory_order_relaxed);
        }
        const float mg = master.load(std::memory_order_relaxed);
        for (const auto& o : masterOuts) routeTo(o, ml, mr, out, outputs, off, n, mg);       // salidas del Master
        cursor += n;
        off += n;
    }
    if (transport.generation.load(std::memory_order_acquire) == callbackGeneration) {
        if (fadeFinished || sectionFinished) {
            transport.sectionPlayback.store(false, std::memory_order_release);
            transport.queuedJump.store(false, std::memory_order_release);
            transport.playing.store(false, std::memory_order_release);
            transport.position.store(0, std::memory_order_release);
        } else {
            transport.position.store(cursor, std::memory_order_release);
        }
        const double songDuration=durations[(size_t)song].load(std::memory_order_relaxed);
        if (transport.playing.load(std::memory_order_acquire) && songDuration > 0.0 && (double)cursor / sr >= songDuration) {
            if (transport.repeat.load(std::memory_order_relaxed)) transport.position.store(0, std::memory_order_release);
            else transport.playing.store(false, std::memory_order_release);
        }
    }
    }
    // Click sigue el proyecto seleccionado; la mezcla de audio permite varios proyectos simultáneos.
    if (transports[(size_t)activeSlot].playing.load(std::memory_order_acquire) && clickEnabled.load(std::memory_order_relaxed)) {
        const double beatSamples = sr * 60.0 / juce::jmax(1.0, bpm.load(std::memory_order_relaxed));
        struct Dest { int d; float g; } dests[AudioEngine::maxOuts * 2];
        int nd = 0;
        for (const auto& o : clickOuts) {
            const int ch = o.ch.load(std::memory_order_relaxed);
            if (ch < 0 || o.mute.load(std::memory_order_relaxed)) continue;
            const float g = o.vol.load(std::memory_order_relaxed);
            const int d0 = mapOut(ch);
            if (d0 >= 0 && d0 < outputs && out[d0] != nullptr) dests[nd++] = { d0, g };
            if (!o.mono.load(std::memory_order_relaxed)) {
                const int d1 = mapOut(ch + 1);
                if (d1 >= 0 && d1 < outputs && out[d1] != nullptr) dests[nd++] = { d1, g };
            }
        }
        for (int n = 0; n < frames && nd > 0; ++n) {
            const int64_t absolute = clickStart + n;
            const int64_t beat = (int64_t)((double)absolute / beatSamples);
            const int64_t at = (int64_t)((double)beat * beatSamples);
            if (at != clickSample && absolute >= at) { clickSample = at; clickPhase = 0.0; }
            if (clickSample >= 0 && absolute - clickSample < sr * 0.035) {
                const float amp = (float)(0.22 * std::exp(-clickPhase * 70.0) * clickGain.load());
                for (int k = 0; k < nd; ++k) out[dests[k].d][n] += amp * dests[k].g;
                clickPhase += 1.0 / sr;
            }
        }
    }
    mixPads(out, outputs, frames);
    {
        float mp = 0.0f;
        for (int c = 0; c < outputs; ++c) if (out[c] != nullptr) mp = juce::jmax(mp, juce::FloatVectorOperations::findMaximum(out[c], frames), -juce::FloatVectorOperations::findMinimum(out[c], frames));
        const float oldM = masterPeak.load(std::memory_order_relaxed);
        masterPeak.store(mp > oldM ? mp : oldM * 0.85f, std::memory_order_relaxed);
    }
    for (int c = 0; c < outputs; ++c) if (out[c] != nullptr) juce::FloatVectorOperations::clip(out[c], out[c], -1.0f, 1.0f, frames);
}

// Reparte un bloque estereo (l, r) a una salida fisica: par estereo (con pan propio) o canal mono (suma L+R).
void AudioEngine::routeTo(const Track::HwOut& o, const float* l, const float* r, float* const* out, int outputs, int off, int n, float extra) const noexcept
{
    const int ch = o.ch.load(std::memory_order_relaxed);
    if (ch < 0 || o.mute.load(std::memory_order_relaxed)) return;
    const float v = o.vol.load(std::memory_order_relaxed) * extra, op = o.pan.load(std::memory_order_relaxed);
    if (o.mono.load(std::memory_order_relaxed)) {
        const int d = mapOut(ch);
        if (d < 0 || d >= outputs || out[d] == nullptr) return;
        juce::FloatVectorOperations::addWithMultiply(out[d] + off, l, 0.5f * v, n);
        juce::FloatVectorOperations::addWithMultiply(out[d] + off, r, 0.5f * v, n);
        return;
    }
    const int dl = mapOut(ch), dr = mapOut(ch + 1);
    const float pl = (op > 0.0f ? 1.0f - op : 1.0f) * v, pr = (op < 0.0f ? 1.0f + op : 1.0f) * v;
    if (dl >= 0 && dl < outputs && out[dl] != nullptr) juce::FloatVectorOperations::addWithMultiply(out[dl] + off, l, pl, n);
    if (dr >= 0 && dr < outputs && out[dr] != nullptr) juce::FloatVectorOperations::addWithMultiply(out[dr] + off, r, pr, n);
}
