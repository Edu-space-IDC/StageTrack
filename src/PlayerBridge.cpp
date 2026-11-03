#include "PlayerBridge.h"
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <future>
#include <stdexcept>
#include <thread>

namespace {
struct FnJob final : juce::ThreadPoolJob
{
    explicit FnJob(std::function<void()> f) : ThreadPoolJob("StageTracks task"), fn(std::move(f)) {}
    JobStatus runJob() override { fn(); return jobHasFinished; }
    std::function<void()> fn;
};

bool isAudioPath(const juce::File& f)
{
    static const char* exts[] = { "wav", "wave", "bwf", "aif", "aiff", "flac", "mp3", "ogg", "m4a", "wma" };
    for (auto* e : exts) if (f.hasFileExtension(e)) return true;
    return false;
}
}

// ------------------------------------------------------------------ utilidades
juce::String PlayerBridge::hex(const juce::String& s)
{
    return juce::String::toHexString(s.toRawUTF8(), (int) s.getNumBytesAsUTF8(), 0).toLowerCase();
}

juce::String PlayerBridge::unhex(const juce::String& h)
{
    if (h.isEmpty() || (h.length() % 2) != 0 || !h.containsOnly("0123456789abcdefABCDEF")) return {};
    juce::MemoryBlock mb;
    mb.loadFromHexString(h);
    return juce::String::fromUTF8((const char*) mb.getData(), (int) mb.getSize());
}

juce::String PlayerBridge::unesc(const juce::String& s)
{
    if (!s.contains("%")) return s;
    juce::MemoryOutputStream out;
    const auto* u = s.toRawUTF8();
    for (int i = 0; u[i] != 0;) {
        if (u[i] == '%' && std::isxdigit((unsigned char) u[i + 1]) && std::isxdigit((unsigned char) u[i + 2])) {
            const char h[3] = { u[i + 1], u[i + 2], 0 };
            out.writeByte((char) std::strtol(h, nullptr, 16));
            i += 3;
        } else out.writeByte(u[i++]);
    }
    return juce::String::fromUTF8((const char*) out.getData(), (int) out.getDataSize());
}

juce::String PlayerBridge::coverHash(const juce::String& s)
{
    uint32_t h1 = 5381u, h2 = 52711u;
    const auto* u = (const unsigned char*) s.toRawUTF8();
    const int n = (int) s.getNumBytesAsUTF8();
    for (int i = 0; i < n; ++i) {
        h1 = h1 * 33u + u[i];
        h2 = h2 * 131u + u[i] + (uint32_t) (i + 1);
    }
    return juce::String::formatted("%08x%08x", (unsigned) h1, (unsigned) h2);
}

juce::String PlayerBridge::imageToDataUri(const juce::File& f)
{
    auto img = juce::ImageFileFormat::loadFrom(f);
    if (img.isNull()) return {};
    const int maxSide = 512;
    const double scale = juce::jmin(1.0, maxSide / (double) juce::jmax(img.getWidth(), img.getHeight()));
    if (scale < 1.0)
        img = img.rescaled(juce::jmax(1, juce::roundToInt(img.getWidth() * scale)),
                           juce::jmax(1, juce::roundToInt(img.getHeight() * scale)), juce::Graphics::highResamplingQuality);
    juce::MemoryOutputStream ms;
    juce::JPEGImageFormat jpg;
    jpg.setQuality(0.86f);
    if (!jpg.writeImageToStream(img, ms)) return {};
    return "data:image/jpeg;base64," + juce::Base64::toBase64(ms.getData(), ms.getDataSize());
}

juce::File PlayerBridge::appDir()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("StageTracks");
}

juce::File PlayerBridge::multitrackDir()
{
    return appDir().getChildFile("Multitracks");
}

static juce::String safeLibraryName(juce::String name)
{
    name = name.replaceCharacters("\\/:*?\"<>|", "---------").trim().substring(0, 100);
    while (name.endsWithChar('.') || name.endsWithChar(' ')) name = name.dropLastCharacters(1);
    return name.isEmpty() ? "Sin título" : name;
}

juce::String PlayerBridge::num(double v, int decimals) { return juce::String(v, decimals); }

juce::String PlayerBridge::timeStr(double sec)
{
    sec = juce::jmax(0.0, sec);
    const int m = (int) (sec / 60.0);
    return juce::String::formatted("%d:%06.3f", m, sec - m * 60.0);
}

// ------------------------------------------------------------------ ciclo de vida
PlayerBridge::PlayerBridge(AudioEngine& e) : engine(e) { loadLibrary(); }

PlayerBridge::~PlayerBridge() { shutdown(); }

void PlayerBridge::shutdown(bool savePending)
{
    if (closing.exchange(true)) return;
    pool.removeAllJobs(true, 8000);
    if (savePending) saveLibrary();
}

void PlayerBridge::runBackground(std::function<void()> fn) { pool.addJob(new FnJob(std::move(fn)), true); }

void PlayerBridge::queueScript(const juce::String& js)
{
    const juce::ScopedLock sl(outLock);
    outbox.add(js);
    while (outbox.size() > 200) outbox.remove(0);
}

juce::String PlayerBridge::takeHostScript()
{
    const juce::ScopedLock sl(outLock);
    if (outbox.isEmpty()) return {};
    auto s = outbox.joinIntoString(";\n");
    outbox.clear();
    return s;
}

void PlayerBridge::toast(const juce::String& message, bool error)
{
    const auto m = juce::JSON::toString(juce::var(message), true);
    queueScript("(function(){var el=document.getElementById('pToast');if(!el)return;el.textContent=" + m
                + ";el.className=" + (error ? "'err'" : "''") + ";el.style.display='block';clearTimeout(window.__tt);"
                  "window.__tt=setTimeout(function(){el.style.display='none'}," + juce::String(error ? 6000 : 3500) + ");})()");
}

// ------------------------------------------------------------------ biblioteca
PlayerBridge::Song* PlayerBridge::activeSongPtr()
{
    return (active >= 0 && active < (int) songs.size()) ? &songs[(size_t) active] : nullptr;
}

int PlayerBridge::freeSlot() const
{
    for (int slot = 0; slot < AudioEngine::maxSongs; ++slot) {
        bool used = false;
        for (const auto& s : songs) if (s.slot == slot) { used = true; break; }
        if (!used) return slot;
    }
    return -1;
}

PlayerBridge::Song* PlayerBridge::addSong(const juce::String& name, bool activateIt)
{
    const juce::ScopedLock sl(lock);
    const int slot = freeSlot();
    if (slot < 0) return nullptr;
    Song s;
    s.slot = slot;
    // Los slots del motor se reutilizan cuando se cierra una canción. No
    // heredar secciones de la canción anterior que ocupaba ese slot.
    ext.erase("sections_" + juce::String(slot));
    s.id = juce::Uuid().toString();
    s.name = name;
    s.hash = coverHash("st:" + s.id);
    engine.clearSongDuration(slot);
    songs.push_back(std::move(s));
    refreshCover(songs.back());
    if (activateIt) activate((int) songs.size() - 1);
    ++structRev;
    return &songs.back();
}

void PlayerBridge::activate(int index)
{
    const juce::ScopedLock sl(lock);
    if (index < 0 || index >= (int) songs.size()) return;
    active = index;
    engine.setActiveSong(songs[(size_t) index].slot);
    engine.bpm.store(songs[(size_t) index].tempo);
    engine.setMaster((float) songs[(size_t) index].masterVolume);
    auto& selected = songs[(size_t) index];
    if (engine.getSongTrackCount(selected.slot) == 0 && !selected.loading && !selected.sources.empty()) {
        juce::Array<juce::File> files;
        juce::StringArray names, routings, volumes, pans, mutes, solos, colors;
        for (const auto& source : selected.sources) {
            const juce::File file(source.path);
            if (!file.existsAsFile()) continue;
            files.add(file); names.add(source.name); routings.add(source.routing);
            volumes.add(juce::String(source.volume)); pans.add(juce::String(source.pan));
            mutes.add(source.mute ? "1" : "0"); solos.add(source.solo ? "1" : "0"); colors.add(source.color);
        }
        if (!files.isEmpty()) startImport(selected.slot, std::move(files), std::move(names), std::move(routings),
                                          std::move(volumes), std::move(pans), std::move(mutes),
                                          std::move(solos), std::move(colors), false);
    }
    ++structRev;
}

void PlayerBridge::closeActive()
{
    const juce::ScopedLock sl(lock);
    auto* s = activeSongPtr();
    if (s == nullptr) return;
    engine.setPlaying(false);
    engine.releaseSong(s->slot);
    songs.erase(songs.begin() + active);
    active = juce::jlimit(0, juce::jmax(0, (int) songs.size() - 1), active);
    if (!songs.empty()) engine.setActiveSong(songs[(size_t) active].slot);
    ++structRev;
}

void PlayerBridge::refreshCover(Song& s)
{
    juce::String value, state = "n";
    const auto f = appDir().getChildFile("Covers").getChildFile(s.hash + ".txt");
    if (f.existsAsFile()) {
        const auto t = f.loadFileAsString().trim();
        if (t == "-") state = "e";
        else if (t.isNotEmpty()) { value = t; state = "s"; }
    }
    s.cover = value;
    s.coverState = state;
    s.coverVer = value.isNotEmpty() ? coverHash(value).substring(0, 10) : juce::String("0");
    if (value.isNotEmpty()) coverByVer[s.coverVer] = value;
}

