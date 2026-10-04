#pragma once
#include "common.hpp"

namespace nvs30::nvpresent {
bool initialize();
void shutdown();
HMODULE module();
bool contains_address(const void* address);
bool wrapper_active(IDXGISwapChain* swapchain, void** wrapper = nullptr);
bool wrapper_active(IDXGISwapChain* swapchain, void** wrapper, std::size_t* offset);
bool enable_wrapper(IDXGISwapChain* swapchain);
bool initialized();
void note_present_trampoline(void* present, void* present1);
bool present_hook_on_path();
std::uint64_t cuda_intercept_count();
std::uint64_t graph_launch_count();
std::uint64_t retarget_count();
}
