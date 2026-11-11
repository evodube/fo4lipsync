#include "Settings.h"

namespace Settings
{
	namespace
	{
		std::string Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_str.find_last_not_of(" \t\r\n");
			return std::string(a_str.substr(first, last - first + 1));
		}

		std::string Lower(std::string a_str)
		{
			std::ranges::transform(a_str, a_str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_str;
		}

		bool ParseBool(const std::string& a_value, bool a_default)
		{
			const auto v = Lower(a_value);
			if (v == "1" || v == "true" || v == "on" || v == "yes") {
				return true;
			}
			if (v == "0" || v == "false" || v == "off" || v == "no") {
				return false;
			}
			return a_default;
		}

		float ParseFloat(const std::string& a_value, float a_default)
		{
			try {
				return std::stof(a_value);
			} catch (...) {
				return a_default;
			}
		}
	}

	void Load()
	{
		const std::filesystem::path path{ "Data/F4SE/Plugins/fo4lipsync.ini" };

		std::ifstream file(path);
		if (!file.is_open()) {
			logger::info("no ini at {}, using defaults", path.string());
			return;
		}

		std::string line;
		while (std::getline(file, line)) {
			const auto comment = line.find_first_of(";#");
			if (comment != std::string::npos) {
				line.erase(comment);
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;  // blank line or [section]
			}

			const auto key = Lower(Trim(std::string_view(line).substr(0, eq)));
			const auto value = Trim(std::string_view(line).substr(eq + 1));

			if (key == "benabled") {
				bEnabled = ParseBool(value, bEnabled);
			} else if (key == "bdebuglog") {
				bDebugLog = ParseBool(value, bDebugLog);
			} else if (key == "bfixliptime") {
				bFixLipTime = ParseBool(value, bFixLipTime);
			} else if (key == "bfixvoicetimer") {
				bFixVoiceTimer = ParseBool(value, bFixVoiceTimer);
			} else if (key == "bfixactorvoicetimer") {
				bFixActorVoiceTimer = ParseBool(value, bFixActorVoiceTimer);
			} else if (key == "fmaxstep") {
				fMaxStep = std::clamp(ParseFloat(value, fMaxStep), 0.02f, 2.0f);
			} else if (key == "flipoffset") {
				fLipOffset = std::clamp(ParseFloat(value, fLipOffset), -1.0f, 1.0f);
			} else if (key == "bsmoothmouth") {
				bSmoothMouth = ParseBool(value, bSmoothMouth);
			} else if (key == "fsmoothing") {
				fSmoothing = std::clamp(ParseFloat(value, fSmoothing), 0.0f, 1.0f);
			} else if (key == "fsmoothradius") {
				fSmoothRadius = std::clamp(ParseFloat(value, fSmoothRadius), 0.25f, 4.0f);
			}
		}

		logger::info(
			"settings: enabled={} debug={} lipTime={} voiceTimer={} actorVoiceTimer={} maxStep={:.3f} lipOffset={:.3f} smooth={} smoothing={:.2f} radius={:.2f}",
			bEnabled, bDebugLog, bFixLipTime, bFixVoiceTimer, bFixActorVoiceTimer, fMaxStep, fLipOffset,
			bSmoothMouth, fSmoothing, fSmoothRadius);
	}
}
