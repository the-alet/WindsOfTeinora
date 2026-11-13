"""Fetch every tool, SDK and library the project needs into .local (SPEC.md, hard rules).

Usage:  python .utilities/bootstrap.py [--only name,name] [--no-msvc] [--venv]
Safe to re-run: finished parts are skipped (delete the folder under .local to refetch).
MSVC and the Windows SDK come from Microsoft's own Visual Studio manifest; running this
accepts the Visual Studio Build Tools licence, as the first-time setup did.
"""
import argparse, concurrent.futures as cf, hashlib, json, re, shutil, subprocess, sys, urllib.parse, urllib.request, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCAL = ROOT / ".local"
DL = LOCAL / "downloads"
UA = {"User-Agent": "taelia-bootstrap"}

PINS = {
    "cmake": "4.4.5",
    "ninja": "1.13.2",
    "dxc": "v1.9.2609",
    "vulkan": "vulkan-sdk-1.4.363.0",   # Vulkan-Headers and volk share SDK tags
    "vma": "v3.5.0",
    "d3d12ma": "v3.3.0",
    "imgui": "v1.93.0",
    "stb": "9f755b604db3492df2cbeb39bf84b81e3e3932e3",
    "agility": "1.620.2",
    "vs_channel": "https://aka.ms/vs/18/stable/channel",
    "streamline": "v2.14.16",  # NVIDIA Streamline (DLSS, Reflex); the user accepted its licence
}


def log(*a):
    print(*a, flush=True)


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
        return r.read()


def fetch(url, name, sha256=None):
    dest = DL / name
    DL.mkdir(parents=True, exist_ok=True)
    if dest.exists() and (sha256 is None or hashlib.sha256(dest.read_bytes()).hexdigest() == sha256.lower()):
        return dest
    part = dest.with_name(dest.name + ".part")
    for attempt in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=300) as r, open(part, "wb") as f:
                shutil.copyfileobj(r, f, 1 << 20)
            break
        except Exception as e:  # flaky CDN: retry
            if attempt == 2:
                raise
            log(f"  retry {name}: {e}")
    if sha256 and hashlib.sha256(part.read_bytes()).hexdigest() != sha256.lower():
        raise RuntimeError(f"sha256 mismatch for {name}")
    part.replace(dest)
    return dest


def unzip(archive, dest, strip=0, prefix=""):
    """Extract entries under `prefix`, dropping `strip` leading path parts."""
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = urllib.parse.unquote(info.filename)
            if not name.startswith(prefix) or name.endswith("/"):
                continue
            parts = Path(name[len(prefix):]).parts[strip:]
            if not parts:
                continue
            out = dest.joinpath(*parts)
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(z.read(info))


def done(path):
    return (path / ".done").exists()


def mark(path):
    (path / ".done").write_text("ok")


def gh_zip(name, repo, tag, folder):
    out = LOCAL / folder
    if done(out):
        return
    log(f"{name} {tag}")
    shutil.rmtree(out, ignore_errors=True)
    unzip(fetch(f"https://github.com/{repo}/archive/refs/tags/{tag}.zip", f"{folder}-{tag}.zip"), out, strip=1)
    mark(out)


def get_cmake():
    out = LOCAL / "cmake"
    if done(out):
        return
    v = PINS["cmake"]
    log(f"cmake {v}")
    shutil.rmtree(out, ignore_errors=True)
    unzip(fetch(f"https://github.com/Kitware/CMake/releases/download/v{v}/cmake-{v}-windows-x86_64.zip", f"cmake-{v}.zip"), out, strip=1)
    mark(out)


def get_ninja():
    out = LOCAL / "ninja"
    if done(out):
        return
    v = PINS["ninja"]
    log(f"ninja {v}")
    unzip(fetch(f"https://github.com/ninja-build/ninja/releases/download/v{v}/ninja-win.zip", f"ninja-{v}.zip"), out)
    mark(out)


def get_dxc():
    out = LOCAL / "dxc"
    if done(out):
        return
    tag = PINS["dxc"]
    log(f"dxc {tag}")
    rel = json.loads(get(f"https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases/tags/{tag}"))
    asset = next(a for a in rel["assets"] if re.fullmatch(r"dxc_[\d_]+\.zip", a["name"]))
    shutil.rmtree(out, ignore_errors=True)
    unzip(fetch(asset["browser_download_url"], asset["name"]), out)
    mark(out)


