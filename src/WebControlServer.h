#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <map>
#include "PlayerBridge.h"

// Servidor HTTP local (solo 127.0.0.1). Entrega la interfaz copiada de REAPER Playback y la API "/_/"
// que la interfaz usa (TRANSPORT, TRACK, GET/SET, EXTSTATE...), mas dos rutas para el anfitrion nativo:
//   POST /host/msg  -> mensajes de la pagina (abrir archivo, importar audio, ajustes de audio...)
//   GET  /host/poll -> JS pendiente para la pagina (avisos, rutas elegidas en dialogos nativos)
class WebControlServer final : private juce::Thread
{
public:
    explicit WebControlServer(PlayerBridge&);
    ~WebControlServer() override;
    bool setRemoteEnabled(bool enabled);
    bool isRemoteEnabled() const noexcept { return remoteEnabled.load(); }
    juce::String getRemoteInfo() const;
    bool isAvailable() const noexcept { return available.load(); }
    int getPort() const noexcept { return port; }
    juce::String getUrl() const { return "http://127.0.0.1:" + juce::String(port) + "/"; }
    std::function<void(const juce::String&)> onHostMessage;   // se llama desde un hilo del servidor
    std::function<void(const juce::File&, const juce::String&, const juce::String&, const juce::String&)> onCommunityDownload;
    std::function<juce::File()> onCommunityExportFile;
    std::function<void()> onCommunityExportCleanup;
private:
    void run() override;
    void serve(juce::StreamingSocket&);
    juce::String page() const;
    struct CommunityDownload {
        juce::File file;
        std::unique_ptr<juce::FileOutputStream> stream;
        juce::String name;
        int64_t expected = 0, written = 0;
    };
    PlayerBridge& bridge;
    juce::CriticalSection downloadLock;
    std::map<juce::String, CommunityDownload> communityDownloads;
    std::unique_ptr<juce::StreamingSocket> listener;
    juce::ThreadPool workers { 6 };
    int port = 0;
    std::atomic<bool> available { false };
    std::atomic<bool> remoteEnabled { false };
};
