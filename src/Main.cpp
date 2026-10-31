#include <JuceHeader.h>
#include "MainComponent.h"

class StageTracksApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "StageTracks"; }
    const juce::String getApplicationVersion() override { return "0.4.0"; }
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
    class Window final : public juce::DocumentWindow {
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
    };
private:
    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION(StageTracksApplication)
