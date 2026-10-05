"""Exercise the actual scaling-mode JSON paths and validate presentation UI metadata."""
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)


def function(path, signature):
    source = (repo / path).read_text(encoding='utf-8-sig')
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


shader = (repo / 'src/Effects/DLSSFG/DLSS_FrameGeneration.hlsl').read_text()
expected = {
    'presentationPacing': (0, [0, 1]),
    'maximumFrameLatency': (2, [1, 2, 3]),
    'presentationBufferFrames': (1, [0, 1, 2]),
    'presentationGpuReady': (1, [0, 1]),
}
for name, (default, choices) in expected.items():
    metadata = next(block for block in shader.split('//!PARAMETER')
                    if re.search(rf'\bint {name};', block))
    assert '//!GROUP Frame Presentation' in metadata, name
    assert int(re.search(r'//!DEFAULT (\d+)', metadata).group(1)) == default, name
    assert list(map(int, re.findall(r'//!OPTION (\d+)', metadata))) == choices, name
    for locale in ['en-US', 'ru', 'zh-Hans']:
        root = ET.parse(repo / f'src/Magpie/Resources.language-{locale}.resw').getroot()
        values = {node.get('name'): node.findtext('value') for node in root.findall('data')}
        assert len(values) == len(root.findall('data')), locale
        prefix = 'EffectParam_DLSSFG_DLSS_FrameGeneration_'
        assert values[prefix + 'Group_Frame_Presentation'], locale
        for suffix in ['Label', 'Description'] + [f'Option_{value}' for value in choices]:
            assert values[prefix + name + '_' + suffix], (locale, name, suffix)

parts = [function('src/Magpie/JsonHelper.cpp', f'bool JsonHelper::{name}(')
         for name in ['ReadBool', 'ReadUInt', 'ReadFloat', 'ReadString']]
parts += [function('src/Magpie/ScalingModesService.cpp', signature) for signature in
          ['static void WriteScalingMode(', 'static bool LoadScalingMode(']]
fixture = r'''
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include "include/DlssPresentationSettings.h"
#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace Magpie {
struct StrHelper {
    static std::string UTF16ToUTF8(const std::wstring& value) { return {value.begin(),value.end()}; }
    static std::wstring UTF8ToUTF16(const char* value) { const std::string s(value); return {s.begin(),s.end()}; }
};
struct ScalingModeNames { static const auto& Trim(const std::wstring& value) { return value; } };
enum class ScalingType : uint32_t { Normal=0 };
struct EffectItem {
    std::wstring name;
    std::unordered_map<std::wstring,float> parameters;
    ScalingType scalingType=ScalingType::Normal;
    std::pair<float,float> scale={1,1};
    bool isRecoveryInvalid=false;
    std::string recoveryOriginal;
    bool HasScale() const { return scalingType!=ScalingType::Normal || scale!=std::pair<float,float>{1,1}; }
};
struct ScalingMode { std::wstring name; std::vector<EffectItem> effects; };
struct JsonHelper {
    static bool ReadBool(const rapidjson::GenericObject<true,rapidjson::Value>&,const char*,bool&,bool required=false) noexcept;
    static bool ReadUInt(const rapidjson::GenericObject<true,rapidjson::Value>&,const char*,uint32_t&,bool required=false) noexcept;
    static bool ReadFloat(const rapidjson::GenericObject<true,rapidjson::Value>&,const char*,float&,bool required=false) noexcept;
    static bool ReadString(const rapidjson::GenericObject<true,rapidjson::Value>&,const char*,std::wstring&,bool required=false) noexcept;
};
PRODUCTION
static ScalingMode RoundTrip(const ScalingMode& mode) {
    rapidjson::StringBuffer buffer; rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    WriteScalingMode(writer,mode);
    const rapidjson::Document document=[&] { rapidjson::Document d; d.Parse(buffer.GetString()); return d; }();
    assert(!document.HasParseError()); ScalingMode loaded;
    assert(LoadScalingMode(document.GetObject(),loaded,true));
    return loaded;
}
static DlssPresentationSettings Read(const EffectItem& effect) {
    return ReadDlssPresentationSettings([&](std::string_view name)->std::optional<float> {
        const std::string key(name); const auto it=effect.parameters.find(std::wstring(key.begin(),key.end()));
        return it==effect.parameters.end()?std::nullopt:std::optional<float>(it->second);
    });
}
}
int main() {
    using namespace Magpie;
    ScalingMode old; old.name=L"Existing FG"; old.effects.resize(1);
    old.effects[0].name=L"DLSSFG\\DLSS_FrameGeneration";
    old.effects[0].parameters={{L"multiplier",2},{L"nvidiaOpticalFlowResolution",50}};
    const auto migrated=RoundTrip(old);
    assert(migrated.effects[0].parameters==old.effects[0].parameters);
    const auto defaults=Read(migrated.effects[0]);
    assert(defaults.strictPacing && defaults.gpuReady && defaults.maximumFrameLatency==2 && defaults.bufferFrames==1);
    for(int latency=1;latency<=3;++latency) for(int reserve=0;reserve<=2;++reserve)
    for(int pacing=0;pacing<=1;++pacing) for(int ready=0;ready<=1;++ready) {
        auto mode=old; auto& values=mode.effects[0].parameters;
        values[L"maximumFrameLatency"]=float(latency); values[L"presentationBufferFrames"]=float(reserve);
        values[L"presentationPacing"]=float(pacing); values[L"presentationGpuReady"]=float(ready);
        // Export/import and a second application save keep all choices and old parameters.
        const auto loaded=RoundTrip(RoundTrip(mode));
        assert(loaded.effects[0].parameters==values);
        const auto chosen=Read(loaded.effects[0]);
        assert(chosen.maximumFrameLatency==uint32_t(latency) && chosen.bufferFrames==uint32_t(reserve));
        assert(chosen.strictPacing==(pacing==0) && chosen.gpuReady==(ready!=0));
    }
    std::cout<<"PASS: production scaling-mode JSON, 36 presentation combinations, old FG/OF choices preserved, shader defaults and localized controls checked\n";
}
'''
(output / 'presentation_settings.cpp').write_text(fixture.replace('PRODUCTION', '\n\n'.join(parts)), encoding='utf-8')
print('Validated all presentation choices and en-US/ru/zh-Hans resources; extracted actual scaling-mode persistence.')
