"""Check saved-model bindings, localization and frontend scheduling boundaries.

This is a source contract check, not a native UI or GPU acceptance test.
"""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
def read(path):
    return (repo / path).read_text(encoding="utf-8-sig")

x = "{http://schemas.microsoft.com/winfx/2006/xaml}"
local = "{using:Magpie}"
profile = ET.fromstring(read("src/Magpie/ProfilePage.xaml"))
group = next(n for n in profile.iter() if n.get(x + "Uid") == "FrameRefresh")
uids = [n.get(x + "Uid", "") for n in group.iter()]
assert uids.index("FrameRefresh_Content") < uids.index("FrameRefresh_Pacing") < uids.index("FrameRefresh_ContentRate")
advanced = next(n for n in group.iter() if n.get(x + "Uid") == "FrameRefresh_Advanced")
assert advanced.get("IsExpanded") == "False"
assert not any("Pacing" in n.get(x + "Uid", "") for n in advanced.iter())
assert next(n for n in group.iter() if n.get(x + "Uid") == "FrameRefresh_Pacing_Input").get("IsEnabled") == "{x:Bind ViewModel.IsContentPacingEnabled, Mode=OneWay}"
for uid, property_name in {
    "FrameRefresh_ContentRate": "ShowContentFrameRate",
    "FrameRefresh_CursorSupplement": "ShowCursorSupplement",
    "FrameRefresh_CursorRate": "ShowCursorSupplementRate",
    "FrameRefresh_IdleRate": "ShowIdleRedrawRate",
}.items():
    node = next(n for n in group.iter() if n.get(x + "Uid") == uid)
    assert node.get("Visibility") == f"{{x:Bind ViewModel.{property_name}, Mode=OneWay}}"
assert all(n.get("IsWrapEnabled") == "True" for n in group.iter(local + "SettingsCard"))
for n in group.iter("{using:Microsoft.UI.Xaml.Controls}NumberBox"):
    assert n.get("Minimum") == "15" and n.get("Maximum") == "360"
    assert n.get("SmallChange") == "1"
    assert n.get("NumberFormatter") == "{x:Bind local:App.IntegerFormatter, Mode=OneTime}"
home = read("src/Magpie/HomePage.xaml")
assert "MinFrameRateIndex" not in home and "DefaultFrameRefreshSummary" in home and "EditDefaultFrameRefresh" in home
assert "Profile_Cursor_Refresh" not in read("src/Magpie/ProfilePage.xaml")
assert "Profile_Performance_FrameRateLimiter" not in read("src/Magpie/ProfilePage.xaml")
idl = read("src/Magpie/ProfileViewModel.idl")
header = read("src/Magpie/ProfileViewModel.h")
vm = read("src/Magpie/ProfileViewModel.cpp")
names = set(re.findall(r"ViewModel\.([A-Za-z]+)", ET.tostring(group, encoding="unicode")))
for name in names:
    assert name in idl and name in header and "ProfileViewModel::" + name + "(" in vm, name
    if name != "ResetFrameRefresh":
        assert f'RaisePropertyChanged(L"{name}")' in vm, name
for lang in ("en-US", "zh-Hans", "zh-Hant"):
    root = ET.fromstring(read(f"src/Magpie/Resources.language-{lang}.resw"))
    resources = {n.get("name"): n.findtext("value") for n in root.findall("data")}
    assert len(resources) == len(root.findall("data")), lang
    for n in group.iter():
        uid = n.get(x + "Uid", "")
        if not uid:
            continue
        if n.tag in (local + "SettingsCard", local + "SettingsExpander", local + "SettingsGroup"):
            assert resources[uid + ".Header"] and resources[uid + ".Description"], (lang, uid)
        elif n.tag.endswith(("NumberBox", "ComboBox")):
            assert resources[uid + "_Input.[using:Windows.UI.Xaml.Automation]AutomationProperties.Name" if not uid.endswith("_Input") else uid + ".[using:Windows.UI.Xaml.Automation]AutomationProperties.Name"], (lang, uid)
        else:
            assert resources[uid + ".Content"], (lang, uid)
    for name in ("Home_FrameRefresh_Summary", "Home_FrameRefresh_Edit.Content", "FrameRefresh_RestartNotice", "FrameRefresh_LegacyNotice"):
        assert resources[name], (lang, name)
renderer = read("src/Magpie.Core/Renderer.cpp")
# The VRR output clock owns the final deadline. FrontEdge must not introduce
# another deadline in the same path; non-VRR content keeps its old gate.
assert re.search(
    r"const bool paced\s*=\s*!ScalingWindow::Get\(\)\.Options\(\)\.isVRREnabled\s*&&\s*!stableBaseOnly\s*&&",
    renderer,
)
assert "else if (!stableBaseOnly) {\n\t\t_frontEdgeClock.Reset();" in renderer
assert "if (submitted && contentFrame && ActiveFrameSyncBackend() == FrameSyncBackend::FrontEdge" in renderer
assert "_frontendPresentedBaseValid && overlayPending &&" in renderer
window = read("src/Magpie.Core/ScalingWindow.cpp")
service = read("src/Magpie/ScalingService.cpp")
assert "ApplyFrameRefreshSettings(_options, frameRefresh)" in window
assert "ApplyFrameRefreshSettings(options, profile.frameRefresh)" in service
settings = read("src/Magpie/AppSettings.cpp")
assert "_LoadProfile(scaleProfilesArray[0].GetObj(), _defaultProfile, true, legacyParameterFocusSwitching, legacyFrameSync, legacyIdle, migrateLegacyRefresh)" in settings
assert "_LoadProfile(scaleProfilesArray[i].GetObj(), rule, false, legacyParameterFocusSwitching, legacyFrameSync, legacyIdle, migrateLegacyRefresh)" in settings
assert "MergeFrameRefreshSettings(mergedFrameRefresh, request.previousFrameRefresh, request.frameRefresh)" in service
assert "FindProfileByIdentity" in service and "scalingRunId" in service
print("Unified refresh UI/model/localization/frontend contracts passed (not native UI/GPU acceptance).")
