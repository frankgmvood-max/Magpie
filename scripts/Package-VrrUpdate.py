"""Small update, keeps the user's working Ampere FG runtime untouched."""
from pathlib import Path
import hashlib, json, subprocess, zipfile
repo=Path(__file__).resolve().parents[1]
output=repo/'publish/x64'
files=[p for p in output.rglob('*') if p.is_file() and (p.name in ('Magpie.exe','Magpie.Core.dll','resources.pri','Magpie.RtxVideo.dll') or p.suffix=='.hlsl')]
assert (output/'Magpie.exe').is_file() and (output/'resources.pri').is_file()
manifest={'version':'0.6.9-vrr4','commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'baseRelease':'v0.6.9-experimental','baseSha256':'f4233fc34b26db6f9bcb5e8fb4a527266806a949b35f5d8feb3c0e53058dfead','fullNativeBackends':True,'hardwareVrrVerified':False,'nativeCrashReporting':'out-of-process','automaticNvapiVrrPolling':False,'noFocusLoss':{'scope':'all-scaling','defaultEnabled':True,'defaultMode':'compatibility','internalForeground':'real','cursorHook':False,'start':'first-output-frame','stop':'before-output-destruction','suspend':'unrelated-foreground-application'},'files':{p.relative_to(output).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
(repo/'dist').mkdir(exist_ok=True)
with zipfile.ZipFile(repo/'dist/Magpie-0.6.9-VRR4-update-x64.zip','w',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
    for p in files:archive.write(p,p.relative_to(output))
    archive.writestr('vrr-update-manifest.json',json.dumps(manifest,indent=2))
    archive.write(repo/'LICENSE','LICENSE-Magpie.txt')
    archive.write(repo/'src/Magpie.Core/third_party/minhook/LICENSE.txt','LICENSE-MinHook.txt')
    archive.write(repo/'docs/MAGPIE_VRR_RU.md','README-VRR-RU.txt')
print('Packaged',len(files),'application/resource/effect files; original vendor runtime files remain untouched.')
