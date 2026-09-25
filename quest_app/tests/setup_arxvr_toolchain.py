"""Set up an isolated macOS arm64 Android toolchain using official distributions."""
from pathlib import Path
from urllib.request import Request,urlopen
import json,zipfile,tarfile,subprocess,os,hashlib
if input('Download the isolated Android toolchain and accept its SDK licenses? (y/n): ').strip().lower() != 'y':
    raise SystemExit('No changes made.')
base=Path(os.environ.get('ARXVR_TOOLCHAIN_ROOT', Path.home() / '.arxvr_toolchain'));base.mkdir(parents=True,exist_ok=True)
def fetch(url,path):
    if path.exists():return
    with urlopen(Request(url,headers={'User-Agent':'arxVR-build'}),timeout=180) as r,path.with_suffix('.part').open('wb') as f:
        while block:=r.read(1024*1024):f.write(block)
    path.with_suffix('.part').rename(path)
jdk=base/'jdk-17'
if not jdk.exists():
    req=Request('https://api.github.com/repos/adoptium/temurin17-binaries/releases/latest',headers={'User-Agent':'arxVR-build'})
    with urlopen(req,timeout=60) as r: release=json.load(r)
    asset=next(a for a in release['assets'] if a['name'].startswith('OpenJDK17U-jdk_aarch64_mac_hotspot_') and a['name'].endswith('.tar.gz'))
    pkg={'link':asset['browser_download_url'],'name':asset['name']}
    archive=base/'temurin17.tar.gz';fetch(pkg['link'],archive)
    sha=base/'temurin17.sha256';fetch(pkg['link']+'.sha256.txt',sha)
    pkg['checksum']=sha.read_text().split()[0]
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==pkg['checksum']
    with tarfile.open(archive) as t:
        prefix=t.getmembers()[0].name.split('/')[0];t.extractall(base,filter='data')
    (base/prefix).rename(jdk)
    (base/'jdk-source.json').write_text(json.dumps(pkg,indent=2))
java=jdk/'Contents/Home';sdk=base/'android-sdk';sdk.mkdir(exist_ok=True)
cmd=sdk/'cmdline-tools/12.0'
if not cmd.exists():
    archive=base/'commandlinetools-mac-11076708_latest.zip';fetch('https://dl.google.com/android/repository/commandlinetools-mac-11076708_latest.zip',archive)
    staging=base/'cmdline-extract';staging.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as z:z.extractall(staging)
    cmd.parent.mkdir(exist_ok=True);(staging/'cmdline-tools').rename(cmd)
    for p in (cmd/'bin').iterdir():p.chmod(0o755)
env=dict(os.environ,JAVA_HOME=str(java),ANDROID_HOME=str(sdk));env['PATH']=str(java/'bin')+':'+env['PATH']
manager=cmd/'bin/sdkmanager'
print('Installing isolated Android toolchain:',sdk,flush=True)
subprocess.run([str(manager),f'--sdk_root={sdk}','--licenses'],input='y\n'*100,text=True,env=env,check=True,stdout=subprocess.DEVNULL)
subprocess.run([str(manager),f'--sdk_root={sdk}','platform-tools','platforms;android-35','build-tools;35.0.0','ndk;27.2.12479018','cmake;3.22.1','build-tools;34.0.0'],env=env,check=True)
archive=base/'gradle-8.9-bin.zip';fetch('https://services.gradle.org/distributions/gradle-8.9-bin.zip',archive)
if not (base/'gradle-8.9').exists():
    with zipfile.ZipFile(archive) as z:z.extractall(base)
(base/'gradle-8.9/bin/gradle').chmod(0o755)
print('Toolchain ready',flush=True)
