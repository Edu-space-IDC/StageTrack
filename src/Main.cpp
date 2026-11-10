#include <JuceHeader.h>
#include "MainComponent.h"
#if JUCE_WINDOWS
#include <windows.h>
#endif

class StageTracksApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "StageTracks"; }
    const juce::String getApplicationVersion() override { return STAGETRACKS_APP_VERSION; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise(const juce::String&) override
    {
        juce::Array<juce::File> files;
        juce::StringArray args;
        args.addTokens(getCommandLineParameters(), true);
        for (const auto& arg : args) {
            juce::File f(arg.unquoted());
            if (f.existsAsFile()) files.add(f);
        }
        window = std::make_unique<Window>(std::move(files));
    }
    void shutdown() override { window.reset(); }
    class Window final : public juce::DocumentWindow, public juce::FileDragAndDropTarget {
    public:
        explicit Window(juce::Array<juce::File> files) : DocumentWindow("StageTracks", juce::Colour(0xff07080a), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true); setContentOwned(new MainComponent(), true);
            setResizable(true,true);setResizeLimits(520,480,10000,10000);
            const auto* display=juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
            const auto area=display!=nullptr?display->userBounds.toNearestInt():juce::Rectangle<int>(0,0,1280,720);
            centreWithSize(juce::jmax(520,juce::jmin(1280,area.getWidth()-20)),
                           juce::jmax(600,juce::jmin(900,area.getHeight()-48)));
            setVisible(true);
#if JUCE_WINDOWS
            applyNativeTitleBarTheme();
#endif
            if (!files.isEmpty())
                if (auto* player=dynamic_cast<MainComponent*>(getContentComponent()))
                    juce::MessageManager::callAsync([safe=juce::Component::SafePointer<MainComponent>(player),files=std::move(files)]() mutable {
                        if (safe != nullptr) safe->importFiles(std::move(files));
                    });
        }
        void closeButtonPressed() override
        {
            if (auto* player = dynamic_cast<MainComponent*>(getContentComponent())) player->requestApplicationClose();
            else juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
        bool isInterestedInFileDrag(const juce::StringArray& files) override
        {
            for (const auto& path : files) {
                const juce::File file(path);
                if (file.hasFileExtension("rpseq;zip;wav;wave;bwf;aif;aiff;flac;mp3;ogg;m4a;wma")) return true;
            }
            return false;
        }
        void filesDropped(const juce::StringArray& paths, int, int) override
        {
            juce::Array<juce::File> files;
            for (const auto& path : paths) {
                const juce::File file(path);
                if (file.existsAsFile() && file.hasFileExtension("rpseq;zip;wav;wave;bwf;aif;aiff;flac;mp3;ogg;m4a;wma"))
                    files.add(file);
            }
            if (auto* player = dynamic_cast<MainComponent*>(getContentComponent())) player->importFiles(std::move(files));
        }
    };
#if JUCE_WINDOWS
    void applyNativeTitleBarTheme()
    {
        using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        const auto module = LoadLibraryW(L"dwmapi.dll");
        if (module == nullptr) return;
        const auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(module, "DwmSetWindowAttribute"));
        const auto hwnd = static_cast<HWND>(getWindowHandle());
        if (setAttribute != nullptr && hwnd != nullptr) {
            const BOOL dark = TRUE;
            setAttribute(hwnd, 20, &dark, sizeof(dark));       // DWMWA_USE_IMMERSIVE_DARK_MODE
            const DWORD caption = RGB(38, 41, 45);
            const DWORD text = RGB(242, 244, 246);
            setAttribute(hwnd, 35, &caption, sizeof(caption));  // DWMWA_CAPTION_COLOR
            setAttribute(hwnd, 36, &text, sizeof(text));        // DWMWA_TEXT_COLOR
        }
        FreeLibrary(module);
    }
#endif
private:
    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION(StageTracksApplication)
