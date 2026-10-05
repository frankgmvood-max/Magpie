"""Exercise the actual Renderer Reflex startup against counting NVAPI doubles."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
source = (repo / 'src/Magpie.Core/Renderer.cpp').read_text(encoding='utf-8-sig')
start = source.index('void Renderer::_InitializeReflex() noexcept')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
method = source[start:end]
# The startup must resolve user policy before touching NVAPI, with no remaining
# unconditional DLSS startup outside the extracted production method.
assert source.index('_frameSyncBackend = ResolveFrameSyncBackend(') < source.index('_InitializeReflex();')
assert source.count('CreateNvReflexDriver(') == 1

fixture = r'''
#include "include/FramePacingOptions.h"
#include "ReflexController.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace Magpie {
static unsigned nativeInitializations = 0, nativeConfigurations = 0;
struct Logger {
    static Logger& Get() { static Logger logger; return logger; }
    void Info(const char*) {}
};
struct Driver final : ReflexDriver {
    ReflexConfigurationResult Configure(ReflexSettings settings) noexcept override {
        ++nativeConfigurations; return {0,0,true,settings.lowLatency};
    }
    int Sleep() noexcept override { return 0; }
    int Marker(ReflexMarker,uint64_t) noexcept override { return 0; }
    int RegisterGenerationQueue(ID3D12CommandQueue*) noexcept override { return 0; }
    int Generation(ID3D12CommandQueue*,uint64_t,uint64_t,bool) noexcept override { return 0; }
    int FrontendRender(uint64_t,uint64_t,bool) noexcept override { return 0; }
    int Present(uint64_t,uint64_t,bool,bool) noexcept override { return 0; }
    void ReportFailure(const char*,int) noexcept override {}
};
std::unique_ptr<ReflexDriver> CreateNvReflexDriver(ID3D11Device*) noexcept {
    ++nativeInitializations; return std::make_unique<Driver>();
}
struct Presenter {
    bool dxgi = true; unsigned bindings = 0;
    bool UsesFrameLatencyWaitableObject() const { return dxgi; }
    void SetReflexController(ReflexController*) { ++bindings; }
};
struct Resources { ID3D11Device* GetD3DDevice() const { return nullptr; } };
struct Renderer {
    FrameSyncBackend _frameSyncBackend = FrameSyncBackend::None;
    std::unique_ptr<Presenter> _presenter = std::make_unique<Presenter>();
    Resources _frontendResources;
    ReflexController _reflex;
    void _InitializeReflex() noexcept;
};
/* STARTUP */
}
int main() {
    using namespace Magpie;
    unsigned cases = 0;
    for (bool enabled : {false,true})
    for (bool dlss : {false,true})
    for (bool xess : {false,true})
    for (bool frontEdge : {false,true})
    for (bool benchmark : {false,true})
    for (bool dxgi : {false,true})
    for (auto mode : {FrameSyncMode::FrontEdge,FrameSyncMode::Async,FrameSyncMode::Reflex}) {
        Renderer renderer;
        renderer._presenter->dxgi = dxgi;
        renderer._frameSyncBackend = ResolveFrameSyncBackend({enabled,0,mode},dlss,xess,frontEdge,benchmark);
        const unsigned beforeInit = nativeInitializations, beforeConfig = nativeConfigurations;
        renderer._InitializeReflex();
        const bool expected = enabled && !benchmark && !xess && mode == FrameSyncMode::Reflex && dxgi;
        if (nativeInitializations != beforeInit + expected || nativeConfigurations != beforeConfig + expected ||
            renderer._presenter->bindings != static_cast<unsigned>(expected) || renderer._reflex.CanResume() != expected)
            throw std::runtime_error("startup ignored user sync selection, benchmark, presenter or XeSS ownership");
        if (!expected) {
            renderer._reflex.RegisterGenerationQueue(nullptr);
            renderer._reflex.Generation(nullptr,1,1,true);
            renderer._reflex.Present(1,1,false,true);
            if (renderer._reflex.BeginCapture() || nativeConfigurations != beforeConfig)
                throw std::runtime_error("disabled Reflex must never enter NVAPI for DLSS generation");
        }
        ++cases;
    }
    std::cout << "PASS: " << cases << " actual Renderer startup cases; disabled/default DLSS FG makes no Reflex calls\n";
}
'''
(output / 'reflex-startup.cpp').write_text(fixture.replace('/* STARTUP */', method), encoding='utf-8')
