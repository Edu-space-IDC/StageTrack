#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <map>
#include <vector>
#include "AudioEngine.h"

// Puente entre la interfaz web (copiada de REAPER Playback) y el motor de audio de StageTracks.
// Habla el mismo protocolo que la interfaz espera ("/_/": TRANSPORT, TRACK, GET/SET, EXTSTATE...),
// de modo que el HTML funciona sin cambios. Aqui vive tambien la biblioteca de canciones,
// las caratulas (tal cual las guarda REAPER Playback: una cadena data: por cancion) y la carga de .rpseq/.zip.
class PlayerBridge final
{
public:
    explicit PlayerBridge(AudioEngine&);
    ~PlayerBridge();

    // protocolo de la interfaz (se llama desde los hilos del servidor HTTP)
    juce::String handle(const juce::String& body);

    // acciones del anfitrion (ventana nativa)
    void openPaths(const juce::Array<juce::File>& files);
    void importAudio(const juce::Array<juce::File>& files);   // a la cancion activa
    void saveLibrary();
    juce::String takeHostScript();                             // JS pendiente para la pagina (avisos, rutas elegidas)
    void queueScript(const juce::String& js);
    void toast(const juce::String& message, bool error);
    void shutdown(bool savePending = true);
    bool saveSequenceFile(const juce::File&, juce::String& error);

private:
    struct SrcTrack {
        juce::String path, name, routing, color;
        double volume = 1.0, pan = 0.0;
        bool mute = false, solo = false;
    };   // ajustes restaurables de la pista importada
    struct Song
    {
        int slot = 0;                 // indice dentro del motor
        juce::String id;              // identificador estable
        juce::String name;
        juce::String artist;
        juce::String key;
        juce::String communityId;
        juce::String communityPackagePath;
        juce::String communityExtractionPath;
        juce::String libraryPath;
        juce::String hash;            // 16 hex, nombre del archivo de caratula
        double tempo = 92.0;
        double positionSeconds = 0.0;
        bool repeat = false;
        double masterVolume = 0.82;
        int timeSigNum = 4, timeSigDen = 4;
        bool tempoManual = false;
        bool tempoKnown = false;
        bool loading = false;
        juce::String cover;           // data:image/...;base64,...
        juce::String coverState = "n"; // n = sin caratula, s = con caratula, e = quitada a proposito
        juce::String coverVer = "0";
        std::vector<SrcTrack> sources;
    };

    // utilidades
    static juce::String hex(const juce::String&);
    static juce::String unhex(const juce::String&);
    static juce::String unesc(const juce::String&);
    static juce::String coverHash(const juce::String&);
    static juce::String imageToDataUri(const juce::File&);
    static juce::File appDir();
    static juce::File multitrackDir();
    static juce::String num(double v, int decimals = 6);
    static juce::String timeStr(double seconds);

    // protocolo
    void one(const juce::String& cmd, juce::String& out);
    void action(int id);
    void transportLine(juce::String& out);
    void beatPosLine(juce::String& out);
    void trackLine(int idx, juce::String& out);
    void setTrack(const juce::StringArray& p);
    void seekString(const juce::String& text);
    juce::String getExt(const juce::String& key);
    void setExt(const juce::String& key, const juce::String& value);
    void execCommand(const juce::String& cmd);
    void result(const juce::String& nonce, bool ok, const juce::String& msg);
    void applyCoverSet(const juce::String& raw);

    // claves publicadas
    juce::String songsString();
    juce::String playSongString();
    juce::String timelineString();
    juce::String waveRow(double duration);
    juce::String folderMapString();

    // routing de salidas (panel RT de la web)
    juce::String rtInfoString();
    juce::String rtHwString();
    void applyRset(const juce::String& value);
    void syncRouting(int trackId);          // copia el ruteo del motor a la biblioteca (para guardarlo)
    static juce::String chanLabel(int ch, bool mono);

    // biblioteca
    Song* activeSongPtr();
    int freeSlot() const;
    Song* addSong(const juce::String& name, bool activate);
    void activate(int index);
    void closeActive();
    bool deleteSongFromLibrary(int index, juce::String& error);
    void refreshCover(Song&);
    void saveCoverOverride(const juce::String& hash, const juce::String& value);
    void loadLibrary();
    void migrateLegacyLibrary();
    void addRecent(const juce::File&);
    void startImport(int slot, juce::Array<juce::File> files, juce::StringArray names, juce::StringArray routings = {},
                     juce::StringArray volumes = {}, juce::StringArray pans = {}, juce::StringArray mutes = {},
                     juce::StringArray solos = {}, juce::StringArray colors = {}, bool rememberSources = true, bool restorePosition = false);
    void loadSequence(const juce::File&);
    void appendSequenceSong(const juce::var& model, const juce::File& baseDirectory, bool activateIt);
    void runBackground(std::function<void()> fn);

    AudioEngine& engine;
    juce::CriticalSection lock;
    std::vector<Song> songs;
    std::array<std::atomic<uint64_t>, AudioEngine::maxSongs> slotGenerations {};
    int active = 0;
    std::map<juce::String, juce::String> ext;        // claves escritas por la web (WebControl/*)
    std::map<juce::String, juce::String> coverByVer;
    juce::StringArray recent;
    juce::String resultStr;
    std::atomic<int> structRev { 0 };
    int rtWatch = -1;                      // pista (1 = primera, 0 = master) cuyo routing mira la web; -1 = ninguna
    bool routingDirty = false;
    juce::String waveSig, waveCache;
    juce::CriticalSection outLock;
    juce::StringArray outbox;
    juce::ThreadPool pool { 1 };
    std::atomic<bool> closing { false };
    std::atomic<bool> projectDirty { false };
    std::thread autosaveThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayerBridge)
};