def get_stb():
    out = LOCAL / "stb"
    if done(out):
        return
    c = PINS["stb"]
    log(f"stb {c[:10]}")
    out.mkdir(parents=True, exist_ok=True)
    for f in ("stb_image.h", "stb_image_write.h", "stb_truetype.h", "LICENSE"):
        (out / f).write_bytes(get(f"https://raw.githubusercontent.com/nothings/stb/{c}/{f}"))
    mark(out)


def get_agility():
    out = LOCAL / "agility"
    if done(out):
        return
    v = PINS["agility"]
    log(f"D3D12 Agility SDK {v}")
    shutil.rmtree(out, ignore_errors=True)
    pkg = fetch(f"https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/{v}/microsoft.direct3d.d3d12.{v}.nupkg", f"agility-{v}.nupkg")
    unzip(pkg, out, prefix="build/native/")
    (out / "version.txt").write_text(v)
    mark(out)


# ---------------------------------------------------------------- MSVC + Windows SDK
def vtuple(s):
    return tuple(int(x) for x in s.split("."))


def get_msvc():
    out = LOCAL / "msvc"
    if done(out):
        return
    log("MSVC + Windows SDK: reading the Visual Studio manifest")
    channel = json.loads(get(PINS["vs_channel"]))
    vs_item = next(i for i in channel["channelItems"] if i["id"] == "Microsoft.VisualStudio.Manifests.VisualStudio")
    manifest = json.loads(get(vs_item["payloads"][0]["url"]))
    packages = {}
    for p in manifest["packages"]:
        packages.setdefault(p["id"].lower(), []).append(p)

    vers = [m.group(1) for k in packages if (m := re.fullmatch(r"microsoft\.vc\.([\d.]+)\.tools\.hostx64\.targetx64\.base", k))]
    msvc = max(vers, key=vtuple)
    sdks = [k.rsplit(".", 1)[1] for k in packages if re.fullmatch(r"microsoft\.visualstudio\.component\.windows1[01]sdk\.\d+", k)]
    sdk_build = max(sdks, key=int)
    log(f"  MSVC {msvc}, Windows SDK build {sdk_build}")
    licence = next((i.get("localizedResources", [{}])[0].get("license") for i in channel["channelItems"] if i.get("type") == "Product" and "BuildTools" in i["id"]), None)
    if licence:
        log(f"  Visual Studio Build Tools licence: {licence}")

    wanted = [f"microsoft.vc.{msvc}.{p}" for p in (
        "crt.headers.base", "crt.x64.desktop.base", "crt.x64.store.base",
        "tools.hostx64.targetx64.base", "tools.hostx64.targetx64.res.base")]
    jobs = []
    for pid in wanted:
        if pid not in packages:
            raise RuntimeError(f"missing package {pid}")
        p = next(p for p in packages[pid] if p.get("language") in (None, "en-US", "neutral"))
        for pl in p["payloads"]:
            jobs.append((pl["url"], pl["fileName"].replace("\\", "_"), pl.get("sha256")))
    log(f"  downloading {len(jobs)} MSVC payloads")
    with cf.ThreadPoolExecutor(8) as ex:
        vsix = list(ex.map(lambda j: fetch(*j), jobs))
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    for f in vsix:
        unzip(f, out, prefix="Contents/")
    for t in out.rglob("vctip.exe"):  # the compiler's telemetry helper: not needed to build
        t.unlink()

    sdk_comp = packages[f"microsoft.visualstudio.component.windows11sdk.{sdk_build}"] if f"microsoft.visualstudio.component.windows11sdk.{sdk_build}" in packages \
        else packages[f"microsoft.visualstudio.component.windows10sdk.{sdk_build}"]
    sdk_pkg = packages[next(iter(sdk_comp[0]["dependencies"])).lower()][0]
    msi_names = [
        "Windows SDK for Windows Store Apps Tools-x86_en-us.msi",
        "Windows SDK for Windows Store Apps Headers-x86_en-us.msi",
        "Windows SDK for Windows Store Apps Headers OnecoreUap-x86_en-us.msi",
        "Windows SDK OnecoreUap Headers x64-x86_en-us.msi",
        "Windows SDK for Windows Store Apps Libs-x86_en-us.msi",
        "Universal CRT Headers Libraries and Sources-x86_en-us.msi",
        "Windows SDK Desktop Headers x64-x86_en-us.msi",
        "Windows SDK Desktop Headers x86-x86_en-us.msi",
        "Windows SDK Desktop Headers arm64-x86_en-us.msi",  # the SDK spreads common headers over the per-arch MSIs
        "Windows SDK Desktop Headers arm-x86_en-us.msi",
        "Windows SDK OnecoreUap Headers x86-x86_en-us.msi",
        "Windows SDK OnecoreUap Headers arm64-x86_en-us.msi",
        "Windows SDK Desktop Libs x64-x86_en-us.msi",
        "Windows SDK Desktop Tools x64-x86_en-us.msi",
    ]
    by_name = {pl["fileName"].replace("/", "\\"): pl for pl in sdk_pkg["payloads"]}
    msis, cabs = [], set()
    for n in msi_names:
        pl = by_name.get(f"Installers\\{n}")
        if not pl:
            log(f"  (no {n})")
            continue
        f = fetch(pl["url"], n, pl.get("sha256"))
        msis.append(f)
        data = f.read_bytes()
        i = 0
        while (i := data.find(b".cab", i + 4)) >= 0:
            cabs.add(data[i - 32:i + 4].decode("ascii", "ignore"))
    cab_jobs = [(by_name[f"Installers\\{c}"]["url"], c, by_name[f"Installers\\{c}"].get("sha256")) for c in cabs if f"Installers\\{c}" in by_name]
    log(f"  downloading {len(msis)} SDK installers and {len(cab_jobs)} cabinets")
    with cf.ThreadPoolExecutor(8) as ex:
        list(ex.map(lambda j: fetch(*j), cab_jobs))
    for m in msis:  # administrative install = plain extraction, nothing is registered on the system
        subprocess.check_call(["msiexec.exe", "/a", str(m), "/quiet", "/qn", f"TARGETDIR={out}"])
        (out / m.name).unlink(missing_ok=True)
    if not (out / "Windows Kits/10/Include").exists():
        raise RuntimeError("Windows SDK extraction failed")
    mark(out)


