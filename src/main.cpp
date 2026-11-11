#include "LipSync.h"
#include "Settings.h"

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	const auto runtime = REL::Module::get().version();
	if (REL::Module::IsVR()) {
		logger::error("game runtime {} is Fallout 4 VR, which isn't supported; not hooking", runtime.string());
		return true;
	}
	logger::info("game runtime {} ({})", runtime.string(), REL::Module::IsNG() ? "next-gen" : "old-gen");

	Settings::Load();
	if (!Settings::bEnabled) {
		logger::info("disabled in ini");
		return true;
	}

	LipSync::Install();
	return true;
}
