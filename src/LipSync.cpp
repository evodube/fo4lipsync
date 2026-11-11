#include "LipSync.h"

#include "Settings.h"

namespace LipSync
{
	namespace
	{
		using clock = std::chrono::steady_clock;

		// ------------------------------------------------------------------
		// Engine layouts (same on 1.10.163, 1.10.980/984 and 1.11.x)
		// ------------------------------------------------------------------

		// Lip file data the engine plays for the current line.
		struct LipSynchAnim
		{
			std::uint32_t numFrames;          // 00
			std::int32_t  startingFrame;      // 04
			std::uint32_t numPhonemeTargets;  // 08
			std::uint32_t pad0C;              // 0C
			float**       p_phonemes;         // 10 - row pointers into phonemes
			float**       modifiers;          // 18
			float*        phonemes;           // 20 - backing store
			bool          hasGestures;        // 28
			std::uint32_t versionNumber;      // 2C
			std::byte     voiceData[0x18];    // 30 - BSExternalAudioIO::ExternalLoad
		};
		static_assert(offsetof(LipSynchAnim, p_phonemes) == 0x10);
		static_assert(offsetof(LipSynchAnim, phonemes) == 0x20);
		static_assert(sizeof(LipSynchAnim) == 0x48);

		// HighProcessData fields on 1.11.x (AIProcess::high); used only as hints, the real
		// lip offset is found at runtime (see Layout discovery)
		constexpr std::ptrdiff_t kHigh_VoiceTimer = 0x3F0;
		constexpr std::ptrdiff_t kHigh_LipSynchAnim = 0x4A0;
		// Actor field
		constexpr std::ptrdiff_t kActor_VoiceTimer = 0x38C;

		// Actor::Update(float) / PlayerCharacter::Update(float)
		constexpr std::size_t kUpdateVfunc = 0xCF;

		template <class T>
		T& Field(void* a_base, std::ptrdiff_t a_offset)
		{
			return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(a_base) + a_offset);
		}

		bool PlausiblePointer(const void* a_ptr)
		{
			const auto p = reinterpret_cast<std::uintptr_t>(a_ptr);
			return p > 0x10000 && p < 0x7FFF'FFFF'FFFF && (p & 0x7) == 0;
		}

