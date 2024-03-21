//
//     ,ad888ba,                              88
//    d8"'    "8b
//   d8            88,dba,,adba,   ,aPP8A.A8  88     The Cmajor Toolkit
//   Y8,           88    88    88  88     88  88
//    Y8a.   .a8P  88    88    88  88,   ,88  88     (C)2024 Cmajor Software Ltd
//     '"Y888Y"'   88    88    88  '"8bbP"Y8  88     https://cmajor.dev
//                                           ,88
//                                        888P"
//
//  The Cmajor project is subject to commercial or open-source licensing.
//  You may use it under the terms of the GPLv3 (see www.gnu.org/licenses), or
//  visit https://cmajor.dev to learn about our commercial licence options.
//
//  CMAJOR IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
//  EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
//  DISCLAIMED.

#pragma once

#include <iostream>

#include "../../compiler/include/cmaj_ErrorHandling.h"
#include "cmajor/helpers/cmaj_Patch.h"
#include "choc/text/choc_JSON.h"
#include "cmaj_AudioPlayer.h"

namespace cmaj
{

//==============================================================================
struct PatchPlayer  : public cmaj::audio_utils::AudioMIDICallback
{
    PatchPlayer (const choc::value::Value& engineOptions,
                 const cmaj::BuildSettings& buildSettings,
                 bool buildSynchronously = false,
                 bool checkFilesForChanges = true)
        : patch (buildSynchronously, checkFilesForChanges)
    {
        initPatchCallbacks (engineOptions, buildSettings);
    }

    ~PatchPlayer() override
    {
        setAudioMIDIPlayer ({});

        patch.stopPlayback      = [] {};
        patch.startPlayback     = [] {};
        patch.patchChanged      = [] {};
        patch.statusChanged     = [] (const cmaj::Patch::Status&) {};
        patch.handleOutputEvent = [] (uint64_t, std::string_view, const choc::value::ValueView&) {};

        patch.unload();
    }

    //==============================================================================
    void setAudioMIDIPlayer (std::shared_ptr<cmaj::audio_utils::MultiClientAudioMIDIPlayer> audioPlayerToUse)
    {
        if (audioPlayer != nullptr)
        {
            audioPlayer->removeCallback (*this);
            audioPlayer.reset();
        }

        cmaj::Patch::PlaybackParams params;

        if (audioPlayerToUse != nullptr)
        {
            audioPlayer = std::move (audioPlayerToUse);

            const auto& options = audioPlayer->player->options;

            params.blockSize          = options.blockSize;
            params.sampleRate         = options.sampleRate;
            params.numInputChannels   = options.inputChannelCount;
            params.numOutputChannels  = options.outputChannelCount;
        }
        else
        {
            // If we don't have a device yet, use some dummy values to allow
            // us to still load patches
            params.blockSize          = 256;
            params.sampleRate         = 44100;
            params.numInputChannels   = 2;
            params.numOutputChannels  = 2;
        }

        patch.setPlaybackParams (params);
        updatePlaybackState();
    }

    //==============================================================================
    bool loadPatch (const std::string& patchFile)
    {
        return patch.loadPatchFromFile (patchFile);
    }

    bool isPlaying() const
    {
        return playing;
    }

    void startPlayback()
    {
        playing = true;
        updatePlaybackState();
    }

    void stopPlayback()
    {
        playing = false;
        updatePlaybackState();
    }

    void setTempo (float bpm)
    {
        newBPM = bpm;
    }

    void setTimeSig (uint32_t newNumerator, uint32_t newDenominator)
    {
        newTimeSig = (newNumerator << 16) | newDenominator;
    }

    void setTransportState (bool isPlaying, bool isRecording)
    {
        newTransportState = isRecording ? 2 : (isPlaying ? 1 : 0);
    }

    //==============================================================================
    void handleStatusChange (const cmaj::Patch::Status& s)
    {
        if (onStatusChange)
            onStatusChange (s);
    }

    cmaj::Patch patch;

    std::function<void()> onPatchLoaded, onPatchUnloaded;
    std::function<void(const cmaj::Patch::Status&)> onStatusChange;

    uint64_t totalFramesRendered = 0;

private:
    //==============================================================================
    void initPatchCallbacks (const choc::value::Value& engineOptions, const cmaj::BuildSettings& buildSettings)
    {
        std::string engineType;

        if (engineOptions.isObject() && engineOptions.hasObjectMember("engine"))
            engineType = engineOptions["engine"].getString();

        patch.createEngine = [=]
        {
            auto engine = cmaj::Engine::create (engineType, &engineOptions);
            engine.setBuildSettings (buildSettings);
            return engine;
        };

        patch.stopPlayback      = [this] { setPatchCallbacksActive (false); };
        patch.startPlayback     = [this] { setPatchCallbacksActive (true); };
        patch.patchChanged      = [this] { handlePatchChange(); };
        patch.statusChanged     = [this] (const cmaj::Patch::Status& s) { handleStatusChange (s); };

        patch.handleOutputEvent = [] (uint64_t frame, std::string_view endpointID, const choc::value::ValueView& v)
        {
            if (endpointID == getConsoleEndpointID())
                std::cout << "event out: " << endpointID << ": " << frame << " " << choc::json::toString (v, false) << std::endl;
        };
    }

