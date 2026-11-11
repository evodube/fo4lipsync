#include "LipSync.h"

#include "Settings.h"

namespace LipSync
{
	namespace
	{
		using clock = std::chrono::steady_clock;

		// ------------------------------------------------------------------
		// Engine layouts (old-gen 1.10.163)
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

		// HighProcessData fields we touch (AIProcess::high)
		constexpr std::ptrdiff_t kHigh_VoiceTimer = 0x3F0;
		constexpr std::ptrdiff_t kHigh_LipSynchAnim = 0x4A0;
		constexpr std::ptrdiff_t kHigh_LipTime = 0x4A8;
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
			if (!PlausiblePointer(rows) || !PlausiblePointer(base) || rows[0] != base) {
				return false;
			}

			static thread_local std::vector<float> scratch;
			static const auto                      kernel = BuildKernel(Settings::fSmoothRadius);
			const float                            strength = Settings::fSmoothing;

			const auto stride = rows[1] - rows[0];
			if (stride == static_cast<std::ptrdiff_t>(frames)) {
				// one contiguous curve per phoneme target
				for (std::size_t t = 0; t < targets; ++t) {
					SmoothCurve(base + t * frames, frames, 1, kernel, strength, scratch);
				}
				return true;
			}
			if (stride == static_cast<std::ptrdiff_t>(targets)) {
				// one row of target weights per frame
				for (std::size_t t = 0; t < targets; ++t) {
					SmoothCurve(base + t, frames, targets, kernel, strength, scratch);
				}
				return true;
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

		struct ActorState
		{
			LipSynchAnim*     anim{ nullptr };
			clock::time_point last{};
			clock::time_point lineStart{};
			Track             lip;
			Track             voice;
			Track             actorVoice;
			double            engineAdvance{ 0.0 };  // what the engine tried to add to lip time this line
			double            realAdvance{ 0.0 };    // what we used instead
			bool              smoothed{ false };
		};

		std::mutex                                       g_lock;
		std::unordered_map<RE::Actor*, ActorState>       g_states;
		std::atomic<bool>                                g_disabled{ false };
		std::atomic<std::uint32_t>                       g_badSamples{ 0 };
		std::uint64_t                                    g_lines{ 0 };
		double                                           g_totalEngine{ 0.0 };
		double                                           g_totalReal{ 0.0 };
		std::uint64_t                                    g_smoothFailures{ 0 };

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

		void FinishLine(RE::Actor* a_actor, const ActorState& a_state)
		{
			if (a_state.realAdvance <= 0.0) {
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

			void* high = process->high;
			auto* anim = Field<LipSynchAnim*>(high, kHigh_LipSynchAnim);

			std::scoped_lock lock(g_lock);

			if (!anim) {
				if (const auto it = g_states.find(a_actor); it != g_states.end()) {
					FinishLine(a_actor, it->second);
					g_states.erase(it);
				}
				return;
			}

			if (!PlausibleAnim(anim)) {
				if (g_badSamples.fetch_add(1) + 1 >= 20) {
					g_disabled = true;
					logger::error(
						"lip data didn't look like 1.10.163's layout 20 times in a row - turning the fix off for this session");
				}
				return;
			}
			g_badSamples = 0;

			const auto now = clock::now();
			auto&      state = g_states[a_actor];

			if (state.anim != anim) {
				// new line started
				if (state.anim) {
					FinishLine(a_actor, state);
				}
				state = {};
				state.anim = anim;
				state.last = now;
				state.lineStart = now;

				if (Settings::bSmoothMouth && Settings::fSmoothing > 0.0f) {
					state.smoothed = SmoothAnim(anim);
					if (!state.smoothed && g_smoothFailures++ < 5) {
						logger::warn(
							"couldn't smooth lip data for actor {:08X} (frames {}, targets {}) - left as is",
							a_actor->formID, anim->numFrames, anim->numPhonemeTargets);
					}
				}
			}

			const float realDt = std::chrono::duration<float>(now - state.last).count();
			state.last = now;

			if (Settings::bFixLipTime) {
				const auto step = Correct(state.lip, Field<float>(high, kHigh_LipTime), realDt, Settings::fLipOffset);
				state.engineAdvance += step.engine;
				state.realAdvance += step.real;
			}
			if (Settings::bFixVoiceTimer) {
				Correct(state.voice, Field<float>(high, kHigh_VoiceTimer), realDt);
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
