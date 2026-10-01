#import <AVFAudio/AVFAudio.h>

#include "SpeechSynth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace
{
constexpr double RENDER_TIMEOUT_SECONDS = 10.0;
constexpr int64_t WAIT_SLICE_NANOSECONDS = 20 * (int64_t) NSEC_PER_MSEC;
constexpr double EMPTY_BUFFER_GRACE_SECONDS = 1.5;
constexpr double ABANDONED_JOB_WATCHDOG_SECONDS = 30.0;
} // namespace

// Owns one render. Blocks hold it strongly, so callbacks that arrive after the caller gave up
// (or after the plugin was destroyed) still land in valid memory.
@interface CKMKSpeechJob : NSObject <AVSpeechSynthesizerDelegate>
{
@public
    std::mutex lock;
    std::vector<float> samples;
    double sampleRate;
    uint64_t generation;
    bool abandoned;
    bool noVoice;
    std::string usedVoice;
}
@property (nonatomic, strong) AVSpeechSynthesizer* synth;
@property (nonatomic, strong) dispatch_semaphore_t done;
@property (nonatomic) BOOL finished;
@end

@implementation CKMKSpeechJob

- (instancetype) init
{
    if ((self = [super init]))
    {
        _done = dispatch_semaphore_create (0);
        sampleRate = 0.0;
        generation = 0;
        abandoned = false;
        noVoice = false;
    }

    return self;
}

// Main thread only; safe to call more than once.
- (void) finish
{
    if (self.finished)
        return;

    self.finished = YES;
    dispatch_semaphore_signal (self.done);

    // Releasing the synthesizer breaks the job -> synth -> callback block -> job cycle.
    dispatch_async (dispatch_get_main_queue(), ^{
        self.synth.delegate = nil;
        self.synth = nil;
    });
}

// Main thread only.
- (void) append: (AVAudioBuffer*) buffer
{
    if (! [buffer isKindOfClass: AVAudioPCMBuffer.class])
        return;

    AVAudioPCMBuffer* pcm = (AVAudioPCMBuffer*) buffer;
    const AVAudioFrameCount frames = pcm.frameLength;
    uint64_t current = 0;

    {
        std::lock_guard<std::mutex> guard (lock);
        current = ++generation;
    }

    if (frames == 0)
    {
        // An empty buffer usually means "done", but didFinish is the reliable signal; only fall back
        // to the empty buffer if nothing else arrives for a while.
        dispatch_after (dispatch_time (DISPATCH_TIME_NOW, (int64_t) (EMPTY_BUFFER_GRACE_SECONDS * NSEC_PER_SEC)),
                        dispatch_get_main_queue(), ^{
            bool idle = false;
            {
                std::lock_guard<std::mutex> guard (self->lock);
                idle = self->generation == current;
            }
            if (idle)
                [self finish];
        });
        return;
    }

    // Trust the buffer's own format: voices differ (Float32 22.05 kHz, Float32 16 kHz, Int16...).
    AVAudioFormat* format = pcm.format;
    const AVAudioChannelCount channels = std::max<AVAudioChannelCount> (1, format.channelCount);
    const NSUInteger stride = pcm.stride;
    const bool interleaved = format.isInterleaved;
    const float channelGain = 1.0f / (float) channels;

    std::lock_guard<std::mutex> guard (lock);

    if (abandoned)
        return;

    if (sampleRate == 0.0)
        sampleRate = format.sampleRate;

    const size_t base = samples.size();
    samples.resize (base + frames, 0.0f);
    float* out = samples.data() + base;

    for (AVAudioChannelCount channel = 0; channel < channels; ++channel)
    {
        switch (format.commonFormat)
        {
            case AVAudioPCMFormatFloat32:
            {
                const float* source = interleaved ? pcm.floatChannelData[0] + channel : pcm.floatChannelData[channel];
                for (AVAudioFrameCount i = 0; i < frames; ++i)
                    out[i] += channelGain * source[i * stride];
                break;
            }
            case AVAudioPCMFormatInt16:
            {
                const int16_t* source = interleaved ? pcm.int16ChannelData[0] + channel : pcm.int16ChannelData[channel];
                for (AVAudioFrameCount i = 0; i < frames; ++i)
                    out[i] += channelGain * (float) source[i * stride] / 32768.0f;
                break;
            }
            case AVAudioPCMFormatInt32:
            {
                const int32_t* source = interleaved ? pcm.int32ChannelData[0] + channel : pcm.int32ChannelData[channel];
                for (AVAudioFrameCount i = 0; i < frames; ++i)
                    out[i] += channelGain * (float) ((double) source[i * stride] / 2147483648.0);
                break;
            }
            case AVAudioPCMFormatFloat64:
            case AVAudioOtherFormat:
                break; // no speech voice produces these
        }
    }
}

- (void) speechSynthesizer: (AVSpeechSynthesizer*) synthesizer didFinishSpeechUtterance: (AVSpeechUtterance*) utterance
{
    [self finish];
}

- (void) speechSynthesizer: (AVSpeechSynthesizer*) synthesizer didCancelSpeechUtterance: (AVSpeechUtterance*) utterance
{
    [self finish];
}

@end