void PlayerBridge::saveCoverOverride(const juce::String& hash, const juce::String& value)
{
    const auto dir = appDir().getChildFile("Covers");
    dir.createDirectory();
    dir.getChildFile(hash + ".txt").replaceWithText(value);
    for (auto& s : songs) if (s.hash == hash) refreshCover(s);
    ++structRev;
}

void PlayerBridge::addRecent(const juce::File& f)
{
    const juce::ScopedLock sl(lock);
    recent.removeString(f.getFullPathName(), true);
    recent.insert(0, f.getFullPathName());
    while (recent.size() > 10) recent.remove(recent.size() - 1);
}

void PlayerBridge::saveLibrary()
{
    const juce::ScopedLock sl(lock);
    juce::Array<juce::var> list;
    for (const auto& s : songs) {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", s.id);
        o->setProperty("name", s.name);
        o->setProperty("artist", s.artist);
        o->setProperty("libraryPath", s.libraryPath);
        o->setProperty("hash", s.hash);
        o->setProperty("tempo", s.tempo);
        o->setProperty("masterVolume", s.masterVolume);
        o->setProperty("timeSigNum", s.timeSigNum);
        o->setProperty("timeSigDen", s.timeSigDen);
        o->setProperty("tempoManual", s.tempoManual);
        o->setProperty("tempoKnown", s.tempoKnown);
        const auto sectionData = ext.find("sections_" + juce::String(s.slot));
        if (sectionData != ext.end()) o->setProperty("sections", sectionData->second);
        juce::Array<juce::var> files;
        for (const auto& t : s.sources) {
            auto* f = new juce::DynamicObject();
            f->setProperty("path", t.path);
            f->setProperty("name", t.name);
            if (t.routing.isNotEmpty()) f->setProperty("routing", t.routing);
            f->setProperty("volume", t.volume);
            f->setProperty("pan", t.pan);
            f->setProperty("mute", t.mute);
            f->setProperty("solo", t.solo);
            if (t.color.isNotEmpty()) f->setProperty("color", t.color);
            files.add(juce::var(f));
        }
        o->setProperty("tracks", files);
        if (s.libraryPath.isNotEmpty()) {
            auto* config = new juce::DynamicObject();
            config->setProperty("id", s.id); config->setProperty("name", s.name); config->setProperty("artist", s.artist);
            config->setProperty("tempo", s.tempo); config->setProperty("timeSigNum", s.timeSigNum); config->setProperty("timeSigDen", s.timeSigDen);
            config->setProperty("tracks", files);
            const auto sections = ext.find("sections_" + juce::String(s.slot));
            if (sections != ext.end()) config->setProperty("sections", sections->second);
            const auto coverFile = appDir().getChildFile("Covers").getChildFile(s.hash + ".txt");
            if (coverFile.existsAsFile()) {
                coverFile.copyFileTo(juce::File(s.libraryPath).getChildFile("cover.txt"));
                config->setProperty("coverFile", "cover.txt");
            }
            juce::File(s.libraryPath).createDirectory();
            juce::File(s.libraryPath).getChildFile("sequence.json").replaceWithText(juce::JSON::toString(juce::var(config), true));
        }
        list.add(juce::var(o));
    }
    juce::Array<juce::var> rec;
    for (const auto& r : recent) rec.add(r);
    auto* root = new juce::DynamicObject();
    root->setProperty("songs", list);
    root->setProperty("active", active);
    root->setProperty("recent", rec);
    root->setProperty("masterOuts", AudioEngine::outsToString(engine.masterOuts).isEmpty() ? juce::String("none") : AudioEngine::outsToString(engine.masterOuts));
    root->setProperty("clickOuts", AudioEngine::outsToString(engine.clickOuts).isEmpty() ? juce::String("none") : AudioEngine::outsToString(engine.clickOuts));
    appDir().createDirectory();
    appDir().getChildFile("library.json").replaceWithText(juce::JSON::toString(juce::var(root)));
    projectDirty.store(false, std::memory_order_relaxed);
}

void PlayerBridge::loadLibrary()
{
    const auto file = appDir().getChildFile("library.json");
    if (!file.existsAsFile()) return;
    const auto parsed = juce::JSON::parse(file);
    if (const auto m = parsed.getProperty("masterOuts", {}).toString(); m.isNotEmpty()) AudioEngine::outsFromString(engine.masterOuts, m == "none" ? juce::String() : m);
    if (const auto c = parsed.getProperty("clickOuts", {}).toString(); c.isNotEmpty()) AudioEngine::outsFromString(engine.clickOuts, c == "none" ? juce::String() : c);
    struct Pending {
        int slot; juce::Array<juce::File> files;
        juce::StringArray names, routings, volumes, pans, mutes, solos, colors;
    };
    std::vector<Pending> pending;
    if (auto* list = parsed.getProperty("songs", {}).getArray())
        for (const auto& v : *list) {
            if ((int) songs.size() >= AudioEngine::maxSongs) break;
            Song s;
            s.slot = (int) songs.size();
            s.id = v.getProperty("id", juce::Uuid().toString()).toString();
            s.name = v.getProperty("name", juce::String("Canción")).toString();
            s.artist = v.getProperty("artist", {}).toString();
            s.libraryPath = v.getProperty("libraryPath", {}).toString();
            s.hash = v.getProperty("hash", {}).toString();
            const auto storedTempo = v.getProperty("tempo", {});
            s.tempo = juce::jlimit(1.0, 999.0, std::round(storedTempo.isVoid() ? 92.0 : (double) storedTempo));
            s.masterVolume = juce::jlimit(0.0, 4.0, (double) v.getProperty("masterVolume", 0.82));
            s.timeSigNum = juce::jlimit(1, 32, (int) v.getProperty("timeSigNum", 4));
            s.timeSigDen = juce::jlimit(1, 32, (int) v.getProperty("timeSigDen", 4));
            s.tempoManual = (bool) v.getProperty("tempoManual", false);
            s.tempoKnown = (bool) v.getProperty("tempoKnown", false) || !storedTempo.isVoid();
            if (s.hash.length() != 16) s.hash = coverHash("st:" + s.id);
            const auto sectionData = v.getProperty("sections", {}).toString();
            if (sectionData.isNotEmpty()) ext["sections_" + juce::String(s.slot)] = sectionData;
            Pending p { s.slot, {}, {}, {}, {}, {}, {}, {}, {} };
            if (auto* tracks = v.getProperty("tracks", {}).getArray())
                for (const auto& t : *tracks) {
                    const juce::File f(t.getProperty("path", {}).toString());
                    SrcTrack source;
                    source.path = f.getFullPathName();
                    source.name = t.getProperty("name", {}).toString();
                    source.routing = t.getProperty("routing", {}).toString();
                    source.volume = juce::jlimit(0.0, 4.0, (double) t.getProperty("volume", 1.0));
                    source.pan = juce::jlimit(-1.0, 1.0, (double) t.getProperty("pan", 0.0));
                    source.mute = (bool) t.getProperty("mute", false);
                    source.solo = (bool) t.getProperty("solo", false);
                    source.color = t.getProperty("color", {}).toString();
                    s.sources.push_back(std::move(source));
                    if (f.existsAsFile()) {
                        p.files.add(f);
                        p.names.add(t.getProperty("name", {}).toString());
                        p.routings.add(t.getProperty("routing", {}).toString());
                        p.volumes.add(t.getProperty("volume", 1.0).toString());
                        p.pans.add(t.getProperty("pan", 0.0).toString());
                        p.mutes.add((bool) t.getProperty("mute", false) ? "1" : "0");
                        p.solos.add((bool) t.getProperty("solo", false) ? "1" : "0");
                        p.colors.add(t.getProperty("color", {}).toString());
                    }
                }
            songs.push_back(std::move(s));
            refreshCover(songs.back());
            pending.push_back(std::move(p));
        }
    if (auto* rec = parsed.getProperty("recent", {}).getArray())
        for (const auto& r : *rec) if (recent.size() < 10 && juce::File(r.toString()).existsAsFile()) recent.add(r.toString());
    active = juce::jlimit(0, juce::jmax(0, (int) songs.size() - 1), (int) parsed.getProperty("active", 0));
    if (!songs.empty()) engine.setActiveSong(songs[(size_t) active].slot);
    // la cancion activa se carga primero (queda lista para tocar enseguida); el resto va detras en segundo plano
    const int activeSlot = songs.empty() ? -1 : songs[(size_t) active].slot;
    for (auto& p : pending)
        if (p.slot == activeSlot && !p.files.isEmpty())
            startImport(p.slot, p.files, p.names, p.routings, p.volumes, p.pans, p.mutes, p.solos, p.colors, false);
    runBackground([] { AudioEngine::pruneCache(); });   // limpia .tmp y mantiene la cache bajo ~6 GB
}

