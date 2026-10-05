"""Pinned SDK inputs for the full 0.6.9 x64 build, outside the source tree.

No runtime is taken from an unverified mirror. The release archive and NVOF
headers are hashed; SDK Git revisions and NVAPI/RTX SDK hashes are pinned.
"""
from pathlib import Path
import hashlib, shutil, subprocess, urllib.request, zipfile, json

repo = Path(__file__).resolve().parents[1]
deps = repo.parent / 'dependencies-vrr'
deps.mkdir(exist_ok=True)

def download(url, target, digest=None):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and (digest is None or hashlib.file_digest(target.open('rb'), 'sha256').hexdigest() == digest):
        return
    partial = target.with_suffix(target.suffix + '.partial')
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent':'Magpie-VRR-build'}), timeout=120) as response, partial.open('wb') as output:
            shutil.copyfileobj(response, output, 1024*1024)
        if digest and hashlib.file_digest(partial.open('rb'), 'sha256').hexdigest() != digest:
            raise RuntimeError('SHA256 mismatch: ' + target.name)
        partial.replace(target)
    finally:
        partial.unlink(missing_ok=True)

sdk_revisions = {
    'dlss': ('NVIDIA/DLSS', '374959484e79a640feaba44c93ac8cfb0a03f5b5', ['include', 'lib/Windows_x86_64/x64', 'LICENSE.txt']),
    'fsr2': ('optiscaler/FidelityFX-FSR2-DX11', 'f2e3f86390746eb3f0bd1b28e91ea3cbc790ee76', ['src/ffx-fsr2-api', 'LICENSE.txt']),
    'fsr3': ('GPUOpen-LibrariesAndSDKs/FidelityFX-SDK', '60f4ea81909200d8542eca14dccb2628b763a9a3', ['Kits/FidelityFX', 'docs/license.md', '3rdpartynotice.md']),
    'xess': ('intel/xess', 'de0fb9c1c510661c571164e1418ceca8101dab69', ['inc', 'lib', 'LICENSE.txt', 'third-party-programs.txt']),
    'vfx': ('NVIDIA-Maxine/Maxine-VFX-SDK', 'f12bd18929e9065cff4f24cb93ac5f4202dc1c4a', ['nvvfx', 'LICENSE']),
}
for name, (project, revision, paths) in sdk_revisions.items():
    directory = deps / name
    if not (directory / '.git').exists():
        subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout', 'https://github.com/'+project+'.git', str(directory)], check=True)
    subprocess.run(['git', '-C', str(directory), 'sparse-checkout', 'set', '--no-cone', *paths], check=True)
    subprocess.run(['git', '-C', str(directory), 'checkout', '--detach', revision], check=True)
    assert subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip() == revision

upstream_zip = deps / 'Magpie-0.6.9-original.zip'
download('https://github.com/SAOG0721/Magpie/releases/download/v0.6.9-experimental/Magpie-Experimental-x64.zip', upstream_zip,
         'f4233fc34b26db6f9bcb5e8fb4a527266806a949b35f5d8feb3c0e53058dfead')
with zipfile.ZipFile(upstream_zip) as archive:
    archive.extractall(deps / 'upstream')
runtime = deps / 'upstream/Magpie-Experimental-x64'
for source_name, target in (
    ('nvngx_dlss.dll', deps/'dlss/lib/Windows_x86_64/rel/nvngx_dlss.dll'),
    ('nvngx_dlssg.dll', deps/'dlss/lib/Windows_x86_64/rel/nvngx_dlssg.dll'),
    ('libxess.dll', deps/'xess/bin/libxess.dll'),
    ('libxess_fg.dll', deps/'xess/bin/libxess_fg.dll'),
    ('libxell.dll', deps/'xess/bin/libxell.dll')):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime / source_name, target)

nvof = deps / 'nvof'
# MIT-licensed v5 API headers already used in the working LS addon, byte pinned.
base = 'https://raw.githubusercontent.com/frankgmvood-max/Magpie/e6d80b7b56c4c3cc9a5444853c72a5b61aa7e668/experiments/LS-DLSSFG/third_party/nvof/'
for name, digest in {
    'nvOpticalFlowCommon.h': 'cdbdbf0797fc02d2de1051520d4164bb1e36b7722009f46cb16216675319436a',
    'nvOpticalFlowD3D11.h': 'c33a4da15d849e26270bd9f3aff9fe06e0b60eedfbf5bc455b4e326dd1796003',
}.items(): download(base + name, nvof / name, digest)

subprocess.run(['pwsh','-NoProfile','-File',str(repo/'scripts/Fetch-NvapiHeaders.ps1'),'-OutputDirectory',str(deps/'nvapi')],check=True)
subprocess.run(['pwsh','-NoProfile','-File',str(repo/'scripts/Fetch-RtxVideoSdk.ps1'),'-Destination',str(deps/'rtx-video')],check=True)

from xml.sax.saxutils import escape
values = {name:'true' for name in ('EnableDLSSSR','EnableDLSSFrameGeneration','EnableDLSSNR','EnableNvidiaOpticalFlow',
    'EnableFSR2ZeroMV','EnableFSR3ZeroMV','EnableAmdOpticalFlow','EnableXeSSZeroMV','EnableXeSSFrameGeneration','EnableRTXVideoDenoise')}
values.update({'DLSSSdkDir':deps/'dlss','NvapiSdkDir':deps/'nvapi','NvidiaOpticalFlowSdkDir':nvof,
    'DLSSNRRuntimeDir':runtime,'FSR2SdkDir':deps/'fsr2','FSR2RuntimeDir':runtime,'FSR3SdkDir':deps/'fsr3',
    'XeSSSdkDir':deps/'xess','VFXSdkDir':deps/'vfx','VFXRuntimeDir':runtime,'VFXLicenseDir':runtime/'licenses',
    'RtxVideoSdkDir':deps/'rtx-video'})
(repo/'src/BuildOptions.props.user').write_text('<Project><PropertyGroup>\n'+''.join(f'<{k}>{escape(str(v))}</{k}>\n' for k,v in values.items())+'</PropertyGroup></Project>',encoding='utf-8')
(deps/'dependency-revisions.json').write_text(json.dumps({k:{'repo':v[0],'commit':v[1]} for k,v in sdk_revisions.items()},indent=2))
print('Pinned full-feature dependencies restored.')
