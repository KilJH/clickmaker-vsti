#pragma once

// Plain C++ interface to the macOS speech synthesizer; the implementation is Objective-C++.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace clickmaker
{

struct SpeechRequest
{
    std::string text;
    std::string voiceId;       // all three empty: the system default voice
    std::string voiceName;     // fallbacks for when macOS renames a voice identifier
    std::string voiceLanguage;
    float rate = 0.5f;         // AVSpeechUtterance rate, 0..1
};

struct RenderedSpeech
{
    std::vector<float> samples;
    double sampleRate = 0.0;
    std::string usedVoiceId;
};

struct SystemVoice
{
    std::string id;
    std::string name;
    std::string language;
    int quality = 1; // 1 default, 2 enhanced, 3 premium
};

using AbortCheck = std::function<bool()>;

// Blocks the calling thread until the speech is rendered. Never call it on the main thread:
// the synthesizer delivers every callback there.
std::optional<RenderedSpeech> renderSpeech (const SpeechRequest&, const AbortCheck& shouldAbort);

std::vector<SystemVoice> listVoices();
SystemVoice defaultVoice();

} // namespace clickmaker