// ------------------------------------------------------------------ importar / abrir
void PlayerBridge::startImport(int slot, juce::Array<juce::File> files, juce::StringArray names, juce::StringArray routings,
                               juce::StringArray volumes, juce::StringArray pans, juce::StringArray mutes,
                               juce::StringArray solos, juce::StringArray colors, bool rememberSources)
{
    {
        const juce::ScopedLock sl(lock);
        for (auto& song : songs) if (song.slot == slot) { song.loading = true; break; }
    }
    runBackground([this, slot, files = std::move(files), names = std::move(names), routings = std::move(routings),
                   volumes = std::move(volumes), pans = std::move(pans), mutes = std::move(mutes),
                   solos = std::move(solos), colors = std::move(colors), rememberSources]() mutable {
        const int total = files.size();
        // Varias pistas se decodifican a la vez (o se leen de la cache); el commit al motor es siempre en orden.
        const int window = 1; // evita que varias pistas grandes se decodifiquen simultáneamente en RAM
        std::vector<std::future<AudioEngine::Prepared>> futures((size_t) total);
        if (rememberSources) {
            juce::String songName, artist, songId;
            { const juce::ScopedLock sl(lock); for (const auto& s : songs) if (s.slot == slot) { songName = s.name; artist = s.artist; songId = s.id; break; } }
            const auto folder = multitrackDir().getChildFile(safeLibraryName(songName + (artist.isNotEmpty() ? " - " + artist : juce::String())) + "-" + songId.substring(0, 8));
            const auto audioFolder = folder.getChildFile("audio");
            audioFolder.createDirectory();
            juce::Array<juce::File> managed;
            for (int i = 0; i < files.size(); ++i) {
                const auto source = files[i];
                const auto baseName = juce::String::formatted("%02d-", i + 1) + safeLibraryName(source.getFileName());
                auto destination = audioFolder.getChildFile(baseName);
                for (int suffix = 2; destination.existsAsFile() && destination.getFullPathName() != source.getFullPathName(); ++suffix)
                    destination = audioFolder.getChildFile(juce::String::formatted("%02d-%02d-", i + 1, suffix) + safeLibraryName(source.getFileName()));
                if (source.getFullPathName() != destination.getFullPathName() && !destination.existsAsFile()) source.copyFileTo(destination);
                managed.add(destination.existsAsFile() ? destination : source);
            }
            files = std::move(managed);
            { const juce::ScopedLock sl(lock); for (auto& s : songs) if (s.slot == slot) { s.libraryPath = folder.getFullPathName(); break; } }
        }
        auto launch = [&](int i) {
            const juce::File file = files[i];
            const juce::String name = i < names.size() ? names[i] : juce::String();
            futures[(size_t) i] = std::async(std::launch::async, [this, file, name] { return engine.prepareFile(file, name); });
        };
        int next = 0;
        while (next < total && next < window && !closing) launch(next++);

        int ok = 0, fromCache = 0;
        juce::String errors;
        for (int i = 0; i < total; ++i) {
            if (!futures[(size_t) i].valid()) break;   // cierre del programa
            toast("Cargando audio… " + juce::String(i + 1) + "/" + juce::String(total), false);
            auto prepared = futures[(size_t) i].get();
            const bool cached = prepared.fromCache;
            juce::String err;
            if (engine.commitPrepared(std::move(prepared), slot, err)) {
                ++ok;
                if (cached) ++fromCache;
                const auto name = i < names.size() ? names[i] : juce::String();
                const juce::ScopedLock sl(lock);
                const auto routing = i < routings.size() ? routings[i] : juce::String();
                for (auto& s : songs)
                    if (s.slot == slot) {
                        const int trackIndex = engine.getSongTrackCount(slot) - 1;
                        if (auto* t = engine.findTrack(slot, trackIndex)) {
                            if (i < volumes.size()) t->gain.store((float) juce::jlimit(0.0, 4.0, volumes[i].getDoubleValue()));
                            if (i < pans.size()) t->pan.store((float) juce::jlimit(-1.0, 1.0, pans[i].getDoubleValue()));
                            if (i < mutes.size()) t->mute.store(mutes[i] == "1");
                            if (i < solos.size()) t->solo.store(solos[i] == "1");
                            if (i < colors.size()) {
                                auto color = colors[i].trim().removeCharacters("#");
                                if (color.length() == 6 && color.containsOnly("0123456789abcdefABCDEF"))
                                    t->color.store(color.getHexValue32() & 0xFFFFFF);
                            }
                        }
                        if (routing.isNotEmpty())
                            if (auto* t = engine.findTrack(slot, trackIndex)) AudioEngine::routingFromString(*t, routing);
                        if (rememberSources) {
                            const double volume = i < volumes.size() ? juce::jlimit(0.0, 4.0, volumes[i].getDoubleValue()) : 1.0;
                            const double pan = i < pans.size() ? juce::jlimit(-1.0, 1.0, pans[i].getDoubleValue()) : 0.0;
                            const bool mute = i < mutes.size() && mutes[i] == "1";
                            const bool solo = i < solos.size() && solos[i] == "1";
                            const auto color = i < colors.size() ? colors[i] : juce::String();
                            s.sources.push_back({ files[i].getFullPathName(), name, routing, color, volume, pan, mute, solo });
                        }
                        break;
                    }
                ++structRev;
            } else errors << files[i].getFileName() << ": " << err << "\n";
            if (next < total && !closing) launch(next++);
        }
        {
            const juce::ScopedLock sl(lock);
            for (auto& song : songs) if (song.slot == slot) { song.loading = false; break; }
        }
        saveLibrary();
        if (errors.isNotEmpty()) toast(errors.trim(), true);
        else {
            juce::String msg = ok == 1 ? juce::String("Audio listo (1 pista") : "Audio listo (" + juce::String(ok) + " pistas";
            if (fromCache > 0) msg << ", " << juce::String(fromCache) << " desde caché";
            toast(msg + ")", false);
        }
    });
}

void PlayerBridge::importAudio(const juce::Array<juce::File>& files)
{
    int slot = -1;
    {
        const juce::ScopedLock sl(lock);
        if (auto* s = activeSongPtr()) slot = s->slot;
    }
    if (slot < 0) {
        auto* s = addSong(files.isEmpty() ? juce::String("Nueva canción") : files[0].getFileNameWithoutExtension(), true);
        if (s == nullptr) { toast("Se alcanzó el límite de 32 canciones.", true); return; }
        slot = s->slot;
    }
    startImport(slot, files, {});
}

void PlayerBridge::openPaths(const juce::Array<juce::File>& files)
{
    runBackground([this, files] {
        juce::Array<juce::File> audio;
        for (const auto& f : files) {
            if (f.hasFileExtension("rpseq") || f.hasFileExtension("zip")) { loadSequence(f); addRecent(f); }
            else if (f.hasFileExtension("rpp")) toast("Los proyectos .rpp todavía no se pueden abrir en StageTracks. Usa .rpseq o .zip.", true);
            else if (isAudioPath(f)) audio.add(f);
            else toast("Formato no soportado: " + f.getFileName(), true);
        }
        if (!audio.isEmpty()) importAudio(audio);
        saveLibrary();
    });
}

void PlayerBridge::loadSequence(const juce::File& source)
{
    try {
        juce::File base = source.getParentDirectory();
        juce::File docFile = source;
        auto signature = source.createInputStream();
        char magic[4] = {};
        const bool isZip = signature != nullptr && signature->read(magic, 4) == 4 && magic[0] == 'P' && magic[1] == 'K';
        if (source.hasFileExtension("zip") || isZip) {
            const auto root = appDir().getChildFile("Sequences");
            const auto folder = root.getChildFile(source.getFileNameWithoutExtension().replaceCharacter(' ', '_') + "-" + juce::Uuid().toString());
            folder.createDirectory();
            juce::ZipFile zip(source);
            uint64_t total = 0;
            for (int i = 0; i < zip.getNumEntries(); ++i) {
                const auto* entry = zip.getEntry(i);
                if (entry == nullptr || entry->filename.isEmpty() || entry->isSymbolicLink) continue;
                auto rel = entry->filename.replaceCharacter('\\', '/');
                if (rel.endsWithChar('/')) continue;
                if (rel.startsWithChar('/') || rel.contains(":") || rel.contains("../") || rel == ".." || rel.endsWith("/..")) continue;
                if (entry->uncompressedSize < 0 || entry->uncompressedSize > (2LL << 30) || (total += (uint64_t) entry->uncompressedSize) > (8ULL << 30))
                    throw std::runtime_error("El paquete es demasiado grande para extraerlo con seguridad.");
                auto target = folder.getChildFile(rel);
                const auto full = target.getFullPathName(), prefix = folder.getFullPathName() + juce::File::getSeparatorString();
                if (!full.startsWithIgnoreCase(prefix)) continue;
                target.getParentDirectory().createDirectory();
                std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
                juce::FileOutputStream out(target);
                if (!in || !out.openedOk()) throw std::runtime_error("No se pudo extraer el paquete.");
                char buffer[65536];
                int bytes = 0;
                while ((bytes = in->read(buffer, (int) sizeof(buffer))) > 0)
                    if (!out.write(buffer, (size_t) bytes)) throw std::runtime_error("No se pudo escribir lo extraído.");
                out.flush();
            }
            juce::Array<juce::File> docs;
            folder.findChildFiles(docs, juce::File::findFiles, true, "*.rpseq");
            if (docs.isEmpty()) throw std::runtime_error("El ZIP no contiene ningún archivo .rpseq.");
            docFile = docs[0];
            base = docFile.getParentDirectory();
        }
        const auto parsed = juce::JSON::parse(docFile);
        auto* arr = parsed.getProperty("songs", {}).getArray();
        if (arr == nullptr || arr->isEmpty()) throw std::runtime_error("No es una secuencia válida o no tiene canciones.");
        const auto community = juce::JSON::parse(source.withFileExtension("community.json"));
        if (auto* metadata = community.getDynamicObject()) {
            for (auto& song : *arr) if (auto* object = song.getDynamicObject()) {
                if (metadata->hasProperty("name")) object->setProperty("name", metadata->getProperty("name"));
                if (metadata->hasProperty("artist")) object->setProperty("artist", metadata->getProperty("artist"));
                if (metadata->getProperty("cover").toString().startsWith("data:image/")) object->setProperty("coverData", metadata->getProperty("cover"));
            }
        }
        bool first = true;
        for (const auto& song : *arr) {
            appendSequenceSong(song, base, first);
            first = false;
        }
        toast(arr->size() == 1 ? "Abierto: " + arr->getReference(0).getProperty("name", {}).toString()
                               : "Secuencia abierta: " + juce::String(arr->size()) + " canciones", false);
    } catch (const std::exception& ex) {
        toast(juce::String("No se pudo abrir: ") + ex.what(), true);
    }
}

