#include "AUInstrumentBase.h"
#include "SinSynthVersion.h"
#include <CoreMIDI/CoreMIDI.h>
#include <vector>
#include <cstring>

#ifdef DEBUG
#include <fstream>
#include <ctime>
#endif

// two debug macros (one for the "base" class, one for "kernel") defined
#ifdef DEBUG
#define DEBUGLOG_B(x) \
  if (baseDebugFile.is_open()) baseDebugFile << "\t" << x
#else
#define DEBUGLOG_B(x)
#endif

using namespace std;

typedef struct MIDIMessageInfoStruct {
  UInt8 status;
  UInt8 channel;
  UInt8 data1;
  UInt8 data2;
  UInt32 startFrame;
} MIDIMessageInfoStruct;

class MIDIOutputCallbackHelper {
  enum { kSizeofMIDIBuffer = 512 };

 public:
  MIDIOutputCallbackHelper() {
    mMIDIMessageList.reserve(64);
    mMIDICallbackStruct.midiOutputCallback = NULL;
    mMIDIBuffer = new Byte[kSizeofMIDIBuffer];
  }

  ~MIDIOutputCallbackHelper() { delete[] mMIDIBuffer; }

  void SetCallbackInfo(AUMIDIOutputCallback &callback, void *userData) {
    mMIDICallbackStruct.midiOutputCallback = callback;
    mMIDICallbackStruct.userData = userData;
  }

  void AddMIDIEvent(UInt8 status, UInt8 channel, UInt8 data1, UInt8 data2,
                    UInt32 inStartFrame);

  void FireAtTimeStamp(const AudioTimeStamp &inTimeStamp);

 private:
  MIDIPacketList *PacketList() { return (MIDIPacketList *)mMIDIBuffer; }

  Byte *mMIDIBuffer;

  AUMIDIOutputCallbackStruct mMIDICallbackStruct;

  typedef vector<MIDIMessageInfoStruct> MIDIMessageList;
  MIDIMessageList mMIDIMessageList;
};

class SinSynthWithMidi : public AUMonotimbralInstrumentBase {
 public:
  SinSynthWithMidi(AudioUnit inComponentInstance);
  ~SinSynthWithMidi();

  OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
                           AudioUnitElement inElement, UInt32 &outDataSize,
                           Boolean &outWritable);

  OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                       AudioUnitElement inElement, void *outData);

  OSStatus SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                       AudioUnitElement inElement, const void *inData,
                       UInt32 inDataSize);

  OSStatus HandleMidiEvent(UInt8 status, UInt8 channel, UInt8 data1,
                           UInt8 data2, UInt32 inStartFrame);

  OSStatus Render(AudioUnitRenderActionFlags &ioActionFlags,
                  const AudioTimeStamp &inTimeStamp, UInt32 inNumberFrames);

  OSStatus Initialize();
  void Cleanup();
  OSStatus Version() { return kSinSynthVersion; }

  AUElement *CreateElement(AudioUnitScope scope, AudioUnitElement element);

  OSStatus GetParameterInfo(AudioUnitScope inScope,
                            AudioUnitParameterID inParameterID,
                            AudioUnitParameterInfo &outParameterInfo);

  MidiControls *GetControls(MusicDeviceGroupID inChannel) {
    SynthGroupElement *group = GetElForGroupID(inChannel);
    return (MidiControls *)group->GetMIDIControlHandler();
  }

 private:
  MIDIOutputCallbackHelper mCallbackHelper;

 protected:
#ifdef DEBUG
  ofstream baseDebugFile;
#endif
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
#pragma mark MIDIOutputCallbackHelper Methods
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void MIDIOutputCallbackHelper::AddMIDIEvent(UInt8 status, UInt8 channel,
                                            UInt8 data1, UInt8 data2,
                                            UInt32 inStartFrame) {
  MIDIMessageInfoStruct info = {status, channel, data1, data2, inStartFrame};
  mMIDIMessageList.push_back(info);
}

