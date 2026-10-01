#include "RenderHarness.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace clickmaker::test
{

namespace
{
constexpr double PPQ_EPSILON = 1.0e-9;
constexpr juce::int64 JITTER_SEED = 12345;
constexpr double FAKE_SAMPLE_RATE = 22050.0;
constexpr double FAKE_WORD_SECONDS = 0.25; // vowel length at the default rate
constexpr double FAKE_CONSONANT_SECONDS = 0.03;
constexpr double FAKE_PADDING_SECONDS = 0.02;
constexpr double FAKE_CONSONANT_HZ = 6000.0;
constexpr float FAKE_CONSONANT_LEVEL = 0.4f;
constexpr float FAKE_VOWEL_LEVEL = 0.8f;
constexpr double FAKE_NAME_HZ = 2000.0;
constexpr double FAKE_FIRST_NUMBER_HZ = 300.0;
constexpr double FAKE_NUMBER_STEP_HZ = 100.0;
constexpr float DEFAULT_SPEECH_RATE = 0.5f;
constexpr int WAV_BITS = 24;

struct ScriptedPlayHead final : juce::AudioPlayHead
{
    PositionInfo info;

    juce::Optional<PositionInfo> getPosition() const override { return info; }
};

bool isPlainAscii (const std::string& text)
{
    return std::all_of (text.begin(), text.end(), [] (char c) { return (unsigned char) c < 0x80; });
}

void appendSine (std::vector<float>& samples, double frequency, double seconds, float level)
{
    const auto count = (int) (seconds * FAKE_SAMPLE_RATE);
    for (int i = 0; i < count; ++i)
        samples.push_back (level * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / FAKE_SAMPLE_RATE));
}

void appendSilence (std::vector<float>& samples, double seconds)
{
    samples.insert (samples.end(), (size_t) (seconds * FAKE_SAMPLE_RATE), 0.0f);
}
} // namespace

Meter meterAt (const Scenario& scenario, double ppq)
{
    Meter meter = scenario.meters.front().meter;

    for (const auto& change : scenario.meters)
        if (change.atPpq <= ppq + PPQ_EPSILON)
            meter = change.meter;

    return meter;
}

double barStartAt (const Scenario& scenario, double ppq)
{
    const MeterChange* segment = &scenario.meters.front();

    for (const auto& change : scenario.meters)
        if (change.atPpq <= ppq + PPQ_EPSILON)
            segment = &change;

    const double barPpq = segment->meter.barPpq();
    return segment->atPpq + std::floor ((ppq - segment->atPpq + PPQ_EPSILON) / barPpq) * barPpq;
}

double RenderResult::sampleAtPpq (double ppq) const
{
    for (const auto& block : blocks)
        if (block.playing && ppq >= block.ppqStart - PPQ_EPSILON
            && ppq < block.ppqStart + block.numSamples * block.ppqPerSample)
            return block.startSample + (ppq - block.ppqStart) / block.ppqPerSample;

    return -1.0;
}

RenderResult renderScenario (juce::AudioProcessor& processor, const Scenario& scenario)
{
    ScriptedPlayHead playHead;
    const int maxBlock = *std::max_element (scenario.blockSizes.begin(), scenario.blockSizes.end());
    processor.setPlayHead (&playHead);
    processor.setRateAndBufferSizeDetails (scenario.sampleRate, maxBlock);
    processor.prepareToPlay (scenario.sampleRate, maxBlock);

    const int channels = processor.getTotalNumOutputChannels();
    juce::AudioBuffer<float> block (channels, maxBlock);
    juce::MidiBuffer midi;
    juce::Random jitter (JITTER_SEED);
    std::vector<std::vector<float>> output ((size_t) channels);
    std::vector<bool> jumped (scenario.jumps.size(), false);
    RenderResult result;

    double ppq = scenario.startPpq;
    double elapsedPpq = 0.0;
    int sample = 0;

    for (size_t index = 0; elapsedPpq < scenario.lengthPpq; ++index)
    {
        for (size_t j = 0; j < scenario.jumps.size(); ++j)
        {
            // Keeps the overshoot, like a host that does not split the block at a cycle end.
            if (! jumped[j] && ppq >= scenario.jumps[j].atPpq - PPQ_EPSILON)
            {
                ppq = scenario.jumps[j].toPpq + (ppq - scenario.jumps[j].atPpq);
                jumped[j] = true;
            }
        }

        const int numSamples = scenario.blockSizes[index % scenario.blockSizes.size()];
        const bool playing = scenario.playing && ppq < scenario.stopAtPpq - PPQ_EPSILON;
        const double bpm = scenario.bpmAt (ppq);
        const double ppqPerSample = bpm / (60.0 * scenario.sampleRate);
        const auto meter = meterAt (scenario, ppq);

        juce::AudioPlayHead::PositionInfo info;
        info.setIsPlaying (playing);
        info.setBpm (bpm);
        info.setTimeSignature (juce::AudioPlayHead::TimeSignature { meter.numerator, meter.denominator });
        info.setPpqPosition (ppq + (jitter.nextDouble() * 2.0 - 1.0) * scenario.jitterPpq);
        info.setTimeInSamples (sample);
        info.setTimeInSeconds (sample / scenario.sampleRate);

        if (scenario.provideBarStart)
            info.setPpqPositionOfLastBarStart (barStartAt (scenario, ppq));

        playHead.info = info;
        midi.clear();

        if (playing)
        {
            for (const auto& note : scenario.notes)
            {
                const auto offset = (int) std::floor ((note.ppq - ppq) / ppqPerSample + 0.5);
                if (offset >= 0 && offset < numSamples)
                    midi.addEvent (juce::MidiMessage::noteOn (1, note.note, (juce::uint8) 100), offset);
            }
        }

        block.setSize (channels, numSamples, false, false, true);
        block.clear();
        processor.processBlock (block, midi);

        for (int c = 0; c < channels; ++c)
        {
            const auto* data = block.getReadPointer (c);
            output[(size_t) c].insert (output[(size_t) c].end(), data, data + numSamples);
        }

        result.blocks.push_back ({ sample, numSamples, ppq, ppqPerSample, playing });

        if (playing)
            ppq += numSamples * ppqPerSample;

        elapsedPpq += numSamples * ppqPerSample;
        sample += numSamples;
    }

    processor.releaseResources();
    processor.setPlayHead (nullptr);

    result.audio.setSize (channels, sample);
    for (int c = 0; c < channels; ++c)
        result.audio.copyFrom (c, 0, output[(size_t) c].data(), sample);

    if (scenario.name.isNotEmpty())
        writeWav (result.audio, scenario.sampleRate, scenario.name);

    return result;
}