bool PlayerBridge::saveSequenceFile(const juce::File& destination, juce::String& error)
{
    auto* song = activeSongPtr();
    if (song == nullptr) { error = "Abre una canción antes de exportar."; return false; }
    if (song->sources.empty()) { error = "Importa al menos una pista de audio antes de compartir la secuencia."; return false; }

    auto* songObject = new juce::DynamicObject();
    songObject->setProperty("name", song->name);
    songObject->setProperty("artist", song->artist);
    songObject->setProperty("tempo", song->tempo);
    songObject->setProperty("timeSigNum", song->timeSigNum);
    songObject->setProperty("timeSigDen", song->timeSigDen);
    songObject->setProperty("masterVolume", song->masterVolume);
    juce::Array<juce::var> tracks;
    int index = 0;
    for (const auto& source : song->sources) {
        const juce::File media(source.path);
        if (!media.existsAsFile()) continue;
        ++index;
        auto relative = "audio/" + juce::String::formatted("%02d-", index) + media.getFileName();
        auto* track = new juce::DynamicObject();
        track->setProperty("name", source.name.isNotEmpty() ? source.name : media.getFileNameWithoutExtension());
        track->setProperty("volume", source.volume);
        track->setProperty("pan", source.pan);
        track->setProperty("mute", source.mute);
        track->setProperty("solo", source.solo);
        if (source.color.isNotEmpty()) track->setProperty("color", source.color);
        auto* item = new juce::DynamicObject();
        item->setProperty("file", relative);
        item->setProperty("pos", 0.0);
        juce::Array<juce::var> items; items.add(juce::var(item));
        track->setProperty("items", items);
        tracks.add(juce::var(track));
    }
    if (tracks.isEmpty()) { error = "No se encontraron los archivos de audio originales de esta secuencia."; return false; }
    songObject->setProperty("tracks", tracks);

    juce::Array<juce::var> regions;
    const auto sections = ext.find("sections_" + juce::String(song->slot));
    if (sections != ext.end()) {
        const auto lines = juce::StringArray::fromLines(sections->second);
        for (int i = 1; i < lines.size(); ++i) {
            const auto fields = juce::StringArray::fromTokens(lines[i], "|", "");
            if (fields.size() < 4 || fields[1].getDoubleValue() <= fields[0].getDoubleValue()) continue;
            auto* region = new juce::DynamicObject();
            region->setProperty("start", fields[0].getDoubleValue()); region->setProperty("end", fields[1].getDoubleValue());
            region->setProperty("color", fields[2]); region->setProperty("name", fields[3]);
            regions.add(juce::var(region));
        }
    }
    songObject->setProperty("regions", regions);

    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> songsToWrite; songsToWrite.add(juce::var(songObject));
    root->setProperty("songs", songsToWrite);
    juce::ZipFile::Builder archive;
    juce::MemoryBlock coverImage;
    juce::MemoryOutputStream coverOutput;
    juce::String coverPath;
    if (song->cover.startsWith("data:image/")) {
        const auto comma = song->cover.indexOfChar(',');
        if (comma > 0 && juce::Base64::convertFromBase64(coverOutput, song->cover.substring(comma + 1))) {
            coverImage.append(coverOutput.getData(), coverOutput.getDataSize());
            const auto extension = song->cover.startsWith("data:image/png") ? juce::String("png") : song->cover.startsWith("data:image/webp") ? juce::String("webp") : juce::String("jpg");
            coverPath = "cover." + extension;
            songObject->setProperty("cover", coverPath);
        }
    }
    const auto manifest = juce::JSON::toString(juce::var(root), true);
    archive.addEntry(std::make_unique<juce::MemoryInputStream>(manifest.toRawUTF8(), manifest.getNumBytesAsUTF8(), true), 6, "setlist.rpseq", juce::Time::getCurrentTime());
    index = 0;
    for (const auto& source : song->sources) {
        const juce::File media(source.path);
        if (!media.existsAsFile()) continue;
        const auto relative = "audio/" + juce::String::formatted("%02d-", ++index) + media.getFileName();
        archive.addFile(media, 0, relative);
    }
    if (coverPath.isNotEmpty()) archive.addEntry(std::make_unique<juce::MemoryInputStream>(std::move(coverImage)), 0, coverPath, juce::Time::getCurrentTime());
    auto output = destination.createOutputStream();
    if (output == nullptr || !output->openedOk()) { error = "No se pudo crear el archivo de secuencia."; return false; }
    if (!archive.writeToStream(*output, nullptr)) { error = "No se pudo escribir el paquete de secuencia."; return false; }
    output->flush();
    if (output->getStatus().failed()) { error = "No se pudo completar el archivo de secuencia."; return false; }
    return true;
}

void PlayerBridge::appendSequenceSong(const juce::var& model, const juce::File& baseDirectory, bool activateIt)
{
    Song* s;
    int slot;
    juce::String hash;
    {
        const juce::ScopedLock sl(lock);
        s = addSong(model.getProperty("name", juce::String("Canción")).toString(), activateIt);
        if (s == nullptr) throw std::runtime_error("Se alcanzó el límite de 32 canciones.");
        slot = s->slot;
        hash = s->hash;
        s->tempo = juce::jlimit(1.0, 999.0, std::round((double) model.getProperty("tempo", 92.0)));
        s->artist = model.getProperty("artist", {}).toString();
        s->tempoKnown = !model.getProperty("tempo", {}).isVoid();
        s->timeSigNum = juce::jlimit(1, 32, (int) model.getProperty("timeSigNum", 4));
        s->timeSigDen = juce::jlimit(1, 32, (int) model.getProperty("timeSigDen", 4));
        if (activateIt) engine.bpm.store(s->tempo);
    }
    // caratula empaquetada: se guarda como la elegida por el usuario (si no hay una ya)
    const auto coverData = model.getProperty("coverData", {}).toString();
    if (coverData.startsWith("data:image/")) {
        const juce::ScopedLock sl(lock);
        saveCoverOverride(hash, coverData);
    }
    const auto coverRel = model.getProperty("cover", {}).toString();
    if (coverRel.isNotEmpty() && coverData.isEmpty()) {
        const auto coverPath = baseDirectory.getChildFile(coverRel.replaceCharacter('\\', '/'));
        const auto full = coverPath.getFullPathName(), prefix = baseDirectory.getFullPathName() + juce::File::getSeparatorString();
        if (full.startsWithIgnoreCase(prefix) && coverPath.existsAsFile()) {
            const auto uri = imageToDataUri(coverPath);
            if (uri.isNotEmpty()) {
                const juce::ScopedLock sl(lock);
                saveCoverOverride(hash, uri);
            }
        }
    }
    juce::Array<juce::File> audioFiles;
    juce::StringArray trackNames, trackVolumes, trackPans, trackMutes, trackSolos, trackColors;
    if (auto* tracks = model.getProperty("tracks", {}).getArray())
        for (const auto& track : *tracks) {
            auto* items = track.getProperty("items", {}).getArray();
            if (items == nullptr || items->isEmpty()) continue;
            for (const auto& item : *items) {
                auto rel = item.getProperty("file", {}).toString();
                if (rel.isEmpty()) rel = item.getProperty("originalFile", {}).toString();
                if (rel.isEmpty()) continue;
                juce::File candidate(rel);
                if (!juce::File::isAbsolutePath(rel)) {
                    candidate = baseDirectory.getChildFile(rel.replaceCharacter('\\', '/'));
                    if (!candidate.getFullPathName().startsWithIgnoreCase(baseDirectory.getFullPathName() + juce::File::getSeparatorString())) continue;
                }
                if (!candidate.existsAsFile()) continue;
                audioFiles.add(candidate);
                trackNames.add(track.getProperty("name", juce::String()).toString());
                trackVolumes.add(track.getProperty("volume", 1.0).toString());
                trackPans.add(track.getProperty("pan", 0.0).toString());
                trackMutes.add((bool) track.getProperty("mute", false) ? "1" : "0");
                trackSolos.add((bool) track.getProperty("solo", false) ? "1" : "0");
                trackColors.add(track.getProperty("color", {}).toString());
                break; // un clip por carril, como en la primera version del importador
            }
        }
    if (auto* regions = model.getProperty("regions", {}).getArray()) {
        juce::StringArray rows;
        rows.add("S");
        for (const auto& region : *regions) {
            const double start = region.getProperty("start", 0.0);
            const double end = region.getProperty("end", 0.0);
            if (start < 0.0 || end <= start) continue;
            auto color = region.getProperty("color", {}).toString().trim().removeCharacters("#").toLowerCase();
            if (color.length() != 6 || !color.containsOnly("0123456789abcdef")) color = "ff9f43";
            auto name = region.getProperty("name", juce::String("Sección")).toString().replaceCharacters("|~\t\r\n", "     ").trim();
            if (name.isEmpty()) name = "Sección";
            rows.add(num(start, 4) + "|" + num(end, 4) + "|" + color + "|" + name);
        }
        const juce::ScopedLock sl(lock);
        ext["sections_" + juce::String(slot)] = rows.joinIntoString("\n");
        ++structRev;
    }
    if (!audioFiles.isEmpty())
        startImport(slot, std::move(audioFiles), std::move(trackNames), {}, std::move(trackVolumes), std::move(trackPans),
                    std::move(trackMutes), std::move(trackSolos), std::move(trackColors));
}

