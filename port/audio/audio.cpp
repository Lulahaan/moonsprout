// Audio for host builds: the audio interface (AI DMA → SDL audio stream) and a software stand-in for
// the GameCube DSP running JAudio's voice microcode.
//
// Frame protocol (JASAiCtrl.cpp / JASAudioThread.cpp): the audio thread calls DsyncFrame2 with a
// left and a right output buffer for `subframes` × 80 samples, then expects one "subframe done"
// mail (0xF355FFxx) per subframe; between mails it updates the voice table (JASDsp::TChannel).
// Here the whole frame is rendered at once and the mails are delivered as interrupts. The AI DMA
// callback is raised from a host thread as the SDL stream drains, which paces everything.
//
// Voice model (after the JAudio/"Zelda" microcode as documented by Dolphin's HLE):
//   mIsActive        voice enabled                mIsFinished     set by the DSP when it ends
//   mIsPlaying       start request; the DSP clears it when it (re)initialises the voice
//   mPitch           resampling ratio, 0x1000 = one input sample per output sample
//   mMixer[0..5]     {bus, target volume, current volume, flags}; volumes ramp over a subframe
//   mDataOffset      ARAM address of the sample data; mSampleOffset is the current read address
//   mCurrentSampleOffset  samples left before the loop point / end (the stream code edits it)
//   mLoopOffset != 0 looping, from mLoopStartOffset (ADPCM history mLast/mPenult at that point)
//   mNextSampleOffset loop end / sample count       mForcedStop      stop request
#include "JSystem/JAudio/JAS/JASDsp.h"

#include <dolphin/ai.h>
#include <dolphin/ar.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" void p2_raise_interrupt(void (*fn)(void*), void* arg);

extern "C" int p2_audio_enabled(void)
{
	// Latched on first use: the sound system is either built at boot or not at all (audio.enabled
	// needs a restart). gP2Config is filled by the launcher before the game starts.
	static const int enabled = gP2Config.audioEnabled ? 1 : 0;
	return enabled;
}

// Settings the mixer and AI threads read: set by the launcher (p2_host.h), read lock-free here.
namespace {
std::atomic<float> g_masterGain{1.0f};
std::atomic<float> g_duckTarget{1.0f}; // mute in background / pause in menu; ramped in the mixer
// Pause While Menu Is Open: voices playing the game's system sound effects (the menu cues) take
// g_uiGainTarget instead of the duck, so the menu is not silent while the game is.
std::atomic<bool> g_menuHold{false};
std::atomic<float> g_uiGainTarget{1.0f};
std::atomic<bool> g_audioDebug{false};
std::atomic<bool> g_voiceLog{false};
std::mutex g_dumpPathLock;
std::string g_dumpPath;
} // namespace

extern "C" void p2_audio_set_master_gain(float gain)
{
	g_masterGain.store(gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain), std::memory_order_relaxed);
}

extern "C" void p2_audio_set_duck(float gain)
{
	g_duckTarget.store(gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain), std::memory_order_relaxed);
}

extern "C" void p2_audio_set_menu_hold(int held, float uiGain)
{
	g_uiGainTarget.store(uiGain < 0.0f ? 0.0f : (uiGain > 1.0f ? 1.0f : uiGain), std::memory_order_relaxed);
	g_menuHold.store(held != 0, std::memory_order_relaxed);
}

extern "C" unsigned long long p2_game_ui_voice_mask(void); // port/game/ui_sound.cpp

extern "C" void p2_audio_set_debug(int debug, int voiceLog)
{
	g_audioDebug.store(debug != 0, std::memory_order_relaxed);
	g_voiceLog.store(voiceLog != 0, std::memory_order_relaxed);
}

