#include "MainComponent.h"
#include <thread>

namespace { constexpr juce::uint32 kBg = 0xff07080a; }

juce::File MainComponent::audioStateFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("StageTracks").getChildFile("audio.xml");
}

void MainComponent::preferAsio()
{
    for (auto* type : devices.getAvailableDeviceTypes()) {
        if (type->getTypeName() != "ASIO" || type->getDeviceNames(false).isEmpty()) continue;
        const auto previous = devices.getCurrentAudioDeviceType();
        devices.setCurrentAudioDeviceType("ASIO", true);
        if (devices.getCurrentAudioDevice() == nullptr)      // el driver no abrio: se vuelve al anterior
            devices.setCurrentAudioDeviceType(previous, true);
        return;
    }
}

juce::String MainComponent::deviceKey() const
{
    if (auto* d = devices.getCurrentAudioDevice()) return d->getTypeName() + "|" + d->getName();
    return {};
}

void MainComponent::enableAllOutputs()
{
    auto* dev = devices.getCurrentAudioDevice();
    if (dev == nullptr) return;
    const int n = juce::jlimit(2, AudioEngine::maxDeviceChannels, dev->getOutputChannelNames().size());
    juce::BigInteger all;
    all.setRange(0, n, true);
    auto setup = devices.getAudioDeviceSetup();
    if (!setup.useDefaultOutputChannels && setup.outputChannels == all) return;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels = all;
    devices.setAudioDeviceSetup(setup, true);
}

MainComponent::MainComponent()
{
    setSize(1200, 820);

    // 1) audio: se recupera el dispositivo/canales elegidos la ultima vez
    std::unique_ptr<juce::XmlElement> saved;
    if (audioStateFile().existsAsFile()) saved = juce::XmlDocument::parse(audioStateFile());
    devices.initialise(0, 2, saved.get(), true);
    if (saved == nullptr) { preferAsio(); enableAllOutputs(); }   // primera vez: ASIO si existe, y todas sus salidas disponibles
    lastDeviceKey = deviceKey();   // lo ya guardado se respeta; solo un dispositivo NUEVO abre todas sus salidas
    devices.addAudioCallback(&engine);
    devices.addChangeListener(this);
    hadDevice = devices.getCurrentAudioDevice() != nullptr;

    // 2) puente + servidor local con la interfaz de REAPER Playback
    bridge = std::make_unique<PlayerBridge>(engine);
    server = std::make_unique<WebControlServer>(*bridge);
    if (server->isAvailable()) {
        server->onHostMessage = [safe = juce::Component::SafePointer<MainComponent>(this)](const juce::String& msg) {
            juce::MessageManager::callAsync([safe, msg] { if (safe != nullptr) safe->onHostMessage(msg); });
        };
        server->onCommunityDownload = [safe = juce::Component::SafePointer<MainComponent>(this)](const juce::File& file, const juce::String& name, const juce::String& artist, const juce::String& cover, const juce::String& communityId, const juce::String& key, const juce::String& bpm, const juce::String& meter) {
            juce::MessageManager::callAsync([safe, file, name, artist, cover, communityId, key, bpm, meter] {
                if (safe != nullptr && safe->bridge != nullptr) {
                    auto* sidecar = new juce::DynamicObject();
                    sidecar->setProperty("name", name); sidecar->setProperty("artist", artist); sidecar->setProperty("cover", cover); sidecar->setProperty("id", communityId); sidecar->setProperty("key", key); sidecar->setProperty("bpm", bpm); sidecar->setProperty("meter", meter);
                    file.withFileExtension("community.json").replaceWithText(juce::JSON::toString(juce::var(sidecar)));
                    safe->bridge->openPaths({ file });
                    safe->bridge->toast("Secuencia descargada. Preparando el setlist…", false);
                }
            });
        };
        server->onCommunityExportFile = [safe = juce::Component::SafePointer<MainComponent>(this)] {
            if (safe == nullptr) return juce::File();
            const juce::ScopedLock lock(safe->communityExportLock);
            return safe->communityExportFile;
        };
        server->onCommunityExportCleanup = [safe = juce::Component::SafePointer<MainComponent>(this)] {
            if (safe == nullptr) return;
            juce::File file;
            { const juce::ScopedLock lock(safe->communityExportLock); file = safe->communityExportFile; safe->communityExportFile = {}; }
            if (file.existsAsFile()) file.deleteFile();
        };
        using Opt = juce::WebBrowserComponent::Options;
        const auto userData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                  .getChildFile("StageTracks").getChildFile("WebView");
        userData.createDirectory();
        browser = std::make_unique<juce::WebBrowserComponent>(
            Opt{}.withBackend(Opt::Backend::webview2)
                 .withKeepPageLoadedWhenBrowserIsHidden()
                 .withUserScript(R"JS(
                    (function() {
                      window.STAGETRACKS_NATIVE = true;
                      const stop = event => { event.preventDefault(); event.stopPropagation(); };
                      document.addEventListener('contextmenu', stop, true);
                      document.addEventListener('wheel', event => {
                        const overTimeline = event.target instanceof Element && event.target.closest('#tl');
                        if (event.ctrlKey && !overTimeline) stop(event);
                      }, { capture: true, passive: false });
                      document.addEventListener('keydown', event => {
                        const key = event.key.toLowerCase();
                        const reload = key === 'f5' || ((event.ctrlKey || event.metaKey) && key === 'r');
                        const zoom = (event.ctrlKey || event.metaKey) && (key === '+' || key === '=' || key === '-' || key === '0');
                        const devtools = key === 'f12' || ((event.ctrlKey || event.metaKey) && event.shiftKey && (key === 'i' || key === 'j'));
                        if (reload || zoom || devtools) stop(event);
                      }, true);
                      document.addEventListener('gesturestart', stop, true);
                      document.addEventListener('gesturechange', stop, true);
                    })();
                 )JS")
                 .withWinWebView2Options(Opt::WinWebView2{}.withUserDataFolder(userData)
                                                         .withBackgroundColour(juce::Colour(kBg))));
        addAndMakeVisible(*browser);
        browser->goToURL(server->getUrl() + "?launch=" + juce::String((int64_t)juce::Time::getCurrentTime().toMilliseconds()));
    }
    if (!hadDevice) bridge->toast("No hay salida de audio. Usa el botón «Audio» para elegir una.", true);
    startTimer(1000);
}