// ------------------------------------------------------------------ protocolo
juce::String PlayerBridge::handle(const juce::String& body)
{
    juce::String out;
    const juce::ScopedLock sl(lock);
    for (const auto& raw : juce::StringArray::fromTokens(body, ";", "")) {
        const auto cmd = raw.trim();
        if (cmd.isEmpty()) continue;
        try { one(cmd, out); } catch (...) {}
    }
    return out;
}

void PlayerBridge::one(const juce::String& cmd, juce::String& o)
{
    if (juce::CharacterFunctions::isDigit(cmd[0])) { action(cmd.getIntValue()); return; }
    if (cmd[0] == '_') return;
    const auto p = juce::StringArray::fromTokens(cmd, "/", "");
    if (p.isEmpty()) return;
    const auto& w = p[0];
    const int tracks = engine.getTrackCount();
    if (w == "TRANSPORT") { transportLine(o); return; }
    if (w == "BEATPOS") { beatPosLine(o); return; }
    if (w == "NTRACK") { o << "NTRACK\t" << (activeSongPtr() ? tracks : 0) << "\n"; return; }
    if (w == "TRACK") {
        if (activeSongPtr() == nullptr) return;
        if (p.size() > 1 && p[1].containsOnly("0123456789")) { trackLine(p[1].getIntValue(), o); return; }
        for (int i = 0; i <= tracks; ++i) trackLine(i, o);
        return;
    }
    if (w == "MARKER") { o << "MARKER_LIST\nMARKER_LIST_END\n"; return; }
    if (w == "REGION") { o << "REGION_LIST\nREGION_LIST_END\n"; return; }
    if (w == "GET") {
        if (p.size() < 2) return;
        if (p[1].containsOnly("0123456789")) {
            const int id = p[1].getIntValue();
            const int st = id == 40364 ? (engine.clickEnabled.load() ? 1 : 0) : id == 1068 ? (engine.getRepeat() ? 1 : 0) : 0;
            o << "CMDSTATE\t" << id << "\t" << st << "\n";
            return;
        }
        if (p[1] == "EXTSTATE" && p.size() >= 4) {
            const auto key = unesc(p[3]);
            o << "EXTSTATE\t" << p[2] << "\t" << key << "\t" << (p[2] == "WebControl" ? getExt(key) : juce::String()) << "\n";
        }
        return;
    }
    if (w == "SET") {
        if (p.size() < 3) return;
        if (p[1] == "POS") { engine.setPositionSeconds(unesc(p[2]).trimCharactersAtEnd("e").getDoubleValue()); return; }
        if (p[1] == "POS_STR") { seekString(unesc(juce::StringArray(p.strings.getRawDataPointer() + 2, p.size() - 2).joinIntoString("/"))); return; }
        if (p[1] == "REPEAT") {
            const double r = p[2].trimCharactersAtEnd("e").getDoubleValue();
            engine.setRepeat(r < 0 ? !engine.getRepeat() : r != 0.0);
            return;
        }
        if (p[1] == "EXTSTATE" || p[1] == "EXTSTATEPERSIST") {
            // SET/EXTSTATE/<seccion>/<clave>/<valor, que puede contener />
            const int i1 = cmd.indexOfChar('/'), i2 = cmd.indexOfChar(i1 + 1, '/'), i3 = cmd.indexOfChar(i2 + 1, '/');
            if (i1 < 0 || i2 < 0 || i3 < 0) return;
            const int i4 = cmd.indexOfChar(i3 + 1, '/');
            if (i4 < 0) return;
            if (cmd.substring(i2 + 1, i3) != "WebControl") return;
            setExt(unesc(cmd.substring(i3 + 1, i4)), unesc(cmd.substring(i4 + 1)));
            return;
        }
        if (p[1] == "TRACK") setTrack(p);
    }
}

void PlayerBridge::action(int id)
{
    switch (id) {
        case 1007:
            if (engine.getDurationSeconds() > 0 && engine.getPositionSeconds() >= engine.getDurationSeconds() - 0.01) engine.stopAndRewind();
            engine.setPlaying(true);
            break;
        case 1008: engine.setPlaying(false); break;
        case 1016: case 40667: engine.stopAndRewind(); break;
        case 40044: if (engine.isPlaying()) engine.stopAndRewind(); else action(1007); break;
        case 40073: if (engine.isPlaying()) engine.setPlaying(false); else action(1007); break;
        case 40042: engine.setPositionSeconds(0); break;
        case 1068: engine.setRepeat(!engine.getRepeat()); break;
        case 40364: engine.clickEnabled.store(!engine.clickEnabled.load()); break;
        default: break;
    }
}

void PlayerBridge::transportLine(juce::String& o)
{
    const bool has = activeSongPtr() != nullptr;
    const double pos = has ? engine.getPositionSeconds() : 0.0;
    const int state = !has ? 0 : engine.isPlaying() ? 1 : (pos > 0.001 ? 2 : 0);
    o << "TRANSPORT\t" << state << "\t" << num(pos) << "\t" << (engine.getRepeat() ? 1 : 0) << "\t" << timeStr(pos) << "\t" << timeStr(pos) << "\n";
}

void PlayerBridge::beatPosLine(juce::String& o)
{
    const bool has = activeSongPtr() != nullptr;
    const double pos = has ? engine.getPositionSeconds() : 0.0;
    const int state = !has ? 0 : engine.isPlaying() ? 1 : (pos > 0.001 ? 2 : 0);
    const double tempo = juce::jmax(1.0, engine.bpm.load());
    const auto* song = activeSongPtr();
    const int numr = song != nullptr ? song->timeSigNum : 4;
    const int den = song != nullptr ? song->timeSigDen : 4;
    const double quarters = pos * tempo / 60.0;
    const double measureQ = numr * 4.0 / den;
    const int measure = (int) std::floor(quarters / measureQ);
    const double inMeasure = (quarters - measure * measureQ) * den / 4.0;
    o << "BEATPOS\t" << state << "\t" << num(pos) << "\t" << num(quarters) << "\t" << measure << "\t" << num(inMeasure) << "\t" << numr << "\t" << den << "\n";
}

void PlayerBridge::trackLine(int idx, juce::String& o)
{
    juce::String name; int flags = 0; double vol; float peak; float pan = 0.0f; int color = 0;
    if (idx == 0) {
        name = "MASTER"; vol = engine.getMaster(); peak = engine.getMasterPeak();
    } else {
        if (idx - 1 >= engine.getTrackCount()) return;
        const auto& t = engine.getTrack(idx - 1);
        name = t.name; vol = t.gain.load(); peak = t.peak.load(); pan = t.pan.load(); color = t.color.load();
        if (t.mute.load()) flags |= 8;
        if (t.solo.load()) flags |= 16;
    }
    int db = peak > 1.0f ? 10 : (int) std::lround(200.0 * std::log10(juce::jmax((double) peak, 1e-6)));
    db = juce::jmax(-600, db);
    name = name.replaceCharacters("|~\t\r\n", "     ");
    const int hwOuts = idx == 0 ? 1 : AudioEngine::activeOutCount(engine.getTrack(idx - 1));
    o << "TRACK\t" << idx << "\t" << name << "\t" << flags << "\t" << num(vol, 7) << "\t" << num(pan, 4) << "\t" << db << "\t" << db
      << "\t1\t3\t0\t0\t" << hwOuts << "\t" << color << "\n";
}

void PlayerBridge::setTrack(const juce::StringArray& p)
{
    if (p.size() < 5 || !p[2].containsOnly("0123456789")) return;
    const int idx = p[2].getIntValue();
    const auto what = p[3];
    if (what == "SEND") return;
    const double v = p[4].trimCharactersAtEnd("e").getDoubleValue();
    if (idx == 0) {
        if (what == "VOL") {
            const float gain = (float) juce::jlimit(0.0, 4.0, v);
            engine.setMaster(gain);
            if (auto* song = activeSongPtr()) {
                song->masterVolume = gain;
                projectDirty.store(true, std::memory_order_relaxed);
            }
        }
        return;
    }
    if (idx - 1 >= engine.getTrackCount()) return;
    auto& t = engine.getTrack(idx - 1);
    if (what == "VOL") t.gain.store((float) juce::jlimit(0.0, 4.0, v));
    else if (what == "PAN") t.pan.store((float) juce::jlimit(-1.0, 1.0, v));
    else if (what == "MUTE") t.mute.store(v < 0 ? !t.mute.load() : v != 0.0);
    else if (what == "SOLO") t.solo.store(v < 0 ? !t.solo.load() : v != 0.0);
    else return;
    if (auto* song = activeSongPtr()) {
        const auto sourceIndex = (size_t) (idx - 1);
        if (sourceIndex < song->sources.size()) {
            auto& source = song->sources[sourceIndex];
            source.volume = t.gain.load(std::memory_order_relaxed);
            source.pan = t.pan.load(std::memory_order_relaxed);
            source.mute = t.mute.load(std::memory_order_relaxed);
            source.solo = t.solo.load(std::memory_order_relaxed);
        }
        projectDirty.store(true, std::memory_order_relaxed);
    }
}

