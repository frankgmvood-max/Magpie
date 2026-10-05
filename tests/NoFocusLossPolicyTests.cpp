#include "include/NoFocusLossSettings.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
	using namespace Magpie;
	const NoFocusLossSettings defaults;
	assert(defaults.enabled);
	assert(defaults.mode == NoFocusLossMode::Compatibility);
	assert(SanitizeNoFocusLossMode(0) == NoFocusLossMode::ForegroundOnly);
	assert(SanitizeNoFocusLossMode(1) == NoFocusLossMode::Compatibility);
	assert(SanitizeNoFocusLossMode(2) == NoFocusLossMode::Compatibility);
	assert(SanitizeNoFocusLossMode(std::numeric_limits<uint32_t>::max()) == NoFocusLossMode::Compatibility);
	for (const bool valid : {false, true}) {
		for (const bool source : {false, true}) {
			for (const bool output : {false, true}) {
				assert(ShouldSpoofForeground(valid, source, output) == (valid && (source || output)));
			}
		}
	}
	std::cout << "PASS: NoFocusLoss defaults, settings migration and foreground eligibility\n";
}