std::vector<int> findOnsets (const float* samples, int numSamples, float threshold, int minimumGap)
{
    std::vector<int> onsets;
    int quietRun = minimumGap;

    for (int i = 0; i < numSamples; ++i)
    {
        if (std::abs (samples[i]) > threshold)
        {
            if (quietRun >= minimumGap)
                onsets.push_back (i);

            quietRun = 0;
        }
        else
        {
            ++quietRun;
        }
    }

    return onsets;
}

double estimateFrequency (const float* samples, int numSamples, double sampleRate)
{
    int crossings = 0;
    for (int i = 1; i < numSamples; ++i)
        if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f))
            ++crossings;

    return crossings * 0.5 * sampleRate / numSamples;
}

int countNonSilent (const float* samples, int numSamples, float threshold)
{
    return (int) std::count_if (samples, samples + numSamples, [threshold] (float s) { return std::abs (s) > threshold; });
}

void writeWav (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::String& name)
{
    const juce::File directory (CLICKMAKER_TEST_OUTPUT_DIR);
    directory.createDirectory();

    const auto file = directory.getChildFile (name + ".wav");
    file.deleteFile(); // createOutputStream appends to an existing file

    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr)
        return;

    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (audio.getNumChannels())
                             .withBitsPerSample (WAV_BITS);

    if (auto writer = format.createWriterFor (stream, options))
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

double fakeFrequencyFor (const std::string& text)
{
    const auto numbers = englishCountWords();

    for (size_t i = 0; i < numbers.size(); ++i)
        if (numbers[i] == text)
            return FAKE_FIRST_NUMBER_HZ + FAKE_NUMBER_STEP_HZ * (double) i;

    return FAKE_NAME_HZ;
}

std::optional<RenderedSpeech> fakeSpeech (const SpeechRequest& request, const AbortCheck&)
{
    RenderedSpeech speech;
    speech.sampleRate = FAKE_SAMPLE_RATE;
    speech.usedVoiceId = "fake";

    appendSilence (speech.samples, FAKE_PADDING_SECONDS);

    // Like a real English voice, the default fake voice renders silence for text it cannot read.
    const bool readable = isPlainAscii (request.text) || request.voiceLanguage.rfind ("ko", 0) == 0;

    if (readable)
    {
        appendSine (speech.samples, FAKE_CONSONANT_HZ, FAKE_CONSONANT_SECONDS, FAKE_CONSONANT_LEVEL);
        appendSine (speech.samples, fakeFrequencyFor (request.text),
                    FAKE_WORD_SECONDS * DEFAULT_SPEECH_RATE / request.rate, FAKE_VOWEL_LEVEL);
    }

    appendSilence (speech.samples, FAKE_PADDING_SECONDS);
    return speech;
}

WordTake fakeTake (const std::string& text, float rate)
{
    const auto speech = fakeSpeech ({ text, {}, {}, {}, rate }, {});
    return prepareTake (speech->samples, speech->sampleRate).value();
}

bool waitUntil (const std::function<bool()>& condition, int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

    while (juce::Time::getMillisecondCounter() < deadline)
    {
        if (condition())
            return true;

        // Pumping the run loop lets real speech callbacks reach the main thread.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }

    return condition();
}

void setParameter (ClickMakerProcessor& processor, const char* id, float plainValue)
{
    auto* parameter = processor.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (plainValue));
}

} // namespace clickmaker::test
