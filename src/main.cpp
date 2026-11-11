#include "LipSync.h"
#include "Settings.h"

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	const auto runtime = REL::Module::get().version();
	logger::info("game runtime {}", runtime.string());

	if (REL::Module::IsNG()) {
		logger::error("this build targets old-gen Fallout 4 (1.10.163); not hooking on next-gen");
		return true;
	}

	Settings::Load();
	if (!Settings::bEnabled) {
		logger::info("disabled in ini");
		return true;
	}

	LipSync::Install();
	return true;
}