def write_toolchain():
    out = LOCAL / "msvc"
    tools = sorted((out / "VC/Tools/MSVC").iterdir(), key=lambda p: vtuple(p.name))[-1]
    kits = out / "Windows Kits/10"
    sdkv = sorted((p.name for p in (kits / "Include").iterdir() if p.name.startswith("10.")), key=vtuple)[-1]
    inc = [tools / "include"] + [kits / "Include" / sdkv / d for d in ("ucrt", "shared", "um", "winrt", "cppwinrt")]
    lib = [tools / "lib/x64", kits / "Lib" / sdkv / "ucrt/x64", kits / "Lib" / sdkv / "um/x64"]
    sdkbin = kits / "bin" / sdkv / "x64"
    q = lambda p: str(p).replace("\\", "/")
    libpaths = " ".join(f'"/LIBPATH:{q(p)}"' for p in lib)
    text = f"""# Generated by .utilities/bootstrap.py: the MSVC and Windows SDK inside .local
set(TAELIA_MSVC_DIR "{q(tools)}")
set(TAELIA_WINSDK_DIR "{q(kits)}")
set(TAELIA_WINSDK_VERSION "{sdkv}")
set(CMAKE_C_COMPILER "{q(tools / 'bin/Hostx64/x64/cl.exe')}")
set(CMAKE_CXX_COMPILER "{q(tools / 'bin/Hostx64/x64/cl.exe')}")
set(CMAKE_LINKER "{q(tools / 'bin/Hostx64/x64/link.exe')}")
set(CMAKE_AR "{q(tools / 'bin/Hostx64/x64/lib.exe')}")
set(CMAKE_RC_COMPILER "{q(sdkbin / 'rc.exe')}")
set(CMAKE_MT "{q(sdkbin / 'mt.exe')}")
# Static CRT everywhere (the DLL CRT import libraries aren't fetched), compiler checks included.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(_taelia_inc {' '.join(f'"{q(p)}"' for p in inc)})
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${{_taelia_inc}})
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${{_taelia_inc}})
set(CMAKE_RC_STANDARD_INCLUDE_DIRECTORIES ${{_taelia_inc}})
set(CMAKE_EXE_LINKER_FLAGS_INIT [=[{libpaths}]=])
set(CMAKE_SHARED_LINKER_FLAGS_INIT [=[{libpaths}]=])
set(CMAKE_MODULE_LINKER_FLAGS_INIT [=[{libpaths}]=])
"""
    (LOCAL / "toolchain.cmake").write_text(text)
    log(f"toolchain.cmake: MSVC {tools.name}, SDK {sdkv}")


