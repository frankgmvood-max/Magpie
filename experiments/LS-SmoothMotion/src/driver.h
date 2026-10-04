// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include "settings.h"
namespace sm {
enum class Profile { Missing,Unsupported,Known };
Profile CheckProfile(const std::wstring&,std::string& sha);
std::wstring ActiveDriverPath();
bool InitializeDriver(const std::wstring&,const Settings&,std::function<void(const char*)>);
void StopDriver();
bool DriverFaulted();
std::string DriverStatus();
}
