#pragma once

namespace LipSync
{
	// Hooks Actor::Update / PlayerCharacter::Update so every talking actor's
	// lip time and voice timer advance by real elapsed time instead of the
	// engine's frame delta (which drifts from the audio at high or uneven FPS).
	void Install();
}