namespace
{
NSString* toNSString (const std::string& text)
{
    NSString* converted = [NSString stringWithUTF8String: text.c_str()];
    return converted != nil ? converted : @"";
}

std::string toStdString (NSString* text)
{
    const char* utf8 = text != nil ? text.UTF8String : nullptr;
    return utf8 != nullptr ? std::string (utf8) : std::string();
}

// Main thread only. Identifiers change when macOS swaps voice assets, so fall back to name, then language.
AVSpeechSynthesisVoice* resolveVoice (const clickmaker::SpeechRequest& request)
{
    if (! request.voiceId.empty())
        if (AVSpeechSynthesisVoice* voice = [AVSpeechSynthesisVoice voiceWithIdentifier: toNSString (request.voiceId)])
            return voice;

    if (! request.voiceName.empty())
    {
        AVSpeechSynthesisVoice* best = nil;

        for (AVSpeechSynthesisVoice* voice in AVSpeechSynthesisVoice.speechVoices)
            if ([voice.name isEqualToString: toNSString (request.voiceName)]
                && [voice.language isEqualToString: toNSString (request.voiceLanguage)]
                && (best == nil || voice.quality > best.quality))
                best = voice;

        if (best != nil)
            return best;
    }

    return [AVSpeechSynthesisVoice voiceWithLanguage: request.voiceLanguage.empty() ? nil : toNSString (request.voiceLanguage)];
}

int languageRank (NSString* language)
{
    if ([language hasPrefix: @"ko"]) return 0;
    if ([language hasPrefix: @"en"]) return 1;
    return 2;
}
} // namespace

namespace clickmaker
{

std::optional<RenderedSpeech> renderSpeech (const SpeechRequest& request, const AbortCheck& shouldAbort)
{
    if (NSThread.isMainThread)
        return std::nullopt; // every callback arrives on the main thread, so waiting here would deadlock

    @autoreleasepool
    {
        NSString* text = [toNSString (request.text) stringByTrimmingCharactersInSet: NSCharacterSet.whitespaceAndNewlineCharacterSet];

        // Empty text produces no callbacks at all, not even the end-of-utterance buffer.
        if (text.length == 0)
            return std::nullopt;

        CKMKSpeechJob* job = [CKMKSpeechJob new];
        const SpeechRequest copied = request;

        dispatch_async (dispatch_get_main_queue(), ^{
            {
                std::lock_guard<std::mutex> guard (job->lock);
                if (job->abandoned)
                {
                    [job finish];
                    return;
                }
            }

            AVSpeechSynthesisVoice* voice = resolveVoice (copied);

            if (voice == nil)
            {
                {
                    std::lock_guard<std::mutex> guard (job->lock);
                    job->noVoice = true;
                }
                [job finish];
                return;
            }

            AVSpeechUtterance* utterance = [AVSpeechUtterance speechUtteranceWithString: text];
            utterance.voice = voice;
            utterance.rate = std::clamp (copied.rate, AVSpeechUtteranceMinimumSpeechRate, AVSpeechUtteranceMaximumSpeechRate);
            utterance.prefersAssistiveTechnologySettings = NO; // VoiceOver settings must not change renders

            {
                std::lock_guard<std::mutex> guard (job->lock);
                job->usedVoice = toStdString (voice.identifier);
            }

            job.synth = [AVSpeechSynthesizer new];
            job.synth.delegate = job;
            [job.synth writeUtterance: utterance toBufferCallback: ^(AVAudioBuffer* buffer) { [job append: buffer]; }];
        });

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds ((int) (RENDER_TIMEOUT_SECONDS * 1000.0));
        bool done = false;

        while (! done && ! (shouldAbort && shouldAbort()) && std::chrono::steady_clock::now() < deadline)
            done = dispatch_semaphore_wait (job.done, dispatch_time (DISPATCH_TIME_NOW, WAIT_SLICE_NANOSECONDS)) == 0;

        std::lock_guard<std::mutex> guard (job->lock);

        if (! done)
        {
            // stopSpeaking does not stop a write, so drop late buffers and let the job run out.
            job->abandoned = true;
            dispatch_after (dispatch_time (DISPATCH_TIME_NOW, (int64_t) (ABANDONED_JOB_WATCHDOG_SECONDS * NSEC_PER_SEC)),
                            dispatch_get_main_queue(), ^{ [job finish]; });
            return std::nullopt;
        }

        if (job->noVoice || job->samples.empty() || job->sampleRate <= 0.0)
            return std::nullopt;

        return RenderedSpeech { std::move (job->samples), job->sampleRate, job->usedVoice };
    }
}

std::vector<SystemVoice> listVoices()
{
    std::vector<SystemVoice> voices;

    @autoreleasepool
    {
        for (AVSpeechSynthesisVoice* voice in AVSpeechSynthesisVoice.speechVoices)
        {
            const auto traits = voice.voiceTraits;

            if ((traits & AVSpeechSynthesisVoiceTraitIsNoveltyVoice) != 0
                || (traits & AVSpeechSynthesisVoiceTraitIsPersonalVoice) != 0)
                continue;

            voices.push_back ({ toStdString (voice.identifier), toStdString (voice.name),
                                toStdString (voice.language), (int) voice.quality });
        }
    }

    std::sort (voices.begin(), voices.end(), [] (const SystemVoice& a, const SystemVoice& b)
    {
        const int rankA = languageRank (toNSString (a.language));
        const int rankB = languageRank (toNSString (b.language));

        if (rankA != rankB) return rankA < rankB;
        if (a.language != b.language) return a.language < b.language;
        if (a.quality != b.quality) return a.quality > b.quality;
        return a.name < b.name;
    });

    return voices;
}

SystemVoice defaultVoice()
{
    @autoreleasepool
    {
        AVSpeechSynthesisVoice* voice = [AVSpeechSynthesisVoice voiceWithLanguage: nil];

        if (voice == nil)
            return {};

        return { toStdString (voice.identifier), toStdString (voice.name), toStdString (voice.language), (int) voice.quality };
    }
}

} // namespace clickmaker