		// True when [a_ptr, a_ptr + a_size) is committed memory we may read (and write, if asked).
		bool Accessible(const void* a_ptr, std::size_t a_size, bool a_write)
		{
			namespace W32 = REX::W32;

			const auto p = reinterpret_cast<std::uintptr_t>(a_ptr);
			if (p < 0x10000 || p >= 0x7FFF'FFFF'FFFF || (p & 0x3) != 0) {
				return false;
			}

			auto       cur = reinterpret_cast<std::uintptr_t>(a_ptr);
			const auto end = cur + a_size;
			while (cur < end) {
				W32::MEMORY_BASIC_INFORMATION mbi{};
				if (W32::VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi)) != sizeof(mbi)) {
					return false;
				}
				if (mbi.state != W32::MEM_COMMIT || (mbi.protect & 0x100u) != 0) {  // 0x100 = PAGE_GUARD
					return false;
				}
				const auto prot = mbi.protect & 0xFFu;
				const bool readable = prot == W32::PAGE_READONLY || prot == W32::PAGE_READWRITE || prot == W32::PAGE_WRITECOPY ||
				                      prot == W32::PAGE_EXECUTE_READ || prot == W32::PAGE_EXECUTE_READWRITE;
				const bool writable = prot == W32::PAGE_READWRITE || prot == W32::PAGE_WRITECOPY || prot == W32::PAGE_EXECUTE_READWRITE;
				if (!readable || (a_write && !writable)) {
					return false;
				}
				const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.baseAddress) + mbi.regionSize;
				if (regionEnd <= cur) {
					return false;
				}
				cur = regionEnd;
			}
			return true;
		}

		bool PlausibleAnim(const LipSynchAnim* a_anim)
		{
			return PlausiblePointer(a_anim) &&
			       a_anim->numFrames > 0 && a_anim->numFrames < 200000 &&
			       a_anim->numPhonemeTargets > 0 && a_anim->numPhonemeTargets <= 128;
		}

		// ------------------------------------------------------------------
		// Smoothing
		// ------------------------------------------------------------------

		// Gaussian blur of a strided curve, blended with the original.
		void SmoothCurve(float* a_data, std::size_t a_count, std::size_t a_stride, std::span<const float> a_kernel, float a_strength, std::vector<float>& a_scratch)
		{
			const auto radius = static_cast<std::ptrdiff_t>(a_kernel.size() / 2);
			const auto n = static_cast<std::ptrdiff_t>(a_count);

			a_scratch.resize(a_count);
			for (std::ptrdiff_t i = 0; i < n; ++i) {
				a_scratch[i] = a_data[i * a_stride];
			}

			for (std::ptrdiff_t i = 0; i < n; ++i) {
				float sum = 0.0f;
				for (std::ptrdiff_t k = -radius; k <= radius; ++k) {
					const auto j = std::clamp<std::ptrdiff_t>(i + k, 0, n - 1);
					sum += a_scratch[j] * a_kernel[k + radius];
				}
				const float orig = a_scratch[i];
				a_data[i * a_stride] = orig + (sum - orig) * a_strength;
			}
		}

		std::vector<float> BuildKernel(float a_sigma)
		{
			const auto radius = std::max(1, static_cast<int>(std::ceil(a_sigma * 2.5f)));
			std::vector<float> kernel(radius * 2 + 1);
			float              total = 0.0f;
			for (int k = -radius; k <= radius; ++k) {
				const float w = std::exp(-(k * k) / (2.0f * a_sigma * a_sigma));
				kernel[k + radius] = w;
				total += w;
			}
			for (auto& w : kernel) {
				w /= total;
			}
			return kernel;
		}

		// Fingerprint of a lip anim's phoneme data, so data we already blurred
		// (the engine reusing a cached line, or two actors sharing one) isn't
		// blurred again.
		std::uint64_t Fingerprint(const LipSynchAnim* a_anim)
		{
			const std::size_t count = static_cast<std::size_t>(a_anim->numFrames) * a_anim->numPhonemeTargets;
			std::uint64_t     hash = 0xcbf29ce484222325ull;
			hash = (hash ^ reinterpret_cast<std::uintptr_t>(a_anim->phonemes)) * 0x100000001b3ull;
			hash = (hash ^ count) * 0x100000001b3ull;
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_anim->phonemes);
			for (std::size_t i = 0; i < count * sizeof(float); ++i) {
				hash = (hash ^ bytes[i]) * 0x100000001b3ull;
			}
			return hash;
		}

		std::unordered_set<std::uint64_t> g_smoothedPrints;

		// Returns false when the data layout isn't one we recognise (left untouched).
		bool SmoothAnim(LipSynchAnim* a_anim)
		{
			const std::size_t frames = a_anim->numFrames;
			const std::size_t targets = a_anim->numPhonemeTargets;
			float**           rows = a_anim->p_phonemes;
			float*            base = a_anim->phonemes;

			if (frames < 3 || targets < 2 || frames == targets) {
				return false;
			}
			if (!PlausiblePointer(rows) || !PlausiblePointer(base) ||
			    !Accessible(rows, sizeof(float*) * 2, false) || rows[0] != base) {
				return false;
			}
			if (!Accessible(base, frames * targets * sizeof(float), true)) {
				return false;
			}

			static thread_local std::vector<float> scratch;
			static const auto                      kernel = BuildKernel(Settings::fSmoothRadius);
			const float                            strength = Settings::fSmoothing;

			const auto stride = rows[1] - rows[0];
			const bool perTarget = stride == static_cast<std::ptrdiff_t>(frames);
			const bool perFrame = stride == static_cast<std::ptrdiff_t>(targets);
			if (!perTarget && !perFrame) {
				return false;
			}

			if (g_smoothedPrints.contains(Fingerprint(a_anim))) {
				return true;  // already smoothed
			}

			for (std::size_t t = 0; t < targets; ++t) {
				if (perTarget) {
					// one contiguous curve per phoneme target
					SmoothCurve(base + t * frames, frames, 1, kernel, strength, scratch);
				} else {
					// one row of target weights per frame
					SmoothCurve(base + t, frames, targets, kernel, strength, scratch);
				}
			}

			if (g_smoothedPrints.size() > 4096) {
				g_smoothedPrints.clear();
			}
			g_smoothedPrints.insert(Fingerprint(a_anim));
			return true;
		}

		// ------------------------------------------------------------------
		// Layout discovery
		// ------------------------------------------------------------------
		//
		// HighProcessData's layout differs between runtimes, so instead of trusting
		// fixed offsets we find the lip data at runtime: scan a talking actor's
		// HighProcessData for pointers shaped like LipSynchAnim, then watch the
		// floats stored just after each one; the one that runs forward like a clock
		// for several frames while the pointer stays put is the lip time. Nothing
		// is written until both offsets are confirmed.

		constexpr std::ptrdiff_t kScanBegin = 0x0;
		constexpr std::ptrdiff_t kScanEnd = 0x800;
		constexpr std::ptrdiff_t kClockSearch = 0x20;  // look for the lip time this far past the pointer
		constexpr std::ptrdiff_t kVoiceBeforeAnim = kHigh_LipSynchAnim - kHigh_VoiceTimer;  // 0xB0 on 1.11.x

		std::atomic<std::ptrdiff_t> g_lipOffset{ -1 };      // confirmed lipSynchAnim offset
		std::atomic<std::ptrdiff_t> g_lipTimeOffset{ -1 };  // confirmed lipTime offset
		std::atomic<std::ptrdiff_t> g_voiceOffset{ -1 };    // confirmed voiceTimer offset
		std::atomic<bool>           g_voiceGaveUp{ false };

		// Loose shape check used both while hunting and to validate each new line.
		bool LooksLikeLipAnim(const void* a_ptr)
		{
			if (!PlausiblePointer(a_ptr) || !Accessible(a_ptr, sizeof(LipSynchAnim), false)) {
				return false;
			}
			const auto* anim = static_cast<const LipSynchAnim*>(a_ptr);
			return PlausibleAnim(anim) &&
			       anim->versionNumber <= 64 &&
			       PlausiblePointer(anim->p_phonemes) &&
			       (anim->phonemes == nullptr || PlausiblePointer(anim->phonemes)) &&
			       Accessible(anim->p_phonemes, sizeof(float*), false);
		}

		struct Clock
		{
			std::ptrdiff_t offset;
			float          last;
			int            hits;
			bool           dead;
		};

		struct Candidate
		{
			std::ptrdiff_t     offset;
			const void*        anim;
			std::vector<Clock> clocks;
		};

		struct Probe
		{
			std::vector<Candidate> candidates;
			clock::time_point      lastScan{};
		};

		struct DiscoveryStats
		{
			std::uint64_t scans{ 0 };
			std::uint64_t shapeMatches{ 0 };
			std::uint64_t droppedPointerChanged{ 0 };
			std::uint64_t droppedNoClock{ 0 };
			std::uint32_t samplesLogged{ 0 };
			std::set<std::ptrdiff_t> sampledOffsets;
		};

		std::unordered_map<RE::Actor*, Probe> g_probes;
		clock::time_point                     g_lastAnyScan{};
		clock::time_point                     g_discoveryStart{};
		clock::time_point                     g_lastStatsLog{};
		DiscoveryStats                        g_stats;

		void LogSample(RE::Actor* a_actor, void* a_high, std::ptrdiff_t a_offset, const LipSynchAnim* a_anim)
		{
			if (g_stats.samplesLogged >= 12 || !g_stats.sampledOffsets.insert(a_offset).second) {
				return;
			}
			++g_stats.samplesLogged;

			std::string floats;
			for (auto o = a_offset + 8; o < a_offset + 8 + kClockSearch; o += 4) {
				floats += std::format(" +0x{:X}={:g}", o - a_offset, Field<float>(a_high, o));
			}
			const auto stride = a_anim->phonemes && Accessible(a_anim->p_phonemes, sizeof(float*) * 2, false) ?
			                        a_anim->p_phonemes[1] - a_anim->p_phonemes[0] :
			                        0;
			logger::info(
				"probe: actor {:08X} +0x{:X} -> frames {} start {} targets {} version {} gestures {} rows[0]==data {} stride {} | after:{}",
				a_actor->formID, a_offset, a_anim->numFrames, a_anim->startingFrame, a_anim->numPhonemeTargets,
				a_anim->versionNumber, a_anim->hasGestures,
				a_anim->phonemes && a_anim->p_phonemes[0] == a_anim->phonemes, stride, floats);
		}

		void LogStats(clock::time_point a_now)
		{
			if (a_now - g_lastStatsLog < 60s) {
				return;
			}
			g_lastStatsLog = a_now;
			logger::info(
				"still looking for lip data: {} scans, {} lip-shaped pointers, dropped {} (pointer changed) / {} (no running clock next to it)",
				g_stats.scans, g_stats.shapeMatches, g_stats.droppedPointerChanged, g_stats.droppedNoClock);
		}

		// Returns true once the lip offsets are known.
		bool Discover(RE::Actor* a_actor, void* a_high, clock::time_point a_now)
		{
			if (g_lipOffset >= 0) {
				return true;
			}
			if (g_discoveryStart == clock::time_point{}) {
				g_discoveryStart = a_now;
				g_lastStatsLog = a_now;
			}
			LogStats(a_now);

			auto& probe = g_probes[a_actor];

			// follow up on candidates from an earlier scan
			if (!probe.candidates.empty()) {
				std::erase_if(probe.candidates, [&](Candidate& c) {
					if (Field<const void*>(a_high, c.offset) != c.anim) {
						++g_stats.droppedPointerChanged;
						return true;
					}
					bool anyAlive = false;
					for (auto& k : c.clocks) {
						if (k.dead) {
							continue;
						}
						const float v = Field<float>(a_high, k.offset);
						if (!std::isfinite(v) || v < k.last - 0.001f || v - k.last > 1.0f) {
							k.dead = true;
							continue;
						}
						if (v > k.last) {
							++k.hits;
						}
						k.last = v;
						anyAlive = true;
					}
					if (!anyAlive) {
						++g_stats.droppedNoClock;
						return true;
					}
					return false;
				});

				for (const auto& c : probe.candidates) {
					for (const auto& k : c.clocks) {
						if (!k.dead && k.hits >= 4) {
							g_lipTimeOffset = k.offset;
							g_lipOffset = c.offset;
							logger::info(
								"found lip data at HighProcessData+0x{:X}, lip time at +0x{:X} (1.11.x layout: +0x{:X} / +0x{:X})",
								c.offset, k.offset, kHigh_LipSynchAnim, kHigh_LipSynchAnim + 8);
							g_probes.clear();
							return true;
						}
					}
				}
				if (!probe.candidates.empty()) {
					return false;
				}
			}

			// scan this actor (throttled: one scan per actor per 0.25s, at most one every 5ms overall)
			if (a_now - probe.lastScan < 250ms || a_now - g_lastAnyScan < 5ms) {
				return false;
			}
			probe.lastScan = a_now;
			g_lastAnyScan = a_now;
			++g_stats.scans;

			if (!Accessible(static_cast<std::byte*>(a_high) + kScanBegin, kScanEnd + kClockSearch + 8 - kScanBegin, false)) {
				return false;
			}

			for (auto off = kScanBegin; off < kScanEnd; off += 8) {
				const auto* ptr = Field<const void*>(a_high, off);
				if (!LooksLikeLipAnim(ptr)) {
					continue;
				}
				++g_stats.shapeMatches;
				LogSample(a_actor, a_high, off, static_cast<const LipSynchAnim*>(ptr));

				Candidate c{ off, ptr, {} };
				for (auto o = off + 8; o < off + 8 + kClockSearch; o += 4) {
					const float v = Field<float>(a_high, o);
					if (std::isfinite(v) && v >= 0.0f && v < 3600.0f) {
						c.clocks.push_back({ o, v, 0, false });
					}
				}
				if (!c.clocks.empty()) {
					probe.candidates.push_back(std::move(c));
				}
			}
			return false;
		}

		// ------------------------------------------------------------------
		// Real-time sync
		// ------------------------------------------------------------------

		struct Track
		{
			float written{ 0.0f };
			float pending{ 0.0f };
			bool  init{ false };
		};

		// Watches a float we don't fully trust yet; it must tick like a timer
		// (small, same-direction steps each frame) before we correct it.
		struct Trial
		{
			float last{ 0.0f };
			int   hits{ 0 };
			int   sign{ 0 };
			bool  init{ false };
			bool  failed{ false };
		};

		struct ActorState
		{
			const LipSynchAnim* anim{ nullptr };
			bool                valid{ false };
			clock::time_point   last{};
			Track               lip;
			Track               voice;
			Track               actorVoice;
			Trial               voiceTrial;
			double              engineAdvance{ 0.0 };  // what the engine tried to add to lip time this line
			double              realAdvance{ 0.0 };    // what we used instead
			bool                smoothed{ false };
		};

		std::mutex                                 g_lock;
		std::unordered_map<RE::Actor*, ActorState> g_states;
		std::atomic<bool>                          g_disabled{ false };
		std::uint32_t                              g_badLines{ 0 };
		std::uint64_t                              g_lines{ 0 };
		double                                     g_totalEngine{ 0.0 };
		double                                     g_totalReal{ 0.0 };
		std::uint64_t                              g_smoothFailures{ 0 };
		std::uint32_t                              g_voiceTrialLines{ 0 };

		struct Step
		{
			float engine{ 0.0f };  // what the engine tried to add
			float real{ 0.0f };    // what we added instead
		};

		// Rescales the engine's per-frame change of a_field to real elapsed time.
		Step Correct(Track& a_track, float& a_field, float a_realDt, float a_initOffset = 0.0f)
		{
			if (!a_track.init) {
				if (a_initOffset != 0.0f) {
					a_field = std::max(0.0f, a_field + a_initOffset);
				}
				a_track = { a_field, 0.0f, true };
				return {};
			}

			const float engineDelta = a_field - a_track.written;

			if (engineDelta == 0.0f) {
				// engine didn't advance (paused, menu, or a second update this frame)
				a_track.pending += a_realDt;
				if (a_track.pending > Settings::fMaxStep) {
					a_track.pending = 0.0f;
				}
				return {};
			}

			if (std::abs(engineDelta) > Settings::fMaxStep * 4.0f) {
				// engine reset or jumped the timer on purpose - accept it
				a_track = { a_field, 0.0f, true };
				return {};
			}

			const float real = std::min(a_track.pending + a_realDt, 1.0f);
			a_track.pending = 0.0f;

			float value = a_track.written + std::copysign(real, engineDelta);
			if (a_track.written >= 0.0f && a_field >= 0.0f && value < 0.0f) {
				value = 0.0f;  // countdown finished; don't overshoot below zero
			}

			a_field = value;
			a_track.written = value;
			return { std::abs(engineDelta), real };
		}

		// Feeds one frame of a candidate voice timer; confirms it after steady ticking.
		void TryVoiceOffset(ActorState& a_state, void* a_high, std::ptrdiff_t a_offset, float a_realDt)
		{
			auto& t = a_state.voiceTrial;
			if (t.failed) {
				return;
			}
			const float v = Field<float>(a_high, a_offset);
			if (!std::isfinite(v) || v < -1.0f || v > 3600.0f) {
				t.failed = true;
				return;
			}
			if (!t.init) {
				t = { v, 0, 0, true, false };
				return;
			}
			const float d = v - t.last;
			t.last = v;
			if (d == 0.0f) {
				return;
			}
			const int sign = d > 0.0f ? 1 : -1;
			if (std::abs(d) > 0.5f || (t.sign != 0 && sign != t.sign) || a_realDt <= 0.0f || std::abs(d) > a_realDt * 6.0f) {
				t.failed = true;
				return;
			}
			t.sign = sign;
			if (++t.hits >= 6) {
				g_voiceOffset = a_offset;
				logger::info("found voice timer at HighProcessData+0x{:X}", a_offset);
			}
		}

		void FinishLine(RE::Actor* a_actor, const ActorState& a_state)
		{
			if (!a_state.valid || a_state.realAdvance <= 0.0) {
				return;
			}

			++g_lines;
			g_totalEngine += a_state.engineAdvance;
			g_totalReal += a_state.realAdvance;

			const auto ratio = a_state.engineAdvance / a_state.realAdvance;
			if (Settings::bDebugLog || g_lines <= 5) {
				logger::info(
					"line done: actor {:08X} lip engine {:.2f}s vs real {:.2f}s (engine speed x{:.2f}){}",
					a_actor->formID, a_state.engineAdvance, a_state.realAdvance, ratio,
					a_state.smoothed ? ", smoothed" : "");
			}
			if (g_lines % 50 == 0) {
				logger::info(
					"{} lines so far: engine lip clock ran at x{:.2f} real time on average",
					g_lines, g_totalEngine / g_totalReal);
			}
		}

		void Process(RE::Actor* a_actor)
		{
			auto* process = a_actor->currentProcess;
			if (!process || !process->high) {
				return;
			}
			void*      high = process->high;
			const auto now = clock::now();

			std::scoped_lock lock(g_lock);

			if (!Discover(a_actor, high, now)) {
				return;
			}

			const auto lipOffset = g_lipOffset.load();
			const auto* anim = Field<const LipSynchAnim*>(high, lipOffset);

			if (!anim) {
				if (const auto it = g_states.find(a_actor); it != g_states.end()) {
					FinishLine(a_actor, it->second);
					g_states.erase(it);
				}
				return;
			}

			auto& state = g_states[a_actor];

			if (state.anim != anim) {
				// new line started
				if (state.anim) {
					FinishLine(a_actor, state);
				}
				state = {};
				state.anim = anim;
				state.last = now;
				state.valid = LooksLikeLipAnim(anim);

				if (!state.valid) {
					if (++g_badLines >= 5) {
						g_disabled = true;
						logger::error("lip data stopped matching its expected layout 5 lines in a row - turning the fix off for this session");
					}
					return;
				}
				g_badLines = 0;

				if (Settings::bSmoothMouth && Settings::fSmoothing > 0.0f) {
					state.smoothed = SmoothAnim(const_cast<LipSynchAnim*>(anim));
					if (!state.smoothed && g_smoothFailures++ < 5) {
						logger::warn(
							"couldn't smooth lip data for actor {:08X} (frames {}, targets {}) - left as is",
							a_actor->formID, anim->numFrames, anim->numPhonemeTargets);
					}
				}

				if (Settings::bFixVoiceTimer && g_voiceOffset < 0 && !g_voiceGaveUp && ++g_voiceTrialLines > 10) {
					g_voiceGaveUp = true;
					logger::warn("couldn't confirm the voice timer after 10 lines; only lip time is being fixed");
				}
			}

			if (!state.valid) {
				return;
			}

			// Real time scaled by the game's slow-motion multiplier (VATS, Jet...),
			// which slows the voice audio too.
			float timeMult = RE::BSTimer::QGlobalTimeMultiplier();
			if (!(timeMult > 0.0f && timeMult <= 10.0f)) {
				timeMult = 1.0f;
			}
			const float realDt = std::chrono::duration<float>(now - state.last).count() * timeMult;
			state.last = now;

			if (Settings::bFixLipTime) {
				const auto step = Correct(state.lip, Field<float>(high, g_lipTimeOffset.load()), realDt, Settings::fLipOffset);
				state.engineAdvance += step.engine;
				state.realAdvance += step.real;
			}

			if (Settings::bFixVoiceTimer) {
				if (const auto voiceOffset = g_voiceOffset.load(); voiceOffset >= 0) {
					Correct(state.voice, Field<float>(high, voiceOffset), realDt);
				} else if (!g_voiceGaveUp && lipOffset - kVoiceBeforeAnim >= 0) {
					TryVoiceOffset(state, high, lipOffset - kVoiceBeforeAnim, realDt);
				}
			}

			if (Settings::bFixActorVoiceTimer) {
				Correct(state.actorVoice, Field<float>(a_actor, kActor_VoiceTimer), realDt);
			}
		}

		// ------------------------------------------------------------------
		// Hooks
		// ------------------------------------------------------------------

		template <std::size_t ID>
		struct UpdateHook
		{
			static void thunk(RE::Actor* a_this, float a_delta)
			{
				func(a_this, a_delta);
				if (a_this && !g_disabled) {
					Process(a_this);
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <class Hook>
		void WriteVfunc(REL::ID a_vtable, std::string_view a_name)
		{
			REL::Relocation<std::uintptr_t> vtbl{ a_vtable };
			Hook::func = vtbl.write_vfunc(kUpdateVfunc, Hook::thunk);
			logger::info("hooked {}::Update", a_name);
		}
	}

	void Install()
	{
		WriteVfunc<UpdateHook<0>>(RE::VTABLE::Actor[0], "Actor"sv);
		WriteVfunc<UpdateHook<1>>(RE::VTABLE::PlayerCharacter[0], "PlayerCharacter"sv);
	}
}