void PlayerBridge::seekString(const juce::String& textIn)
{
    const auto text = textIn.trim();
    if (text.isEmpty()) return;
    const auto dots = juce::StringArray::fromTokens(text, ".", "");
    if (dots.size() == 3 && text.containsOnly("0123456789.")) {   // compas.tiempo.fraccion
        const double beatSec = 60.0 / juce::jmax(1.0, engine.bpm.load());
        const int m = dots[0].getIntValue(), b = dots[1].getIntValue();
        const double frac = ("0." + dots[2]).getDoubleValue();
        engine.setPositionSeconds(((m - 1) * 4 + (b - 1) + frac) * beatSec);
        return;
    }
    if (text.contains(":")) {
        double total = 0;
        for (const auto& part : juce::StringArray::fromTokens(text, ":", "")) total = total * 60.0 + part.getDoubleValue();
        engine.setPositionSeconds(total);
        return;
    }
    engine.setPositionSeconds(text.getDoubleValue());
}

// ------------------------------------------------------------------ claves ExtState
juce::String PlayerBridge::getExt(const juce::String& key)
{
    auto* s = activeSongPtr();
    if (key == "helper") return "studio-v22";
    if (key == "project") return s ? s->name : juce::String();
    if (key == "projectpath") return s ? hex("stagetracks:" + s->id) : juce::String("-");
    if (key == "projectdirty") return projectDirty.load(std::memory_order_relaxed) ? "1" : "0";
    if (key == "recent") { juce::StringArray r; for (auto& x : recent) r.add(hex(x)); return r.joinIntoString("|"); }
    if (key == "songs") return songsString();
    if (key == "metainfo") return s ? num(s->tempo, 2) + "|" + juce::String(s->timeSigNum) + "|" + juce::String(s->timeSigDen) : "92|4|4";
    if (key == "playsong") return playSongString();
    if (key == "timeline") return timelineString();
    if (key == "timelinesig") return s ? juce::String(s->slot) + "-" + juce::String(structRev.load()) + "-" + juce::String(engine.getTrackCount()) : juce::String("0");
    if (key == "timelinepos") {
        if (!s) return "0.000|0|0.000";
        const double pos = engine.getPositionSeconds();
        return num(pos, 3) + "|" + (engine.isPlaying() ? "1" : "0") + "|" + num(engine.isPlaying() ? 0.0 : pos, 3);
    }
    if (key == "foldermap") return folderMapString();
    if (key == "padmeter") {
        const float peak = engine.getPadPeak();
        const int db = peak > 1.0f ? 10 : (int) std::lround(200.0 * std::log10(juce::jmax((double) peak, 1e-6)));
        return juce::String(juce::jmax(-600, db));
    }
    if (key == "rtinfo") return rtInfoString();
    if (key == "rthw") return rtHwString();
    if (key == "uploadack") return {};
    if (key == "result") return resultStr;
    if (key.startsWith("cv_")) {
        const auto it = coverByVer.find(key.substring(3));
        return it != coverByVer.end() ? it->second : juce::String();
    }
    const auto it = ext.find(key);
    return it != ext.end() ? it->second : juce::String();
}

juce::String PlayerBridge::songsString()
{
    juce::StringArray rows;
    for (const auto& s : songs)
        rows.add(hex(s.name) + "~" + hex("stagetracks:" + s.id) + "~" + s.hash + "~" + s.coverState + "~" + s.coverVer + "~" + num(s.tempo, 2) + "~" + juce::String(s.timeSigNum) + "~" + juce::String(s.timeSigDen) + "~" + (s.tempoManual ? "1" : "0") + "~" + (s.tempoKnown ? "1" : "0") + "~" + hex(s.artist));
    return juce::String(songs.empty() ? 0 : active) + "|" + rows.joinIntoString("|");
}

juce::String PlayerBridge::playSongString()
{
    if (activeSongPtr() == nullptr) return "-1|0";
    int shown = active;
    if (!engine.isPlaying()) {
        shown = -1;
        for (int i = 0; i < (int) songs.size(); ++i)
            if (engine.isSongPlaying(songs[(size_t)i].slot)) { shown = i; break; }
        if (shown < 0 && engine.getPositionSeconds() > 0.001) shown = active;
    }
    if (shown < 0) return "-1|0";
    const int slot = songs[(size_t)shown].slot;
    const bool playing = engine.isSongPlaying(slot);
    return juce::String(shown) + (playing ? "|1" : "|2");
}

juce::String PlayerBridge::folderMapString()
{
    const int n = activeSongPtr() ? engine.getTrackCount() : 0;
    if (n == 0) return "-";
    juce::StringArray rows;
    for (int i = 1; i <= n; ++i) rows.add(juce::String(i) + "~0~0~~0");
    return rows.joinIntoString("|");
}

juce::String PlayerBridge::timelineString()
{
    if (activeSongPtr() == nullptr) return "30.0000|";
    juce::StringArray rows;
    const int n = engine.getTrackCount();
    double maxEnd = engine.getDurationSeconds();
    for (int i = 0; i < n; ++i) {
        const auto& t = engine.getTrack(i);
        const int idx = i + 1;
        const double len = engine.getTrackSeconds(t);
        const auto name = t.name.replaceCharacters("|~\t\r\n", "     ");
        rows.add("T~" + juce::String(idx) + "~0~" + hex(name));
        if (len > 0) rows.add("I~" + juce::String(idx) + "~0.0000~" + num(len, 4) + "~" + hex(name));
        maxEnd = juce::jmax(maxEnd, len);
    }
    const double dur = juce::jmax(maxEnd, 30.0);
    const auto saved = ext.find("sections_" + juce::String(activeSongPtr() ? activeSongPtr()->slot : -1));
    if (saved != ext.end()) {
        const auto lines = juce::StringArray::fromLines(saved->second);
        for (int i = 1; i < lines.size(); ++i) {
            const auto fields = juce::StringArray::fromTokens(lines[i], "|", "");
            if (fields.size() < 4) continue;
            const double start = fields[0].getDoubleValue(), end = fields[1].getDoubleValue();
            if (end <= start) continue;
            rows.add("R~" + num(start, 4) + "~" + num(end, 4) + "~" + hex(fields[3]) + "~" + fields[2]);
        }
    }
    const auto wave = waveRow(dur);
    if (wave.isNotEmpty()) rows.add(wave);
    return num(dur, 4) + "|" + rows.joinIntoString("|");
}

// Onda de la linea de tiempo: suma de los picos de todas las pistas (calculados al importar), 600-2000 columnas.
// Formato: W~columnas~estereo(0/1)~canalL~canalR, cada columna = 2 hex de maximo + 2 hex de minimo.
juce::String PlayerBridge::waveRow(double dur)
{
    const int n = engine.getTrackCount();
    const auto sig = juce::String(active) + "-" + juce::String(structRev.load()) + "-" + juce::String(n) + "-" + num(dur, 3);
    if (sig == waveSig) return waveCache;

    const int bins = juce::jlimit(600, 2000, (int) std::floor(dur * 25.0));
    const double dt = dur / bins;
    std::vector<double> mx((size_t) bins, 0.0);
    bool any = false;
    for (int i = 0; i < n; ++i) {
        const auto& t = engine.getTrack(i);
        const double len = engine.getTrackSeconds(t);
        const int pk = (int) t.peaks.size();
        if (len <= 0.0 || pk <= 0) continue;
        any = true;
        for (int b = 0; b < bins; ++b) {
            const double t0 = b * dt;
            if (t0 >= len) break;
            const int i0 = juce::jlimit(0, pk - 1, (int) std::floor(t0 / len * pk));
            const int i1 = juce::jlimit(i0, pk - 1, (int) std::ceil(juce::jmin(t0 + dt, len) / len * pk) - 1);
            float peak = 0.0f;
            for (int k = i0; k <= i1; ++k) peak = juce::jmax(peak, t.peaks[(size_t) k]);
            mx[(size_t) b] += peak;
        }
    }
    juce::String row;
    if (any) {
        double top = 0.0;
        for (double v : mx) top = juce::jmax(top, v);
        if (top > 0.0) {
            const double scale = 0.98 / top;
            juce::String chan;
            chan.preallocateBytes((size_t) bins * 4 + 8);
            for (double v : mx) {
                const int q = juce::jlimit(0, 255, (int) std::lround(v * scale * 255.0));
                const auto h = juce::String::formatted("%02x", q);
                chan << h << h;
            }
            row = "W~" + juce::String(bins) + "~0~" + chan + "~";
        }
    }
    waveSig = sig;
    waveCache = row;
    return row;
}

void PlayerBridge::setExt(const juce::String& key, const juce::String& value)
{
    if (key == "cmd") { execCommand(value); return; }
    if (key == "fadestop") { engine.fadeStopAndRewind(); return; }
    if (key == "padfile") {
        const auto fields = juce::StringArray::fromTokens(value, "|", "");
        if (fields.size() >= 3) {
            const int bank = fields.size() >= 4 ? fields[1].getIntValue() : 1;
            const int offset = fields.size() >= 4 ? 2 : 1;
            const int note = fields[offset].getIntValue();
            const float gain = fields[offset + 1].getFloatValue();
            if (fields[0] == "on") engine.setPad(note, true, gain, bank);
            else if (fields[0] == "off") engine.setPad(note, false, gain, bank);
            else if (fields[0] == "vol")
                engine.setPad(note, engine.getActivePad() == note && engine.getActivePadBank() == bank, gain, bank);
        }
        return;
    }
    if (key == "sectionloop") {
        const auto fields = juce::StringArray::fromTokens(value, "|", "");
        if (fields.size() >= 4) {
            const double from = fields[0].getDoubleValue(), to = fields[1].getDoubleValue();
            const bool loop = fields[2].getIntValue() != 0, enabled = fields[3].getIntValue() != 0;
            engine.setSectionPlayback(from, to, loop, enabled && to > from);
        } else engine.setSectionPlayback(0.0, 0.0, false, false);
        ext[key] = value;
        return;
    }
    if (key == "rwatch") {
        const int id = value.containsOnly("0123456789") && value.isNotEmpty() ? value.getIntValue() : -1;
        rtWatch = id;
        if (id < 0 && routingDirty) { routingDirty = false; saveLibrary(); }
        return;
    }
    if (key == "rset") { applyRset(value); return; }
    if (key == "coverset") { applyCoverSet(value); return; }
    if (key == "upload") { ext["uploadack"] = {}; return; }
    ext[key] = value;
}