def make_venv():
    venv = LOCAL / "venv"
    if done(venv):
        return
    log("python venv (Pillow, numpy) for frame tools")
    subprocess.check_call([sys.executable, "-m", "venv", str(venv)])
    py = venv / "Scripts/python.exe"
    env_cache = str(LOCAL / "pip-cache")
    subprocess.check_call([str(py), "-m", "pip", "install", "--quiet", "--cache-dir", env_cache, "pillow", "numpy"])
    mark(venv)


def get_streamline():
    out = LOCAL / "streamline"
    if done(out):
        return
    tag = PINS["streamline"]
    log(f"NVIDIA Streamline {tag}")
    shutil.rmtree(out, ignore_errors=True)
    z = fetch(f"https://github.com/NVIDIA-RTX/Streamline/releases/download/{tag}/streamline-sdk-{tag}.zip", f"streamline-{tag}.zip")
    with zipfile.ZipFile(z) as zf:
        tops = {n.split("/")[0] for n in zf.namelist()}
    unzip(z, out, strip=1 if len(tops) == 1 else 0)
    mark(out)


def build_vvl():
    """Vulkan validation layers, built from source (the SDK installer would register itself system-wide).
    The game points VK_ADD_LAYER_PATH at .local/vulkan-layers when run with --gpu-debug."""
    out = LOCAL / "vulkan-layers"
    if done(out):
        return
    src = LOCAL / "src/vvl"
    log(f"Vulkan validation layers {PINS['vulkan']} (from source, takes a while)")
    if not (src / "CMakeLists.txt").exists():
        subprocess.check_call(["git", "clone", "--depth", "1", "-b", PINS["vulkan"],
                               "https://github.com/KhronosGroup/Vulkan-ValidationLayers.git", str(src)])
    cmake = str(LOCAL / "cmake/bin/cmake.exe")
    bld = LOCAL / "build-vvl"
    import os
    env = dict(os.environ)  # update_deps.py calls plain `cmake`: use ours
    env["PATH"] = f"{LOCAL / 'cmake/bin'};{LOCAL / 'ninja'};{env.get('PATH', '')}"
    subprocess.check_call([cmake, "-S", str(src), "-B", str(bld), "-G", "Ninja",
                           f"-DCMAKE_MAKE_PROGRAM={LOCAL / 'ninja/ninja.exe'}",
                           f"-DCMAKE_TOOLCHAIN_FILE={LOCAL / 'toolchain.cmake'}",
                           "-DCMAKE_BUILD_TYPE=Release", "-DUPDATE_DEPS=ON", "-DBUILD_TESTS=OFF", "-DBUILD_WERROR=OFF",
                           "-DUSE_MIMALLOC=OFF"], env=env)
    subprocess.check_call([cmake, "--build", str(bld), "--config", "Release"], env=env)
    subprocess.check_call([cmake, "--install", str(bld), "--prefix", str(out), "--config", "Release"], env=env)
    mark(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--no-msvc", action="store_true")
    ap.add_argument("--venv", action="store_true")
    a = ap.parse_args()
    steps = {
        "cmake": get_cmake, "ninja": get_ninja, "dxc": get_dxc, "stb": get_stb, "agility": get_agility,
        "vulkan": lambda: gh_zip("Vulkan-Headers", "KhronosGroup/Vulkan-Headers", PINS["vulkan"], "vulkan-headers"),
        "volk": lambda: gh_zip("volk", "zeux/volk", PINS["vulkan"], "volk"),
        "vma": lambda: gh_zip("VMA", "GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator", PINS["vma"], "vma"),
        "d3d12ma": lambda: gh_zip("D3D12MA", "GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator", PINS["d3d12ma"], "d3d12ma"),
        "imgui": lambda: gh_zip("Dear ImGui", "ocornut/imgui", PINS["imgui"], "imgui"),
        "streamline": get_streamline,
    }
    only = [s for s in a.only.split(",") if s]
    for name, fn in steps.items():
        if not only or name in only:
            fn()
    if not a.no_msvc and (not only or "msvc" in only):
        get_msvc()
    if (LOCAL / "msvc/.done").exists():
        write_toolchain()
    if a.venv or "venv" in only:
        make_venv()
    if "vvl" in only:
        build_vvl()
    log("bootstrap done")


if __name__ == "__main__":
    main()