void MIDIOutputCallbackHelper::FireAtTimeStamp(
    const AudioTimeStamp &inTimeStamp) {
  if (!mMIDIMessageList.empty()) {
    if (mMIDICallbackStruct.midiOutputCallback) {
      // synthesize the packet list and call the MIDIOutputCallback
      // iterate through the vector and get each item
      MIDIPacketList *pktlist = PacketList();

      MIDIPacket *pkt = MIDIPacketListInit(pktlist);

      for (MIDIMessageList::iterator iter = mMIDIMessageList.begin();
           iter != mMIDIMessageList.end(); iter++) {
        const MIDIMessageInfoStruct &item = *iter;

        Byte midiStatusByte = item.status + item.channel;
        const Byte data[3] = {midiStatusByte, item.data1, item.data2};
        UInt32 midiDataCount =
            ((item.status == 0xC || item.status == 0xD) ? 2 : 3);

        pkt = MIDIPacketListAdd(pktlist, kSizeofMIDIBuffer, pkt,
                                item.startFrame, midiDataCount, data);
        if (!pkt) {
          // send what we have and then clear the buffer and then go through
          // this again issue the callback with what we got
          OSStatus result = (*mMIDICallbackStruct.midiOutputCallback)(
              mMIDICallbackStruct.userData, &inTimeStamp, 0, pktlist);
          if (result != noErr)
            printf("error calling output callback: %d", (int)result);

          // clear stuff we've already processed, and fire again
          mMIDIMessageList.erase(mMIDIMessageList.begin(), iter);
          FireAtTimeStamp(inTimeStamp);
          return;
        }
      }

      // fire callback
      OSStatus result = (*mMIDICallbackStruct.midiOutputCallback)(
          mMIDICallbackStruct.userData, &inTimeStamp, 0, pktlist);
      if (result != noErr)
        printf("error calling output callback: %d", (int)result);
    }
    mMIDIMessageList.clear();
  }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
#pragma mark SinSynthWithMidi Methods
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

AUDIOCOMPONENT_ENTRY(AUMusicDeviceFactory, SinSynthWithMidi)

static const AudioUnitParameterID kGlobalMuteSwitchParam = 0;
static const CFStringRef kGlobalMuteSwitchName = CFSTR("Mute Switch");

SinSynthWithMidi::SinSynthWithMidi(AudioComponentInstance inComponentInstance)
    : AUMonotimbralInstrumentBase(inComponentInstance, 0, 1) {
  CreateElements();

  Globals()->UseIndexedParameters(1);  // we're only defining one param
  Globals()->SetParameter(kGlobalMuteSwitchParam,
                          false);  // initialize default value

#ifdef DEBUG
  string bPath, bFullFileName;
  bPath = getenv("HOME");
  if (!bPath.empty()) {
    bFullFileName = bPath + "/Desktop/" + "Debug.log";
  } else {
    bFullFileName = "Debug.log";
  }

  baseDebugFile.open(bFullFileName.c_str());
  DEBUGLOG_B("Plug-in constructor invoked with parameters:" << endl);
#endif
}

SinSynthWithMidi::~SinSynthWithMidi() {}

OSStatus SinSynthWithMidi::GetPropertyInfo(AudioUnitPropertyID inID,
                                           AudioUnitScope inScope,
                                           AudioUnitElement inElement,
                                           UInt32 &outDataSize,
                                           Boolean &outWritable) {
  if (inScope == kAudioUnitScope_Global) {
    if (inID == kAudioUnitProperty_MIDIOutputCallbackInfo) {
      outDataSize = sizeof(CFArrayRef);
      outWritable = false;
      return noErr;
    } else if (inID == kAudioUnitProperty_MIDIOutputCallback) {
      outDataSize = sizeof(AUMIDIOutputCallbackStruct);
      outWritable = true;
      return noErr;
    }
  }
  return AUMonotimbralInstrumentBase::GetPropertyInfo(inID, inScope, inElement,
                                                      outDataSize, outWritable);
}

void SinSynthWithMidi::Cleanup() {
#ifdef DEBUG
  DEBUGLOG_B("SinSynth::Cleanup" << endl);
#endif
}

OSStatus SinSynthWithMidi::Initialize() {
#ifdef DEBUG
  DEBUGLOG_B("->SinSynth::Initialize" << endl);
#endif
  AUMonotimbralInstrumentBase::Initialize();

#ifdef DEBUG
  DEBUGLOG_B("<-SinSynth::Initialize" << endl);
#endif

  return noErr;
}

AUElement *SinSynthWithMidi::CreateElement(AudioUnitScope scope,
                                           AudioUnitElement element) {
  switch (scope) {
    case kAudioUnitScope_Group:
      return new SynthGroupElement(this, element, new MidiControls);
    case kAudioUnitScope_Part:
      return new SynthPartElement(this, element);
    default:
      return AUBase::CreateElement(scope, element);
  }
}

OSStatus SinSynthWithMidi::GetParameterInfo(
    AudioUnitScope inScope, AudioUnitParameterID inParameterID,
    AudioUnitParameterInfo &outParameterInfo) {

  if (inParameterID != kGlobalMuteSwitchParam)
    return kAudioUnitErr_InvalidParameter;

  if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

  outParameterInfo.flags = SetAudioUnitParameterDisplayType(
      0, kAudioUnitParameterFlag_DisplaySquareRoot);
  outParameterInfo.flags += kAudioUnitParameterFlag_IsWritable;
  outParameterInfo.flags += kAudioUnitParameterFlag_IsReadable;

  AUBase::FillInParameterName(outParameterInfo, kGlobalMuteSwitchName, false);
  outParameterInfo.unit = kAudioUnitParameterUnit_Boolean;
  outParameterInfo.minValue = false;
  outParameterInfo.maxValue = true;
  outParameterInfo.defaultValue = true;  // disabled?
  return noErr;
}

OSStatus SinSynthWithMidi::GetProperty(AudioUnitPropertyID inID,
                                       AudioUnitScope inScope,
                                       AudioUnitElement inElement,
                                       void *outData) {
  if (inScope == kAudioUnitScope_Global) {
    if (inID == kAudioUnitProperty_MIDIOutputCallbackInfo) {
      CFStringRef strs[1];
      strs[0] = CFSTR("MIDI Callback");

      CFArrayRef callbackArray =
          CFArrayCreate(NULL, (const void **)strs, 1, &kCFTypeArrayCallBacks);
      *(CFArrayRef *)outData = callbackArray;
      return noErr;
    }
  }
  return AUMonotimbralInstrumentBase::GetProperty(inID, inScope, inElement,
                                                  outData);
}

OSStatus SinSynthWithMidi::SetProperty(AudioUnitPropertyID inID,
                                       AudioUnitScope inScope,
                                       AudioUnitElement inElement,
                                       const void *inData, UInt32 inDataSize) {
#ifdef DEBUG
  DEBUGLOG_B("SetProperty" << endl);
#endif
  if (inScope == kAudioUnitScope_Global) {
    if (inID == kAudioUnitProperty_MIDIOutputCallback) {
      if (inDataSize < sizeof(AUMIDIOutputCallbackStruct))
        return kAudioUnitErr_InvalidPropertyValue;

      AUMIDIOutputCallbackStruct *callbackStruct =
          (AUMIDIOutputCallbackStruct *)inData;
      mCallbackHelper.SetCallbackInfo(callbackStruct->midiOutputCallback,
                                      callbackStruct->userData);
      return noErr;
    }
  }
  return AUMonotimbralInstrumentBase::SetProperty(inID, inScope, inElement,
                                                  inData, inDataSize);
}

OSStatus SinSynthWithMidi::HandleMidiEvent(UInt8 status, UInt8 channel,
                                           UInt8 data1, UInt8 data2,
                                           UInt32 inStartFrame) {

  bool sw = Globals()->GetParameter(kGlobalMuteSwitchParam);

#ifdef DEBUG
  DEBUGLOG_B("HandleMidiEvent - Mute Switch: " << sw << endl);
#endif

  // snag the midi event and then store it in a vector
  if (!sw || status != 0x90)
    mCallbackHelper.AddMIDIEvent(status, channel, data1, data2, inStartFrame);

  return AUMIDIBase::HandleMidiEvent(status, channel, data1, data2,
                                     inStartFrame);
}

OSStatus SinSynthWithMidi::Render(AudioUnitRenderActionFlags &ioActionFlags,
                                  const AudioTimeStamp &inTimeStamp,
                                  UInt32 inNumberFrames) {

  OSStatus result =
      AUInstrumentBase::Render(ioActionFlags, inTimeStamp, inNumberFrames);
  if (result == noErr) {
    mCallbackHelper.FireAtTimeStamp(inTimeStamp);
  }
  return result;
}
