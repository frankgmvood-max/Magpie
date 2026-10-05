#include "include/NoFocusLossSettings.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
	using namespace Magpie;
	const NoFocusLossSettings defaults;
	assert(defaults.enabled);
	assert(defaults.mode == NoFocusLossMode::ClassicCompatibility);
	assert(SanitizeNoFocusLossMode(0) == NoFocusLossMode::ForegroundOnly);
	assert(SanitizeNoFocusLossMode(1) == NoFocusLossMode::Compatibility);
	assert(SanitizeNoFocusLossMode(2) == NoFocusLossMode::ClassicCompatibility);
	assert(SanitizeNoFocusLossMode(3) == NoFocusLossMode::ClassicCompatibility);
	assert(SanitizeNoFocusLossMode(std::numeric_limits<uint32_t>::max()) == NoFocusLossMode::ClassicCompatibility);
	for (const bool valid : {false, true}) {
		for (const bool source : {false, true}) {
			for (const bool output : {false, true}) {
				for (const auto mode : {NoFocusLossMode::ForegroundOnly, NoFocusLossMode::Compatibility, NoFocusLossMode::ClassicCompatibility}) {
					assert(ShouldSpoofForeground(mode, valid, source, output) ==
						(valid && (mode == NoFocusLossMode::ClassicCompatibility || source || output)));
				}
			}
		}
	}
	std::cout << "PASS: NoFocusLoss defaults, settings migration and foreground eligibility\n";
}
