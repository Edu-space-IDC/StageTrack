#pragma once
#include <JuceHeader.h>
#include "AudioEngine.h"
#include "PlayerBridge.h"
#include "WebControlServer.h"

// Ventana principal: aloja la interfaz de REAPER Playback (HTML) en un WebView2 y conecta la pagina con el motor.
class MainComponent final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;
    void importFiles(juce::Array<juce::File> files);
    void requestApplicationClose();
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void onHostMessage(const juce::String&);
    void preferAsio();
    void enableAllOutputs();      // abre TODAS las salidas del dispositivo para que el routing use canales fisicos
    juce::String deviceKey() const;
    void openAudioSettings();
    void pickToOpen();
    void pickToImport();
    void pickToSave();
    void pickToExportSequence(const juce::String& suggestedName);
    void downloadUpdateInstaller(const juce::String& url, bool autoInstall);
    static juce::File audioStateFile();

    AudioEngine engine;
    juce::AudioDeviceManager devices;
    std::unique_ptr<PlayerBridge> bridge;
    std::unique_ptr<WebControlServer> server;
    std::unique_ptr<juce::WebBrowserComponent> browser;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::CriticalSection communityExportLock;
    juce::File communityExportFile;
    juce::String lastDeviceKey;
    int reconnectCooldown = 0;
    bool hadDevice = false;
    bool updateDownloadInProgress = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
