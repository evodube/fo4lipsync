#pragma once

namespace Settings
{
	// [General]
	inline bool bEnabled = true;
	inline bool bDebugLog = false;

	// [Sync]
	inline bool  bFixLipTime = true;           // mouth animation position follows real time
	inline bool  bFixVoiceTimer = true;        // when the line counts as finished (stops talk-over / cut-off lines)
	inline bool  bFixActorVoiceTimer = false;  // secondary voice timer on the actor itself (experimental)
	inline float fMaxStep = 0.25f;             // real-time gaps longer than this while the engine is paused are dropped
	inline float fLipOffset = 0.0f;            // seconds; positive = mouth leads the audio, negative = mouth lags

	// [Smoothing]
	inline bool  bSmoothMouth = true;    // soften the frame-to-frame steps in the lip data
	inline float fSmoothing = 0.6f;      // 0 = vanilla shapes, 1 = fully smoothed
	inline float fSmoothRadius = 1.25f;  // blur width in lip-file frames

	void Load();
}
