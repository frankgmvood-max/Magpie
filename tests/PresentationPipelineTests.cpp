#include "include/DlssPresentationSettings.h"
#include "PresentationBuffer.h"
#include "VrrPresentationClock.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>

using namespace Magpie;
using namespace std::chrono_literals;

static void Settings() {
	std::map<std::string, float> values;
	auto read = [&] { return ReadDlssPresentationSettings([&](std::string_view name) -> std::optional<float> {
		const auto it = values.find(std::string(name));
		return it == values.end() ? std::nullopt : std::optional<float>(it->second);
	}); };
	const auto defaults = read();
	assert(defaults.strictPacing && defaults.gpuReady && defaults.maximumFrameLatency == 2 && defaults.bufferFrames == 1);
	for (const float invalid : {-1.f, 0.5f, 1000.f, std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::quiet_NaN()}) {
		values = {{"maximumFrameLatency",invalid},{"presentationBufferFrames",invalid},
			{"presentationPacing",invalid},{"presentationGpuReady",invalid}};
		const auto result = read();
		assert(result.maximumFrameLatency==2 && result.bufferFrames==1 && result.strictPacing && result.gpuReady);
	}
	for (uint32_t latency=1;latency<=3;++latency) for(uint32_t reserve=0;reserve<=2;++reserve) {
		values = {{"maximumFrameLatency",float(latency)},{"presentationBufferFrames",float(reserve)},
			{"presentationPacing",1.f},{"presentationGpuReady",0.f}};
		const auto result=read();
		assert(result.maximumFrameLatency==latency && result.bufferFrames==reserve && !result.strictPacing && !result.gpuReady);
		assert(DlssSwapChainBufferCount(latency)>latency);
		for(uint32_t multiplier=2;multiplier<=4;++multiplier) {
			const auto slots=DlssPresentationSlotCount(multiplier,reserve);
			assert(slots>=multiplier && slots<=DLSS_PRESENTATION_MAX_SLOTS && slots>reserve);
		}
	}
}

static void Buffer() {
	using Time = PresentationBuffer::Time;
	const Time zero{};
	for(uint32_t mult=2;mult<=4;++mult) for(uint32_t reserve=0;reserve<=2;++reserve) {
		PresentationBuffer buffer;
		buffer.Configure(reserve);
		const auto filled=reserve*10ms;
		const auto first=buffer.WaitUntil(zero,mult+reserve,10ms,mult,1);
		if (!reserve) assert(!first);
		else {
			assert(first==zero+filled);
			assert(buffer.WaitUntil(zero+filled-1ns,mult+reserve,10ms,mult,1));
			assert(!buffer.WaitUntil(zero+filled,mult+reserve,10ms,mult,1));
			// Once primed, a queued single frame is not held for another warmup.
			assert(!buffer.WaitUntil(zero+filled+10ms,1,10ms,mult,1));
			// Empty queues and new resource generations must re-prime.
			assert(!buffer.WaitUntil(zero+50ms,0,10ms,mult,1));
			assert(buffer.WaitUntil(zero+60ms,mult+reserve,10ms,mult,1)==zero+60ms+filled);
			assert(buffer.WaitUntil(zero+70ms,mult+reserve,10ms,mult,2)==zero+70ms+filled);
		}
		buffer.Reset();
		// A history seed, capture pause, or runtime rejection produces one image.
		// It must drain after a finite wait, rather than deadlock a full pipeline.
		const auto timeout=buffer.WaitUntil(zero,1,10ms,mult,3);
		if(reserve) {
			assert(timeout && *timeout>zero && *timeout<=zero+100ms);
			assert(buffer.WaitUntil(*timeout-1ns,1,10ms,mult,3));
			assert(!buffer.WaitUntil(*timeout,1,10ms,mult,3));
		}
	}
	PresentationBuffer slow;
	slow.Configure(2);
	assert(slow.WaitUntil(zero,1,1s,4,0)==zero+100ms);
	assert(!slow.WaitUntil(zero+100ms,1,1s,4,0));
}

int main() {
	Settings(); Buffer();
	std::cout << "PASS: validated persisted choices, x2-x4 bounded slot capacity, latency1-3 allocations, finite priming, underflow and generation resets\n";
}
