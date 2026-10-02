#include "RenderHarness.h"

#include "ParamIds.h"
#include "PluginEditor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <iostream>
#include <thread>

namespace
{
using namespace clickmaker;

constexpr int SPEECH_TIMEOUT_MS = 120000;
constexpr float SNAPSHOT_SCALE = 2.0f;
const std::array<float, 3> SMOKE_RATES { 0.5f, 0.55f, 0.62f };

int listVoicesMode()
{
    const auto fallback = defaultVoice();
    std::cout << "system default: " << fallback.name << " " << fallback.language << " " << fallback.id << "\n";

    for (const auto& voice : listVoices())
        std::cout << voice.language << "\tquality " << voice.quality << "\t" << voice.name << "\t" << voice.id << "\n";

    return 0;
}

// Real speech on a worker thread while the main thread services the synthesizer's callbacks.
void renderWords (const std::string& voiceId)
{
    const std::vector<std::string> words { "Four", "Three", "Seven", "Pre-Chorus", "프리코러스", "하나" };
    std::vector<std::string> lines;
    std::atomic<bool> finished { false };

    std::thread worker ([&]
    {
        for (const auto& word : words)
        {
            for (const float rate : SMOKE_RATES)
            {
                const auto started = juce::Time::getMillisecondCounterHiRes();
                const auto speech = renderSpeech ({ word, voiceId, {}, {}, rate }, {});
                const auto elapsed = juce::Time::getMillisecondCounterHiRes() - started;
                juce::String line = juce::String::fromUTF8 (word.c_str()) + " @" + juce::String (rate, 2) + ": ";

                if (! speech.has_value())
                {
                    lines.push_back ((line + "render failed").toStdString());
                    continue;
                }

                const auto take = prepareTake (speech->samples, speech->sampleRate);
                line << juce::String (elapsed, 0) << " ms wall, " << speech->sampleRate << " Hz, raw "
                     << juce::String ((double) speech->samples.size() / speech->sampleRate, 3) << " s";

                if (take.has_value())
                {
                    line << ", take " << juce::String (take->durationSeconds(), 3) << " s, onset "
                         << juce::String (take->onsetSeconds() * 1000.0, 1) << " ms, voice " << speech->usedVoiceId;

                    juce::AudioBuffer<float> audio (1, (int) take->samples.size());
                    audio.copyFrom (0, 0, take->samples.data(), (int) take->samples.size());
                    test::writeWav (audio, take->sampleRate,
                                    "tts_" + juce::String::toHexString (juce::String::fromUTF8 (word.c_str()).hashCode())
                                        + "_" + juce::String ((int) (rate * 100.0f)));
                }
                else
                {
                    line << ", silent (voice cannot read this text)";
                }

                lines.push_back (line.toStdString());
            }
        }

        finished = true;
    });

    while (! finished)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

    worker.join();

    for (const auto& line : lines)
        std::cout << line << "\n";
}

// The whole pipeline with real speech: worker, takes, fitting and scheduling.
void renderCues (const std::string& voiceId)
{
    ClickMakerProcessor processor;
    test::setParameter (processor, param::CLICK_PAN, -1.0f); // click left, cue right, as in an MTR stem
    test::setParameter (processor, param::CUE_PAN, 1.0f);
    auto settings = processor.settings();
    settings.voice.id = voiceId;
    settings.slots[7].name = "코러스";
    processor.setSettings (settings);

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const bool ready = test::waitUntil ([&] { return processor.speechStatus().complete; }, SPEECH_TIMEOUT_MS);
    const auto status = processor.speechStatus();

    std::cout << "worker " << (ready ? "complete" : "TIMED OUT") << " in "
              << juce::String ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) << " s: "
              << status.wordsReady << "/" << status.wordsTotal << " ready, " << status.wordsFailed << " failed\n";

    // Own-bar names, then names inside the count ("Pre-Chorus 3 2 1"), where long names meet fast tempos.
    for (const int nameBeats : { 0, 1 })
    {
        test::setParameter (processor, param::NAME_LENGTH, (float) nameBeats);

        for (const double bpm : { 120.0, 200.0 })
        {
            test::Scenario scenario;
            scenario.name = juce::String (nameBeats == 0 ? "real_cue_" : "real_cue_inline_") + juce::String ((int) bpm);
            scenario.bpmAt = [bpm] (double) { return bpm; };
            scenario.lengthPpq = 22.0;
            scenario.notes = { { 0.0, FIRST_SLOT_NOTE + 0 }, { 8.0, FIRST_SLOT_NOTE + 5 }, { 16.0, FIRST_SLOT_NOTE + 7 } };
            test::renderScenario (processor, scenario);
            std::cout << "wrote " << CLICKMAKER_TEST_OUTPUT_DIR << "/" << scenario.name << ".wav\n";
        }
    }
}

int snapshotMode()
{
    ClickMakerProcessor processor (test::fakeSpeech);
    test::waitUntil ([&] { return processor.speechStatus().complete; }, SPEECH_TIMEOUT_MS);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditorAndMakeActive());
    test::waitUntil ([] { return false; }, 300); // let timers update the display once

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, SNAPSHOT_SCALE);
    editor.reset();

    const juce::File file (juce::String (CLICKMAKER_TEST_OUTPUT_DIR) + "/editor.png");
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    const bool written = stream.openedOk() && png.writeImageToStream (image, stream);
    std::cout << (written ? "wrote " : "failed to write ") << file.getFullPathName() << "\n";
    return written ? 0 : 1;
}

int runTests (const juce::String& category)
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    if (category.isEmpty())
    {
        runner.runTestsInCategory ("Unit");
        runner.runTestsInCategory ("Processor");
    }
    else
    {
        runner.runTestsInCategory (category);
    }

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    return failures > 0 ? 1 : 0;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const juce::ArgumentList arguments (argc, argv);

    if (arguments.containsOption ("--list-voices"))
        return listVoicesMode();

    if (arguments.containsOption ("--tts-smoke"))
    {
        const auto voiceId = arguments.getValueForOption ("--tts-smoke").toStdString();
        renderWords (voiceId);
        renderCues (voiceId);
        return 0;
    }

    if (arguments.containsOption ("--snapshot"))
        return snapshotMode();

    return runTests (arguments.getValueForOption ("--category"));
}