void PlayerBridge::applyCoverSet(const juce::String& raw)
{
    // <hash16>|<id>|<total>
    const auto parts = juce::StringArray::fromTokens(raw, "|", "");
    if (parts.size() != 3 || parts[0].length() != 16 || !parts[0].containsOnly("0123456789abcdefABCDEF")) return;
    const auto hash = parts[0].toLowerCase();
    const auto id = parts[1];
    const int total = parts[2].getIntValue();
    if (total < 1 || total > 200) return;
    juce::String all;
    for (int i = 0; i < total; ++i) {
        const auto it = ext.find("cvc_" + id + "_" + juce::String(i));
        if (it == ext.end() || it->second.isEmpty()) return;
        all << it->second;
    }
    for (int i = 0; i < total; ++i) ext.erase("cvc_" + id + "_" + juce::String(i));
    const auto value = unhex(all);
    if (value.isNotEmpty() && value.length() <= 600000) saveCoverOverride(hash, value);
}

// ------------------------------------------------------------------ ordenes del helper: cmd = tipo_arg_arg_nonce
void PlayerBridge::result(const juce::String& nonce, bool ok, const juce::String& msg)
{
    resultStr = nonce + "|" + (ok ? "1" : "0") + "|" + msg.replaceCharacter('|', ' ');
}

void PlayerBridge::execCommand(const juce::String& cmd)
{
    const auto fields = juce::StringArray::fromTokens(cmd, "_", "");
    auto field = [&](int i) { return i < fields.size() ? fields[i] : juce::String(); };
    const auto commandTail = cmd.fromLastOccurrenceOf("_", false, false);
    const auto commandNonce = commandTail.isNotEmpty() && commandTail.containsOnly("0123456789") ? commandTail : juce::String("0");
    if (field(0) == "songmeta") {
        auto* song = activeSongPtr();
        if (!song) { result(commandNonce, false, "Abre una canción primero."); return; }
        const double tempo = std::round(field(1).getDoubleValue());
        const int numerator = field(2).getIntValue(), denominator = field(3).getIntValue();
        const bool manual = field(4).getIntValue() != 0;
        if (tempo < 40.0 || tempo > 240.0 || numerator < 1 || numerator > 32 ||
            (denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8 && denominator != 16 && denominator != 32)) {
            result(commandNonce, false, "BPM o compás fuera de rango."); return;
        }
        if (manual || !song->tempoManual) {
            song->tempo = tempo;
            song->timeSigNum = numerator;
            song->timeSigDen = denominator;
            song->tempoManual = manual;
            song->tempoKnown = true;
            engine.bpm.store(tempo, std::memory_order_relaxed);
            ++structRev;
            saveLibrary();
        }
        result(commandNonce, true, "BPM y compás guardados.");
        return;
    }
    if (field(0) == "rep") {
        const bool on = field(1).getIntValue() != 0;
        const double start = field(2).getDoubleValue(), end = field(3).getDoubleValue();
        engine.setRepeat(on);
        engine.setSectionPlayback(start, end, true, on && end > start);
        result(commandNonce, true, "Loop actualizado.");
        return;
    }
    if (field(0) == "section" || field(0) == "frame") {
        engine.cancelQueuedSectionJump();
        const double start = field(1).getDoubleValue(), end = field(2).getDoubleValue();
        if (end <= start) { result(commandNonce, false, "Rango de seccion no valido."); return; }
        if (field(0) == "frame") {
            engine.setSectionPlayback(start, end, engine.getRepeat(), true);
        } else if (engine.isPlaying()) {
            engine.setSectionPlayback(0.0, 0.0, false, false);
            engine.setRepeat(false);
            engine.setPositionSeconds(start);
        } else {
            engine.setSectionPlayback(0.0, 0.0, false, false);
            engine.setPositionSeconds(start);
        }
        result(commandNonce, true, "Seccion actualizada.");
        return;
    }
    if (field(0) == "jumpqueue") {
        const double boundary = field(1).getDoubleValue();
        const double start = field(2).getDoubleValue(), end = field(3).getDoubleValue();
        if (end > start) {
            engine.setRepeat(false);
            engine.setSectionPlayback(0.0, 0.0, false, false);
            engine.queueSectionJump(boundary, start, end);
            result(commandNonce, true, "Seccion en cola hasta el final de la actual.");
        } else result(commandNonce, false, "Rango de salto no valido.");
        return;
    }
    if (field(0) == "jump") {
        engine.cancelQueuedSectionJump();
        const double start = field(1).getDoubleValue(), end = field(2).getDoubleValue();
        if (end > start) {
            engine.setSectionPlayback(start, end, field(3).getIntValue() != 0, true);
            engine.setPositionSeconds(start);
            engine.setPlaying(true);
            result(commandNonce, true, "Salto de seccion aplicado.");
        } else result(commandNonce, false, "Rango de salto no valido.");
        return;
    }
    if (field(0) == "jumpcancel") {
        engine.cancelQueuedSectionJump();
        result(commandNonce, true, "Salto cancelado.");
        return;
    }
    if (field(0) == "secsave") {
        const auto tail = cmd.fromLastOccurrenceOf("_", false, false);
        const auto payloadHex = cmd.fromFirstOccurrenceOf("secsave_", false, false).upToLastOccurrenceOf("_", false, false);
        const auto nonce = tail.containsOnly("0123456789") ? tail : juce::String("0");
        const auto payload = unhex(payloadHex);
        if (!activeSongPtr() || !payload.startsWith("S\n")) { result(nonce, false, "No se pudieron guardar las secciones."); return; }
        ext["sections_" + juce::String(activeSongPtr()->slot)] = payload;
        ++structRev;
        saveLibrary();
        result(nonce, true, "Secciones guardadas.");
        return;
    }
    const auto tail = cmd.fromLastOccurrenceOf("_", false, false);
    const auto nonce = (tail.isNotEmpty() && tail.containsOnly("0123456789")) ? tail : juce::String("0");
    const auto parts = juce::StringArray::fromTokens(cmd, "_", "");
    const auto kind = parts.isEmpty() ? juce::String() : parts[0];
    auto P = [&](int i) { return i < parts.size() ? parts[i] : juce::String(); };
    auto* s = activeSongPtr();
    const auto none = juce::String("Abre o crea una canción primero.");
    const auto unsupported = juce::String("No disponible en StageTracks todavía.");

    if (kind == "stopson") {
        const int i = P(1).getIntValue();
        if (!P(1).containsOnly("0123456789") || i < 0 || i >= (int) songs.size()) { result(nonce, false, "Ese proyecto ya no está abierto."); return; }
        engine.stopSongAndRewind(songs[(size_t)i].slot);
        result(nonce, true, "Reproducción detenida.");
    } else if (kind == "tab" || kind == "tabstop") {
        const int i = P(1).getIntValue();
        if (!P(1).containsOnly("0123456789") || i < 0 || i >= (int) songs.size()) { result(nonce, false, "Esa canción ya no está abierta."); return; }
        if (kind == "tabstop") engine.stopAndRewind();
        activate(i);
        saveLibrary();
        result(nonce, true, "Canción cambiada.");
    } else if (kind == "close") {
        if (!s) { result(nonce, false, "No hay canción abierta."); return; }
        closeActive();
        saveLibrary();
        result(nonce, true, "Canción cerrada.");
    } else if (kind == "new") {
        if (addSong("Sin título", true) == nullptr) { result(nonce, false, "Se alcanzó el límite de 32 canciones."); return; }
        saveLibrary();
        result(nonce, true, "Nueva canción creada.");
    } else if (kind == "open") {
        const auto path = unhex(P(1));
        if (!juce::File(path).existsAsFile()) { result(nonce, false, "No se encontró el archivo."); return; }
        openPaths({ juce::File(path) });
        result(nonce, true, "Abriendo…");
    } else if (kind == "saveas") {
        if (!s) { result(nonce, false, none); return; }
        auto path = juce::File(unhex(P(1)));
        if (!path.hasFileExtension("rpseq")) path = path.withFileExtension("rpseq");
        juce::String error;
        if (!saveSequenceFile(path, error)) { result(nonce, false, error); return; }
        saveLibrary();
        toast("Secuencia exportada con sus pistas y carátula.", false);
        result(nonce, true, "Secuencia exportada: " + path.getFileName());
    } else if (kind == "save") {
        saveLibrary();
        result(nonce, true, "Guardado.");
    } else if (kind == "discard") {
        result(nonce, true, "No hay cambios pendientes.");
    } else if (kind == "projrename") {
        if (!s) { result(nonce, false, "No hay canción abierta."); return; }
        auto name = unhex(P(1)).replaceCharacters("\\/:*?\"<>|", "         ").trim();
        if (name.isEmpty()) { result(nonce, false, "El nombre no puede quedar vacío."); return; }
        s->name = name;
        ++structRev;
        saveLibrary();
        result(nonce, true, "Renombrada: " + name);
    } else if (kind == "import") {
        if (!s) { result(nonce, false, none); return; }
        const juce::File f(unhex(P(1)));
        if (!f.existsAsFile()) { result(nonce, false, "No se encontró el archivo para importar."); return; }
        startImport(s->slot, { f }, {});
        result(nonce, true, "Importando audio…");
    } else if (kind == "del") {
        const int i = P(1).getIntValue();
        if (!s || i < 1 || i > engine.getTrackCount()) { result(nonce, false, "No se encontró esa pista (o es el master)."); return; }
        const auto name = engine.getTrack(i - 1).name;
        engine.removeTrack(i - 1);
        if (i - 1 < (int) s->sources.size()) s->sources.erase(s->sources.begin() + (i - 1));
        ++structRev;
        saveLibrary();
        result(nonce, true, "Pista eliminada: " + name);
    } else if (kind == "rename") {
        const int i = P(1).getIntValue();
        const auto name = unhex(P(2)).trim();
        if (!s || i < 1 || i > engine.getTrackCount()) { result(nonce, false, "No se encontró esa pista (o es el master)."); return; }
        if (name.isEmpty()) { result(nonce, false, "El nombre de la pista no puede quedar vacío."); return; }
        auto& t = engine.getTrack(i - 1);
        const auto old = t.name;
        t.name = name;
        if (i - 1 < (int) s->sources.size()) s->sources[(size_t) (i - 1)].name = name;
        ++structRev;
        saveLibrary();
        result(nonce, true, "Pista renombrada: " + old + " → " + name);
    } else if (kind == "color") {
        const int i = P(1).getIntValue();
        if (!s || i < 1 || i > engine.getTrackCount()) { result(nonce, false, "No se encontró esa pista (o es el master)."); return; }
        const auto hexColor = P(2).isEmpty() ? juce::String("0") : P(2);
        auto& t = engine.getTrack(i - 1);
        if (hexColor == "0") t.color.store(0);
        else if (hexColor.length() == 6 && hexColor.containsOnly("0123456789abcdefABCDEF")) t.color.store(hexColor.getHexValue32() & 0xFFFFFF);
        else { result(nonce, false, "Color no válido."); return; }
        if (i - 1 < (int) s->sources.size())
            s->sources[(size_t) (i - 1)].color = hexColor == "0" ? juce::String() : hexColor;
        projectDirty.store(true, std::memory_order_relaxed);
        result(nonce, true, "Color cambiado: " + t.name);
    } else if (kind == "rhw" || kind == "rhk") {   // rhw_<pista>_<canal> (pista 0 = Master) / rhk_<canal> (click): añade una salida
        const bool click = kind == "rhk";
        const int id = click ? 0 : P(1).getIntValue(), ch = P(click ? 1 : 2).getIntValue();
        if (id < 0 || (id > 0 && (!s || id > engine.getTrackCount()))) { result(nonce, false, "No se encontró esa pista."); return; }
        auto& arr = click ? engine.clickOuts : (id == 0 ? engine.masterOuts : engine.getTrack(id - 1).outs);
        for (auto& o : arr)
            if (o.ch.load() < 0) {
                o.mono.store(false); o.mute.store(false); o.vol.store(1.0f); o.pan.store(0.0f);
                o.ch.store(juce::jlimit(0, AudioEngine::maxDeviceChannels - 1, ch));
                if (id > 0) syncRouting(id);
                saveLibrary();
                result(nonce, true, "Salida añadida: " + chanLabel(ch, false));
                return;
            }
        result(nonce, false, "Máximo " + juce::String(AudioEngine::maxOuts) + " salidas.");
    } else if (kind == "rdel") {           // rdel_<pista>_<h|k>_<ranura>
        const int id = P(1).getIntValue(), slot = P(3).getIntValue();
        const bool okKind = P(2) == "h" || (P(2) == "k" && id == 0);
        if (!okKind || id < 0 || (id > 0 && (!s || id > engine.getTrackCount())) || slot < 0 || slot >= AudioEngine::maxOuts) { result(nonce, false, "No se pudo quitar esa salida."); return; }
        auto& arr = P(2) == "k" ? engine.clickOuts : (id == 0 ? engine.masterOuts : engine.getTrack(id - 1).outs);
        arr[(size_t) slot].ch.store(-1);
        if (id > 0) syncRouting(id);
        saveLibrary();
        result(nonce, true, "Salida quitada.");
    } else if (kind == "rmain") {          // rmain_<pista>_<0|1>: envio de la pista al bus Master
        const int id = P(1).getIntValue();
        if (!s || id < 1 || id > engine.getTrackCount()) { result(nonce, false, "No se encontró esa pista."); return; }
        engine.getTrack(id - 1).masterSend.store(P(2).getIntValue() != 0);
        syncRouting(id);
        saveLibrary();
        result(nonce, true, "Envío al Master actualizado.");
    } else if (kind == "radd") {
        result(nonce, false, "StageTracks no tiene envíos entre pistas: usa las salidas directas de cada pista.");
    } else {
        // add, move, folder*...
        if (!kind.isEmpty()) result(nonce, true, unsupported);
    }
}