    void handlePatchChange()
    {
        bool loaded = patch.isPlayable();

        if (patchLoaded != loaded)
        {
            patchLoaded = loaded;

            if (loaded)
            {
                if (onPatchLoaded)
                    onPatchLoaded();
            }
            else
            {
                if (onPatchUnloaded)
                    onPatchUnloaded();
            }
        }
    }

    //==============================================================================
    void prepareToStart (double newSampleRate, HandleMIDIOutEventFn handleOutgoingMIDI) override
    {
        sampleRate = newSampleRate;
        currentBPM = 0;
        numerator = 0;
        denominator = 0;
        transportFlags = 0;
        dispatcher.reset (newSampleRate);
        dispatcher.setMidiOutputCallback (std::move (handleOutgoingMIDI));
    }

    void addIncomingMIDIEvent (const void* data, uint32_t size) override
    {
        dispatcher.addMIDIEvent (data, size);
    }

    void process (choc::buffer::ChannelArrayView<const float> input,
                  choc::buffer::ChannelArrayView<float> output,
                  bool replaceOutput) override
    {
        patch.beginChunkedProcess();
        dispatcher.setAudioBuffers (input, output);

        sendTimecodeEventsToPatch();

        dispatcher.processInChunks ([p = std::addressof (patch), replaceOutput]
                                    (const choc::audio::AudioMIDIBlockDispatcher::Block& block)
        {
            p->processChunk (block, replaceOutput);
        });

        patch.endChunkedProcess();
        totalFramesRendered += output.getNumFrames();
        ++blockCounter;
    }

    void sendTimecodeEventsToPatch()
    {
        if (patch.wantsTimecodeEvents())
        {
            uint32_t timeout = 0;
            auto newFlags = newTransportState.load();

            if (newFlags != transportFlags)
            {
                transportFlags = newFlags;
                patch.sendTransportState ((transportFlags & 2) != 0,
                                          (transportFlags & 1) != 0,
                                          (transportFlags & 4) != 0,
                                          timeout);
            }

            auto bpmToUse = newBPM.load();

            if (currentBPM != bpmToUse)
            {
                currentBPM = bpmToUse;
                patch.sendBPM (bpmToUse, timeout);
            }

            auto timesig = newTimeSig.load();

            if (numerator != (timesig >> 16) || denominator != (timesig & 0xffffu))
            {
                numerator = timesig >> 16;
                denominator = timesig & 0xffffu;
                patch.sendTimeSig (static_cast<int> (numerator),
                                   static_cast<int> (denominator),
                                   timeout);
            }

            double quarterNote = 0, barStartQuarterNote = 0;

            if (numerator != 0 && denominator != 0)
            {
                auto samplesPerQuarterNote = sampleRate / (currentBPM / 60.0);
                auto quarterNotesPerBar = (4.0 * numerator) / denominator;
                quarterNote = totalFramesRendered / samplesPerQuarterNote;
                auto barNumber = std::floor (quarterNote / quarterNotesPerBar);
                barStartQuarterNote = barNumber * quarterNotesPerBar;
            }

            patch.sendPosition (static_cast<int64_t> (totalFramesRendered), quarterNote, barStartQuarterNote, timeout);
        }
    }

    //==============================================================================
    void updatePlaybackState()
    {
        if (audioPlayer)
        {
            if (playing && patchCallbackActive)
                audioPlayer->addCallback (*this);
            else
                audioPlayer->removeCallback (*this);
        }
    }

    void setPatchCallbacksActive (bool b)
    {
        if (patchCallbackActive != b)
        {
            patchCallbackActive = b;
            updatePlaybackState();
        }
    }

    bool patchLoaded = false,
         playing = false,
         patchCallbackActive = false;

    double sampleRate = 0;
    float currentBPM = 0;
    std::atomic<float> newBPM { 0 };
    uint32_t numerator = 0, denominator = 0;
    std::atomic<uint32_t> newTimeSig { 0 };
    uint32_t transportFlags = 0;
    std::atomic<uint32_t> newTransportState { 0 };
    uint32_t blockCounter = 0;

    std::shared_ptr<cmaj::audio_utils::MultiClientAudioMIDIPlayer> audioPlayer;

    choc::audio::AudioMIDIBlockDispatcher dispatcher;
};

} // namespace cmaj