MainComponent::~MainComponent()
{
    stopTimer();
    devices.removeChangeListener(this);
    browser.reset();
    server.reset();
    if (bridge) bridge->shutdown();
    devices.removeAudioCallback(&engine);
    devices.closeAudioDevice();
    bridge.reset();
}

void MainComponent::importFiles(juce::Array<juce::File> files)
{
    if (bridge && !files.isEmpty()) bridge->openPaths(files);
}

void MainComponent::requestApplicationClose()
{
    if (browser != nullptr)
        browser->evaluateJavascript("window.requestAppClose && window.requestAppClose()");
    else
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(kBg));
    if (browser == nullptr) {
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(16.0f));
        g.drawFittedText("No se pudo iniciar el servidor local de la interfaz (puertos 47821-47840 ocupados).",
                         getLocalBounds().reduced(24), juce::Justification::centred, 3);
    }
}

void MainComponent::resized()
{
    if (browser != nullptr) browser->setBounds(getLocalBounds());
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // dispositivo nuevo (cambio de driver/tarjeta): se habilitan todas sus salidas para poder rutear a cualquiera
    if (const auto key = deviceKey(); key.isNotEmpty() && key != lastDeviceKey) { lastDeviceKey = key; enableAllOutputs(); }

    // guarda el dispositivo y los canales de salida para el proximo inicio
    if (auto xml = devices.createStateXml()) {
        audioStateFile().getParentDirectory().createDirectory();
        xml->writeTo(audioStateFile());
    }
}

void MainComponent::timerCallback()
{
    const bool hasDevice = devices.getCurrentAudioDevice() != nullptr;
    if (hadDevice && !hasDevice) {
        engine.setPlaying(false);
        bridge->toast("Se perdió la salida de audio. Reintentando automáticamente…", true);
        reconnectCooldown = 0;
    }
    if (!hasDevice && --reconnectCooldown <= 0) {
        reconnectCooldown = 3;
        devices.restartLastAudioDevice();
        if (devices.getCurrentAudioDevice() != nullptr) {
            bridge->toast("Audio reconectado: " + devices.getCurrentAudioDevice()->getName(), false);
        }
    }
    hadDevice = devices.getCurrentAudioDevice() != nullptr;
}