extern "C" int p2_audio_debug(void)
{
	return g_audioDebug.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" void p2_audio_set_dump_path(const char* path)
{
	std::lock_guard lk(g_dumpPathLock);
	g_dumpPath = path != nullptr ? path : "";
}

// ============================================================================================
// AI
// ============================================================================================
namespace {

constexpr int kOutputRate = 32000;

std::mutex g_aiLock;
std::condition_variable g_aiCv;
uintptr_t g_dmaAddr      = 0;
u32 g_dmaLen             = 0;
u32 g_dmaGeneration      = 0;
bool g_dmaRunning        = false;
AIDCallback g_dmaCallback = nullptr;
u32 g_aiDspRate          = 0;
SDL_AudioStream* g_stream = nullptr;
bool g_aiThreadStarted   = false;
std::mutex g_streamLock;   // held while the AI thread uses g_stream (p2_audio_shutdown destroys it)
bool g_aiQuit            = false;

std::atomic<int> g_dmaIrqs{0}, g_dspFrames{0}, g_mailIrqs{0};
int g_mailsQueuedDbg();
int g_mailsPendingDbg();
void dmaInterrupt(void*)
{
	g_dmaIrqs++;
	if (g_dmaCallback) {
		g_dmaCallback();
	}
}

// tools.audioDumpPath (MPPM_AUDIO_DUMP=<file.wav>): also write the output to a WAV file (for
// checking audio headless).
FILE* openDump()
{
	std::string path;
	{
		std::lock_guard lk(g_dumpPathLock);
		path = g_dumpPath;
	}
	if (path.empty()) {
		return nullptr;
	}
	FILE* f = std::fopen(path.c_str(), "wb");
	if (f != nullptr) {
		std::fwrite("RIFF\xff\xff\xff\x7fWAVEfmt \x10\0\0\0\x01\0\x02\0", 1, 24, f);
		const u32 rate = kOutputRate, bytes = kOutputRate * 4;
		std::fwrite(&rate, 4, 1, f);
		std::fwrite(&bytes, 4, 1, f);
		std::fwrite("\x04\0\x10\0data\xff\xff\xff\x7f", 1, 12, f);
	}
	return f;
}

void aiThreadMain()
{
	std::vector<s16> swapped;
	FILE* dump = openDump();
	for (;;) {
		std::unique_lock lk(g_aiLock);
		g_aiCv.wait_for(lk, std::chrono::milliseconds(2), [] { return g_dmaRunning && g_dmaAddr != 0; });
		if (!g_dmaRunning || g_dmaAddr == 0 || g_stream == nullptr) {
			continue;
		}
		const uintptr_t addr = g_dmaAddr;
		const u32 len        = g_dmaLen;
		const u32 gen        = g_dmaGeneration;
		lk.unlock();

		std::unique_lock streamLk(g_streamLock);
		if (g_aiQuit || g_stream == nullptr) {
			return;
		}
		// Keep about two DMA buffers queued.
		if (SDL_GetAudioStreamQueued(g_stream) > (int)len * 2) {
			streamLk.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		// The DMA buffer holds host-order s16 pairs in right/left order; SDL wants left/right.
		const u32 count = len / 2;
		swapped.resize(count);
		const s16* src = reinterpret_cast<const s16*>(addr);
		for (u32 i = 0; i + 1 < count; i += 2) {
			swapped[i]     = src[i + 1];
			swapped[i + 1] = src[i];
		}
		SDL_PutAudioStreamData(g_stream, swapped.data(), (int)(count * sizeof(s16)));
		if (dump != nullptr) {
			std::fwrite(swapped.data(), sizeof(s16), count, dump);
			std::fflush(dump);
		}

		// Like the hardware: the callback fires when the buffer starts playing, and the game
		// answers by arming the next one (AIInitDMA). Only raise it again once the game has
		// answered, so a starved audio thread does not get its small message queue flooded
		// (that drops its DSP messages and stalls the mixer); meanwhile the last buffer repeats.
		static u32 s_raisedGen = ~0u;
		static auto s_raisedAt = std::chrono::steady_clock::now();
		const auto now         = std::chrono::steady_clock::now();
		if (gen != s_raisedGen || now - s_raisedAt > std::chrono::milliseconds(50)) {
			s_raisedGen = gen;
			s_raisedAt  = now;
			p2_raise_interrupt(dmaInterrupt, nullptr);
		}
		{
			static auto s_last = std::chrono::steady_clock::now();
			if (p2_audio_debug() && std::chrono::steady_clock::now() - s_last > std::chrono::seconds(1)) {
				s_last = std::chrono::steady_clock::now();
				std::fprintf(stderr, "mppm audio: dma irqs %d, dsp frames %d, mails %d (queued %d pending %d), queued %d\n", g_dmaIrqs.load(), g_dspFrames.load(), g_mailIrqs.load(), g_mailsQueuedDbg(), g_mailsPendingDbg(), SDL_GetAudioStreamQueued(g_stream));
			}
		}
		streamLk.unlock();
		lk.lock();
		g_aiCv.wait_for(lk, std::chrono::milliseconds(30), [gen] { return g_dmaGeneration != gen || !g_dmaRunning; });
	}
}

} // namespace

extern "C" {

// Quitting: stop feeding the audio device and close it before Aurora shuts SDL down.
void p2_audio_shutdown(void)
{
	{
		std::lock_guard lk(g_aiLock);
		g_dmaRunning = false;
	}
	std::lock_guard streamLk(g_streamLock);
	g_aiQuit = true;
	if (g_stream != nullptr) {
		SDL_DestroyAudioStream(g_stream);
		g_stream = nullptr;
	}
}

void AIInit(u8*) { }

void AIInitDMA(uintptr_t addr, u32 length)
{
	std::lock_guard lk(g_aiLock);
	g_dmaAddr = addr;
	g_dmaLen  = length;
	g_dmaGeneration++;
	g_aiCv.notify_all();
}

void AIStartDMA(void)
{
	if (!p2_audio_enabled()) {
		return; // without audio the JAudio thread just waits, as before
	}
	std::lock_guard lk(g_aiLock);
	if (g_stream == nullptr) {
		SDL_InitSubSystem(SDL_INIT_AUDIO);
		SDL_AudioSpec spec{};
		spec.format   = SDL_AUDIO_S16;
		spec.channels = 2;
		spec.freq     = kOutputRate;
		g_stream      = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (g_stream != nullptr) {
			SDL_ResumeAudioStreamDevice(g_stream);
		} else {
			SDL_Log("mppm: could not open an audio device: %s", SDL_GetError());
		}
	}
	g_dmaRunning = true;
	if (!g_aiThreadStarted) {
		g_aiThreadStarted = true;
		std::thread(aiThreadMain).detach();
	}
	g_aiCv.notify_all();
}

void AIStopDMA(void)
{
	std::lock_guard lk(g_aiLock);
	g_dmaRunning = false;
	g_aiCv.notify_all();
}

AIDCallback AIRegisterDMACallback(AIDCallback cb)
{
	AIDCallback old = g_dmaCallback;
	g_dmaCallback   = cb;
	return old;
}

u32 AIGetDSPSampleRate(void) { return g_aiDspRate; }
void AISetDSPSampleRate(u32 rate) { g_aiDspRate = rate; }

} // extern "C"

// ============================================================================================
// DSP
// ============================================================================================
namespace {

constexpr int kSubframeSamples = 80;
constexpr int kMaxVoices       = 64;
constexpr int kMixers          = 6;

// AFC ADPCM predictor coefficients (JASDSPInterface.cpp DSPADPCM_FILTER), 1.0 = 0x800.
const s16 kAfcCoef[16][2] = {
	{ 0, 0 },          { 0x800, 0 },      { 0, 0x800 },      { 0x400, 0x400 },  { 0x1000, -0x800 }, { 0xE00, -0x600 },
	{ 0xC00, -0x400 }, { 0x1200, -0xA00 }, { 0x1068, -0x8C8 }, { 0x12C0, -0x8FC }, { 0x1400, -0xC00 }, { 0x800, -0x800 },
	{ 0x400, -0x400 }, { -0x400, 0x400 },  { -0x400, 0 },      { -0x800, 0 },
};

struct VoiceState {
	bool initialised = false;
	s16 block[16];       // decoded samples of the current block
	int blockPos   = 16; // next sample index in `block`
	int blockLen   = 0;
	s16 hist1      = 0;
	s16 hist2      = 0;
	s16 cur        = 0; // interpolation pair
	s16 next       = 0;
	u32 frac       = 0; // 16.16 position between cur and next
};

JASDsp::TChannel* g_voices = nullptr;
int g_voiceCount           = 0;
DSPCallback g_dspCallback  = nullptr;
VoiceState g_state[kMaxVoices];
f32 g_mixerLevel = 1.0f;

std::atomic<int> g_mailsPending{0};

const u8* aram() { return reinterpret_cast<const u8*>(ARGetStorageAddress()); }

s16 clamp16(s32 v) { return (s16)std::clamp(v, -32768, 32767); }

// Output saturation with a soft knee above about -2.7 dBFS instead of hard clipping.
s16 softClip(f32 v)
{
	constexpr f32 kKnee = 24000.0f, kRoom = 32767.0f - kKnee;
	const f32 mag       = std::fabs(v);
	if (mag > kKnee) {
		v = std::copysign(kKnee + kRoom * std::tanh((mag - kKnee) / kRoom), v);
	}
	return clamp16((s32)std::lrint(v));
}


JASDsp::TChannel::TMixer* mixerAt(JASDsp::TChannel* ch, int i)
{
	// six mixers from offset 0x10 (the struct names four; the next two live in _30)
	return reinterpret_cast<JASDsp::TChannel::TMixer*>(reinterpret_cast<u8*>(ch) + 0x10) + i;
}

// Decode the next block of the voice at its current ARAM address into st.block.
void decodeBlock(JASDsp::TChannel* ch, VoiceState& st)
{
	const u8* base = aram() + ch->mSampleOffset;
	if (ch->mSamplesPerBlock == 16 && ch->mBytesPerBlock == 9) {
		const int header = base[0];
		const int shift  = header >> 4;
		const int idx    = header & 0xF;
		for (int i = 0; i < 16; i++) {
			int nibble = (i & 1) ? (base[1 + i / 2] & 0xF) : (base[1 + i / 2] >> 4);
			if (nibble >= 8) {
				nibble -= 16;
			}
			s32 sample = ((nibble << shift) << 11) + kAfcCoef[idx][0] * st.hist1 + kAfcCoef[idx][1] * st.hist2;
			const s16 out = clamp16(sample >> 11);
			st.hist2      = st.hist1;
			st.hist1      = out;
			st.block[i]   = out;
		}
		st.blockLen = 16;
		ch->mSampleOffset += 9;
	} else if (ch->mSamplesPerBlock == 16 && ch->mBytesPerBlock == 5) {
		const int header = base[0];
		const int shift  = header >> 4;
		const int idx    = header & 0xF;
		for (int i = 0; i < 16; i++) {
			int crumb = (base[1 + i / 4] >> (6 - 2 * (i & 3))) & 3;
			if (crumb >= 2) {
				crumb -= 4;
			}
			s32 sample = ((crumb << shift) << 13) + kAfcCoef[idx][0] * st.hist1 + kAfcCoef[idx][1] * st.hist2;
			const s16 out = clamp16(sample >> 11);
			st.hist2      = st.hist1;
			st.hist1      = out;
			st.block[i]   = out;
		}
		st.blockLen = 16;
		ch->mSampleOffset += 5;
	} else if (ch->mBytesPerBlock == 16) { // PCM16, big-endian in ARAM
		st.block[0] = (s16)((base[0] << 8) | base[1]);
		st.blockLen = 1;
		ch->mSampleOffset += 2;
	} else { // PCM8
		st.block[0] = (s16)((s8)base[0] << 8);
		st.blockLen = 1;
		ch->mSampleOffset += 1;
	}
	st.blockPos = 0;
}

u32 blockBytes(const JASDsp::TChannel* ch)
{
	if (ch->mSamplesPerBlock == 16) {
		return ch->mBytesPerBlock;
	}
	return ch->mBytesPerBlock == 16 ? 2 : 1;
}

// Seek to sample `pos` (relative to mDataOffset) and prime the block decoder there.
void seekVoice(JASDsp::TChannel* ch, VoiceState& st, u32 pos)
{
	const u32 spb     = ch->mSamplesPerBlock ? ch->mSamplesPerBlock : 1;
	ch->mSampleOffset = ch->mDataOffset + (pos / spb) * blockBytes(ch);
	decodeBlock(ch, st);
	st.blockPos = pos % spb;
}

// Next input sample; false when a non-looping voice has run out.
bool fetchSample(JASDsp::TChannel* ch, VoiceState& st, s16& out)
{
	if ((s32)ch->mCurrentSampleOffset <= 0) {
		if (ch->mLoopOffset == 0) {
			return false;
		}
		st.hist1 = ch->mLast;
		st.hist2 = ch->mPenult;
		seekVoice(ch, st, ch->mLoopStartOffset);
		ch->mCurrentSampleOffset = ch->mNextSampleOffset - ch->mLoopStartOffset;
		if ((s32)ch->mCurrentSampleOffset <= 0) {
			return false;
		}
	}
	if (st.blockPos >= st.blockLen) {
		decodeBlock(ch, st);
	}
	out = st.block[st.blockPos++];
	ch->mCurrentSampleOffset--;
	return true;
}

std::atomic<int> g_voiceStarts{0};
bool vlog() { return g_voiceLog.load(std::memory_order_relaxed); }
void startVoice(JASDsp::TChannel* ch, VoiceState& st)
{
	g_voiceStarts++;
	if (vlog()) {
		std::fprintf(stderr, "VSTART %p spb=%d bpb=%d pitch=%04x data=%x blk=%x len=%x loop=%d ls=%x auto=%d lvl=%x m0=%x/%x m1=%x/%x\n", (void*)ch,
		             ch->mSamplesPerBlock, ch->mBytesPerBlock, (u16)ch->mPitch, ch->mDataOffset, ch->mBlockCount, ch->mNextSampleOffset,
		             ch->mLoopOffset, ch->mLoopStartOffset, ch->mIsMixerInitialized, ch->mMixerLevel, mixerAt(ch, 0)->mBusConnect,
		             (u16)mixerAt(ch, 0)->mMixVolume, mixerAt(ch, 1)->mBusConnect, (u16)mixerAt(ch, 1)->mMixVolume);
	}
	st              = VoiceState{};
	st.initialised  = true;
	const u32 start = (ch->mSamplesPerBlock == 1) ? ch->mBlockCount : 0; // PCM may start mid-wave
	ch->mCurrentSampleOffset = ch->mNextSampleOffset > start ? ch->mNextSampleOffset - start : 0;
	if (ch->mBytesPerBlock >= 4) {
		seekVoice(ch, st, start);
	}
	s16 s = 0;
	fetchSample(ch, st, s);
	st.cur = s;
	fetchSample(ch, st, s);
	st.next = s;
	ch->mIsPlaying = 0;
}

f32* busFor(u16 bus, f32* frontL, f32* frontR, f32* backL, f32* backR)
{
	switch (bus) {
	case 0xD00:
		return frontL;
	case 0xD60:
		return frontR;
	case 0xDC0:
		return backL;
	case 0xE20:
		return backR;
	default:
		return nullptr; // reverb sends and unused buses
	}
}

void renderVoice(int index, f32* frontL, f32* frontR, f32* backL, f32* backR, int count)
{
	JASDsp::TChannel* ch = &g_voices[index];
	VoiceState& st       = g_state[index];
	if (!ch->mIsActive || ch->mIsFinished) {
		if (vlog() && st.initialised) std::fprintf(stderr, "VSTOP %p inactive act=%d fin=%d left=%d\n", (void*)ch, ch->mIsActive, ch->mIsFinished, (s32)ch->mCurrentSampleOffset);
		st.initialised = false;
		return;
	}
	if (ch->mForcedStop) {
		if (vlog() && st.initialised) std::fprintf(stderr, "VSTOP %p forced left=%d\n", (void*)ch, (s32)ch->mCurrentSampleOffset);
		ch->mIsFinished = 1;
		st.initialised  = false;
		return;
	}
	if (ch->mIsPlaying || !st.initialised) {
		if (ch->mBytesPerBlock < 4) {
			ch->mIsPlaying = 0; // oscillator voices are not synthesised yet
			st.initialised = true;
			return;
		}
		startVoice(ch, st);
	}
	if (ch->mPauseFlag || ch->mBytesPerBlock < 4) {
		return;
	}

	// Resample this subframe into a scratch buffer.
	f32 voice[kSubframeSamples * 8];
	const u32 step = (u32)(u16)ch->mPitch << 4; // 0x1000 → 0x10000
	bool ended     = false;
	for (int n = 0; n < count; n++) {
		voice[n] = (st.cur + (st.next - st.cur) * (f32)(st.frac & 0xFFFF) / 65536.0f) / 32768.0f;
		st.frac += step;
		while (st.frac >= 0x10000) {
			st.frac -= 0x10000;
			st.cur = st.next;
			s16 s  = 0;
			if (!ended && !fetchSample(ch, st, s)) {
				ended = true;
			}
			st.next = ended ? 0 : s;
		}
	}

	// Auto mixer (mix config 0xFFFF, set up through initAutoMixer/setAutoMixer): level in
	// mMixerLevel, pan in the high byte of mVolumeAndPan (0..127). The six bus mixers are left
	// uninitialised by the game in this mode.
	if (ch->mIsMixerInitialized) {
		const f32 from = ch->mCurrentMixerValue / 32768.0f;
		const f32 to   = ch->mMixerLevel / 32768.0f;
		ch->mCurrentMixerValue = ch->mMixerLevel;
		const f32 pan  = std::clamp((ch->mVolumeAndPan >> 8) / 127.0f, 0.0f, 1.0f);
		const f32 gl   = std::cos(pan * 1.5707963f);
		const f32 gr   = std::sin(pan * 1.5707963f);
		for (int n = 0; n < count; n++) {
			const f32 v = voice[n] * (from + (to - from) * n / count);
			frontL[n] += v * gl;
			frontR[n] += v * gr;
		}
	} else {
		// Bus mixers, Q15 volumes ramping from the current to the target value over the subframe.
		for (int m = 0; m < kMixers; m++) {
			JASDsp::TChannel::TMixer* mixer = mixerAt(ch, m);
			f32* bus       = busFor(mixer->mBusConnect, frontL, frontR, backL, backR);
			const f32 from = (s16)mixer->mBaseVolume / 32768.0f;
			const f32 to   = mixer->mMixVolume / 32768.0f;
			mixer->mBaseVolume = mixer->mMixVolume;
			if (bus == nullptr || (from == 0.0f && to == 0.0f)) {
				continue;
			}
			for (int n = 0; n < count; n++) {
				bus[n] += voice[n] * (from + (to - from) * n / count);
			}
		}
	}

	if (ended) {
		if (vlog()) std::fprintf(stderr, "VSTOP %p ended\n", (void*)ch);
		ch->mIsFinished = 1;
		st.initialised  = false;
	}
}

std::mutex g_mailLock;
std::condition_variable g_mailCv;
int g_mailsQueued         = 0;
bool g_frameRequested     = false;
std::chrono::steady_clock::time_point g_lastFrameRequest;
bool g_mailThreadStarted  = false;

void mailInterrupt(void*);
int g_mailsQueuedDbg() { return g_mailsQueued; }
int g_mailsPendingDbg() { return g_mailsPending.load(); }

// Deliver queued mails: the next one only after the previous was read, with a short gap so the
// audio thread can run its subframe update in between.
void mailThreadMain()
{
	for (;;) {
		std::unique_lock lk(g_mailLock);
		g_mailCv.wait_for(lk, std::chrono::milliseconds(5), [] { return g_mailsQueued > 0; });
		// Watchdog: if a "subframe done" message got lost the audio thread waits for it forever
		// (it never asks for the next frame). Hand out one more mail after a long silence.
		if (g_mailsQueued == 0 && g_mailsPending.load() == 0 && g_frameRequested
		    && std::chrono::steady_clock::now() - g_lastFrameRequest > std::chrono::milliseconds(150)) {
			g_lastFrameRequest = std::chrono::steady_clock::now();
			g_mailsQueued++;
		}
		if (g_mailsQueued == 0 || g_mailsPending.load() > 0) {
			lk.unlock();
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			continue;
		}
		g_mailsQueued--;
		lk.unlock();
		g_mailsPending.fetch_add(1);
		p2_raise_interrupt(mailInterrupt, nullptr);
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
}

void mailInterrupt(void*)
{
	g_mailIrqs++;
	if (g_dspCallback) {
		g_dspCallback(nullptr);
	}
}

} // namespace

// JAudio declares these with C++ linkage (JSystem/JAudio/DSP.h).
void DspBoot(DSPCallback callback) { g_dspCallback = callback; }
void DspFinishWork(u16) { }
void DSPReleaseHalt2(u32) { }
void DsetMixerLevel(f32 level) { g_mixerLevel = level; }

void DsetupTable(u32 count, uptr voices, uptr, uptr, uptr)
{
	g_voices     = reinterpret_cast<JASDsp::TChannel*>(voices);
	g_voiceCount = std::min<int>(count, kMaxVoices);
}

void DsyncFrame2(u32 subframes, uptr left, uptr right)
{
	g_dspFrames++;
	s16* outL       = reinterpret_cast<s16*>(left);
	s16* outR       = reinterpret_cast<s16*>(right);
	const int total = (int)subframes * kSubframeSamples;
	std::vector<f32> fl(total), fr(total), bl(total), br(total);
	// Menu cue voices (see g_menuHold) mix into their own pair, which the duck does not touch. A
	// voice stays a cue voice until it ends, release tail included.
	static bool s_uiVoice[kMaxVoices] = {};
	const bool held = g_menuHold.load(std::memory_order_relaxed);
	bool anyUi      = false;
	if (held && g_voices != nullptr) {
		const unsigned long long mask = p2_game_ui_voice_mask();
		for (int v = 0; v < g_voiceCount; v++) {
			if (!g_voices[v].mIsActive || g_voices[v].mIsFinished) {
				s_uiVoice[v] = false;
			} else if (v < 64 && (mask >> v) & 1) {
				s_uiVoice[v] = true;
			}
			anyUi = anyUi || s_uiVoice[v];
		}
	} else {
		std::fill(std::begin(s_uiVoice), std::end(s_uiVoice), false);
	}
	std::vector<f32> ul, ur, ubl, ubr;
	if (anyUi) {
		ul.assign(total, 0.0f);
		ur.assign(total, 0.0f);
		ubl.assign(total, 0.0f);
		ubr.assign(total, 0.0f);
	}
	if (g_voices != nullptr) {
		for (int sf = 0; sf < (int)subframes; sf++) {
			const int at = sf * kSubframeSamples;
			for (int v = 0; v < g_voiceCount; v++) {
				if (anyUi && s_uiVoice[v]) {
					renderVoice(v, &ul[at], &ur[at], &ubl[at], &ubr[at], kSubframeSamples);
				} else {
					renderVoice(v, &fl[at], &fr[at], &bl[at], &br[at], kSubframeSamples);
				}
			}
		}
	}
	static int s_frames = 0;
	if (p2_audio_debug() && g_voices != nullptr && (s_frames++ % 60) == 0) {
		int active = 0;
		for (int v = 0; v < g_voiceCount; v++) {
			JASDsp::TChannel* ch = &g_voices[v];
			if (ch->mIsActive) {
				active++;
				if (active <= 4) {
					std::fprintf(stderr, "mppm audio: v%d fin=%d play=%d pitch=%04x spb=%d bpb=%d data=%x cur=%x left=%d loop=%d mix0=%x/%x/%x mix1=%x/%x/%x auto=%x/%x\n", v,
					             ch->mIsFinished, ch->mIsPlaying, (u16)ch->mPitch, ch->mSamplesPerBlock, ch->mBytesPerBlock, ch->mDataOffset,
					             ch->mSampleOffset, (s32)ch->mCurrentSampleOffset, ch->mLoopOffset, mixerAt(ch, 0)->mBusConnect,
					             (u16)mixerAt(ch, 0)->mMixVolume, mixerAt(ch, 0)->mBaseVolume, mixerAt(ch, 1)->mBusConnect,
					             (u16)mixerAt(ch, 1)->mMixVolume, mixerAt(ch, 1)->mBaseVolume, ch->mMixerLevel, ch->mVolumeAndPan);
				}
			}
		}
		std::fprintf(stderr, "mppm audio: %d active voices, %d started so far, mixer level %f\n", active, g_voiceStarts.load(), g_mixerLevel);
	}
	const f32 gain = 32767.0f * (g_mixerLevel > 0.0f ? g_mixerLevel : 1.0f) * g_masterGain.load(std::memory_order_relaxed);
	// The duck gain glides to its target (about 0.1 s for a full swing) so muting never clicks.
	static f32 s_duck        = 1.0f;
	const f32 duckTarget     = g_duckTarget.load(std::memory_order_relaxed);
	constexpr f32 kDuckStep  = 1.0f / 3200.0f;
	static f32 s_uiGain      = 1.0f;
	const f32 uiGainTarget   = g_uiGainTarget.load(std::memory_order_relaxed);
	for (int n = 0; n < total; n++) {
		if (s_duck != duckTarget) {
			s_duck = s_duck < duckTarget ? std::min(duckTarget, s_duck + kDuckStep) : std::max(duckTarget, s_duck - kDuckStep);
		}
		if (s_uiGain != uiGainTarget) {
			s_uiGain = s_uiGain < uiGainTarget ? std::min(uiGainTarget, s_uiGain + kDuckStep)
			                                   : std::max(uiGainTarget, s_uiGain - kDuckStep);
		}
		fl[n] *= s_duck;
		fr[n] *= s_duck;
		if (anyUi) {
			fl[n] += ul[n] * s_uiGain;
			fr[n] += ur[n] * s_uiGain;
		}
		// bl/br collect the 0xDC0/0xE20 sends, which the default mix configs scale by the FX mix
		// (reverb sends); there is no reverb yet, so they are not part of the dry output.
		outL[n] = softClip(fl[n] * gain);
		outR[n] = softClip(fr[n] * gain);
	}
	// One "subframe done" mail per subframe, handed out one at a time by the mail thread (the
	// audio thread's message queue is small; mails sent all at once would be dropped).
	{
		std::lock_guard lk(g_mailLock);
		g_mailsQueued += (int)subframes;
		g_frameRequested   = true;
		g_lastFrameRequest = std::chrono::steady_clock::now();
		if (!g_mailThreadStarted) {
			g_mailThreadStarted = true;
			std::thread(mailThreadMain).detach();
		}
	}
	g_mailCv.notify_all();
}

extern "C" {

u32 DSPCheckMailFromDSP(void) { return g_mailsPending.load() > 0 ? 1 : 0; }

u32 DSPReadMailFromDSP(void)
{
	if (g_mailsPending.load() > 0) {
		g_mailsPending.fetch_sub(1);
	}
	return 0xF355FF00;
}

} // extern "C"
