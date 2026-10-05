#pragma once

namespace Magpie {

// Seeding the color history does not execute NVOF. The first actual execute
// after any reset must discard the driver's previous temporal hints.
class OpticalFlowHistory {
public:
	void Reset() noexcept { _resetPending = true; }
	bool DisableTemporalHints() const noexcept { return _resetPending; }
	void Executed() noexcept { _resetPending = false; }
private:
	bool _resetPending = true;
};

}