void MainComponent::onHostMessage(const juce::String& msg)
{
    if (msg == "logout" || msg == "openAudioSettings") openAudioSettings();
    else if (msg == "quitNow") juce::JUCEApplication::getInstance()->systemRequestedQuit();
    else if (msg == "quitDiscard") {
        if (bridge) bridge->shutdown(false);
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
    else if (msg.startsWith("saveExitResult|")) {
        const auto fields = juce::StringArray::fromTokens(msg, "|", "");
        if (fields.size() >= 2 && fields[1] == "1")
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        else if (bridge)
            bridge->toast("No se guardaron los cambios. La aplicación seguirá abierta.", true);
    }
    else if (msg == "enableWebControl") server->setRemoteEnabled(true);
    else if (msg == "disableWebControl") server->setRemoteEnabled(false);
    else if (msg.startsWith("connectWeb|")) {
        const auto target = msg.fromFirstOccurrenceOf("|", false, false).trim();
        if (target.startsWithIgnoreCase("http://") && browser != nullptr) browser->goToURL(target);
        else bridge->toast("Usa una dirección Control web que empiece por http://", true);
    }
    else if (msg.startsWith("downloadUpdate|")) {
        const auto fields = juce::StringArray::fromTokens(msg, "|", "");
        if (fields.size() >= 2) downloadUpdateInstaller(fields[1].trim(), fields.size() >= 3 && fields[2] == "1");
    }
    else if (msg == "openFeedbackPage") {
        juce::URL("https://stagetrack-project.vercel.app/?feedback=1#feedback").launchInDefaultBrowser();
    }
    else if (msg == "openProject") pickToOpen();
    else if (msg == "importMedia") pickToImport();
    else if (msg == "saveProject") pickToSave();
    else if (msg == "exportCommunitySequence") {
        const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                .getChildFile("StageTracks").getChildFile("CommunityUploads");
        folder.createDirectory();
        juce::File oldExport;
        { const juce::ScopedLock lock(communityExportLock); oldExport = communityExportFile; communityExportFile = {}; }
        if (oldExport.existsAsFile()) oldExport.deleteFile();
        // The payload is a ZIP archive (.rpseq + audio/ + optional cover),
        // so give it the portable extension expected by other DAWs.
        const auto exportFile = folder.getChildFile(juce::Uuid().toString().removeCharacters("{}") + ".zip");
        juce::String error;
        if (!bridge->saveSequenceFile(exportFile, error)) {
            exportFile.deleteFile();
            bridge->queueScript("window.receiveCommunityExport(false," + juce::JSON::toString(juce::var(error)) + ")");
        } else {
            { const juce::ScopedLock lock(communityExportLock); communityExportFile = exportFile; }
            bridge->queueScript("window.receiveCommunityExport(true," + juce::String(exportFile.getSize()) + ")");
        }
    }
    else if (msg.startsWith("exportSequence|")) {
        const auto payload = juce::JSON::parse(msg.fromFirstOccurrenceOf("|", false, false));
        const auto suggestedName = payload.getProperty("name", juce::String("StageTracks-sequence")).toString();
        pickToExportSequence(suggestedName);
    }
}

void MainComponent::downloadUpdateInstaller(const juce::String& url, bool autoInstall)
{
    if (updateDownloadInProgress) { if (bridge) bridge->toast("La descarga de la actualización ya está en curso.", false); return; }
    if (!url.startsWithIgnoreCase("https://github.com/Edu-space-IDC/StageTrack/releases/")) {
        if (bridge) bridge->toast("No se pudo descargar la actualización: enlace no reconocido.", true);
        return;
    }
    updateDownloadInProgress = true;
    showUpdateProgress(0, "Conectando con GitHub…");
    const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                            .getChildFile("StageTracks").getChildFile("Updates");
    folder.createDirectory();
    const auto installer = folder.getChildFile("StageTracks-Setup-windows-x64.exe");
    auto safe = juce::Component::SafePointer<MainComponent>(this);
    std::thread([safe, url, installer, autoInstall] {
        juce::String error;
        auto reportProgress = [safe](int percent, const juce::String& message) {
            juce::MessageManager::callAsync([safe, percent, message] {
                if (safe != nullptr) safe->showUpdateProgress(percent, message);
            });
        };
        const auto temporary = installer.getSiblingFile(installer.getFileName() + ".part");
        int responseCode = 0;
        const auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs(30000).withStatusCode(&responseCode).withNumRedirectsToFollow(10);
        auto input = juce::URL(url).createInputStream(options);
        if (input == nullptr || responseCode < 200 || responseCode >= 300) {
            error = "No se pudo conectar con GitHub para descargar el instalador.";
            reportProgress(-2, error);
        } else {
            auto output = std::make_unique<juce::FileOutputStream>(temporary);
            if (!output->openedOk()) { error = "No se pudo guardar el instalador en la carpeta de actualizaciones."; reportProgress(-2, error); }
            else {
                char buffer[256 * 1024];
                int bytes = 0; int64_t downloaded = 0; const int64_t totalBytes = input->getTotalLength(); int lastPercent = -1;
                reportProgress(0, "Descargando instalador... 0%");
                while ((bytes = input->read(buffer, (int) sizeof(buffer))) > 0) {
                    if (!output->write(buffer, (size_t) bytes)) { error = "La descarga del instalador se interrumpió al guardar el archivo."; break; }
                    downloaded += bytes;
                    if (totalBytes > 0) {
                        const int percent = (int) juce::jlimit<int64_t>(0, 99, downloaded * 100 / totalBytes);
                        if (percent != lastPercent) { lastPercent = percent; reportProgress(percent, "Descargando instalador... " + juce::String(percent) + "%"); }
                    } else if (downloaded % (1024 * 1024) < bytes) {
                        reportProgress(-1, "Descargando instalador... " + juce::String(downloaded / (1024 * 1024)) + " MB");
                    }
                }
                output->flush();
                output.reset();
                if (error.isEmpty() && temporary.getSize() < 1024 * 1024) error = "El instalador descargado está incompleto.";
                if (error.isNotEmpty()) reportProgress(-2, error);
                if (error.isEmpty()) {
                    installer.deleteFile();
                    if (!temporary.moveFileTo(installer)) { error = "No se pudo preparar el instalador descargado."; reportProgress(-2, error); }
                }
            }
        }
        if (error.isNotEmpty()) temporary.deleteFile();
        juce::MessageManager::callAsync([safe, installer, error, autoInstall] {
            if (safe == nullptr) return;
            safe->updateDownloadInProgress = false;
            if (error.isNotEmpty()) { if (safe->bridge) safe->bridge->toast(error, true); return; }
            safe->showUpdateProgress(100, "Descarga completa; abriendo el instalador…");
            if (!installer.startAsProcess(autoInstall ? "--update" : juce::String())) {
                if (safe->bridge) {
                    const auto message = juce::String("Windows no pudo abrir el instalador descargado.");
                    safe->showUpdateProgress(-2, message);
                    safe->bridge->toast(message, true);
                }
                return;
            }
            if (safe->bridge) safe->bridge->toast("Instalador abierto. Guarda tu secuencia y confirma el cierre para actualizar StageTracks.", false);
        });
    }).detach();
}

void MainComponent::showUpdateProgress(int percent, const juce::String& message)
{
    if (browser == nullptr) return;
    const auto script = "if(typeof window.receiveStageTracksUpdateProgress==='function')window.receiveStageTracksUpdateProgress("
                        + juce::String(percent) + "," + juce::JSON::toString(juce::var(message)) + ");";
    browser->evaluateJavascript(script);
}

void MainComponent::openAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(devices, 0, 0, 2, 64, false, false, true, false);
    selector->setSize(640, 430);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "Dispositivo y salidas de audio";
    options.dialogBackgroundColour = juce::Colour(0xff1b1e22);
    options.escapeKeyTriggersCloseButton = true;
    options.componentToCentreAround = this;
    options.launchAsync();
}