// ------------------------------------------------------------------ routing de salidas
juce::String PlayerBridge::chanLabel(int ch, bool mono)
{
    return mono ? juce::String(ch + 1) + " (mono)" : juce::String(ch + 1) + "/" + juce::String(ch + 2);
}

juce::String PlayerBridge::rtHwString()
{
    if (rtWatch < 0) return {};
    auto names = engine.getOutputNames();
    const int n = juce::jmax(names.size(), engine.getDeviceOutputCount());
    juce::String o = juce::String(rtWatch);
    for (int i = 0; i < n; ++i) {
        auto name = i < names.size() ? names[i] : juce::String();
        if (name.isEmpty()) name = "Salida " + juce::String(i + 1);
        o << "|" << i << "~" << name.replaceCharacters("|~\t\r\n", "     ");
    }
    return o;
}

juce::String PlayerBridge::rtInfoString()
{
    if (rtWatch < 0) return {};
    juce::String o;
    // cat~i~otra~nombre~vol~pan~mute~modo~origen~destino   (destino: canal | 1024 si es mono)
    auto addItems = [&](char cat, const std::array<AudioEngine::Track::HwOut, AudioEngine::maxOuts>& outs) {
        for (int j = 0; j < AudioEngine::maxOuts; ++j) {
            const auto& out = outs[(size_t) j];
            const int ch = out.ch.load();
            if (ch < 0) continue;
            const bool mono = out.mono.load();
            o << "|" << juce::String::charToString((juce::juce_wchar) cat) << "~" << j << "~-1~" << chanLabel(ch, mono) << "~"
              << num(out.vol.load(), 5) << "~" << num(out.pan.load(), 5) << "~" << (out.mute.load() ? 1 : 0) << "~0~-1~" << (ch | (mono ? 1024 : 0));
        }
    };
    if (rtWatch == 0) {                                   // Master: sus salidas ('h') y las del click ('k')
        o = "0|1";
        addItems('h', engine.masterOuts);
        addItems('k', engine.clickOuts);
    } else if (activeSongPtr() != nullptr && rtWatch <= engine.getTrackCount()) {
        const auto& t = engine.getTrack(rtWatch - 1);
        o = juce::String(rtWatch) + "|" + (t.masterSend.load() ? "1" : "0");
        addItems('h', t.outs);                            // salidas directas de la pista
    } else {
        o = juce::String(rtWatch) + "|1";
    }
    return o;
}

void PlayerBridge::syncRouting(int trackId)
{
    auto* s = activeSongPtr();
    if (s == nullptr || trackId < 1 || trackId > engine.getTrackCount() || trackId - 1 >= (int) s->sources.size()) return;
    s->sources[(size_t) (trackId - 1)].routing = AudioEngine::routingToString(engine.getTrack(trackId - 1));
}

// rset = <pista>_<cat>_<ranura>_<v:..,p:..,m:..,c:..>_<marca>     (pista 0 = Master: cat h = Master, k = click)
void PlayerBridge::applyRset(const juce::String& value)
{
    const auto f = juce::StringArray::fromTokens(value, "_", "");
    if (f.size() < 4 || (f[1] != "h" && f[1] != "k")) return;
    const int id = f[0].getIntValue(), slot = f[2].getIntValue();
    if (id < 0 || id > engine.getTrackCount() || slot < 0 || slot >= AudioEngine::maxOuts) return;
    AudioEngine::Track::HwOut* target = nullptr;
    if (id == 0) target = f[1] == "k" ? &engine.clickOuts[(size_t) slot] : &engine.masterOuts[(size_t) slot];
    else if (f[1] == "h") target = &engine.getTrack(id - 1).outs[(size_t) slot];
    if (target == nullptr) return;
    auto& o = *target;
    if (o.ch.load() < 0) return;
    for (const auto& kv : juce::StringArray::fromTokens(f[3], ",", "")) {
        const auto key = kv.upToFirstOccurrenceOf(":", false, false);
        const double v = kv.fromFirstOccurrenceOf(":", false, false).getDoubleValue();
        if (key == "v") o.vol.store((float) juce::jlimit(0.0, 4.0, v));
        else if (key == "p") o.pan.store((float) juce::jlimit(-1.0, 1.0, v));
        else if (key == "m") o.mute.store(v != 0.0);
        else if (key == "c") {
            const int code = (int) std::lround(v);
            o.mono.store((code & 1024) != 0);
            o.ch.store(juce::jlimit(0, AudioEngine::maxDeviceChannels - 1, code & 1023));
        }
    }
    if (id > 0) syncRouting(id);
    routingDirty = true;     // se guarda al cerrar el panel (no en cada movimiento del fader)
}