void MainComponent::pickToOpen()
{
    chooser = std::make_unique<juce::FileChooser>("Abrir secuencia", juce::File(), "*.rpseq;*.zip");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this](const juce::FileChooser& fc) {
                             const auto results = fc.getResults();
                             if (!results.isEmpty()) bridge->openPaths(results);
                         });
}

void MainComponent::pickToImport()
{
    chooser = std::make_unique<juce::FileChooser>("Importar audio", juce::File(), "*.wav;*.wave;*.bwf;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.wma");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this](const juce::FileChooser& fc) {
                             const auto results = fc.getResults();
                             if (results.isEmpty()) return;
                             juce::Array<juce::var> paths;
                             for (const auto& f : results) paths.add(f.getFullPathName());
                             bridge->queueScript("window.receiveLocalFiles(" + juce::JSON::toString(juce::var(paths), true) + ")");
                         });
}

void MainComponent::pickToSave()
{
    chooser = std::make_unique<juce::FileChooser>("Guardar secuencia", juce::File(), "*.rpseq");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this](const juce::FileChooser& fc) {
                             const auto f = fc.getResult();
                             bridge->queueScript(f == juce::File() ? juce::String("window.receiveSavePath(null)")
                                                                    : "window.receiveSavePath(" + juce::JSON::toString(juce::var(f.getFullPathName()), true) + ")");
                         });
}

void MainComponent::pickToExportSequence(const juce::String& suggestedName)
{
    auto fileName = suggestedName.replaceCharacters("\\/:*?\"<>|", "---------").trim().substring(0, 100);
    if (fileName.isEmpty()) fileName = "StageTracks-sequence";
    const auto initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                             .getChildFile(fileName + ".rpseq");
    chooser = std::make_unique<juce::FileChooser>("Exportar secuencia", initial, "*.rpseq");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this](const juce::FileChooser& fc) {
                             if (bridge == nullptr) return;
                             const auto file = fc.getResult();
                             const auto path = file == juce::File() ? juce::var() : juce::var(file.withFileExtension("rpseq").getFullPathName());
                             bridge->queueScript("window.receiveExportPath(" + juce::JSON::toString(path) + ")");
                         });
}
