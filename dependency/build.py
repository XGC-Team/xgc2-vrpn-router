#!/usr/bin/env python3
"""Build the private, pinned native profile; never install into system paths."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def run(*args, cwd=None):
    subprocess.run([str(x) for x in args], cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    work, prefix = args.work_dir.resolve(), args.prefix.resolve()
    if prefix == Path("/") or str(prefix).startswith(("/usr", "/lib", "/etc", "/opt")):
        parser.error("a private build/test prefix is required; no system installation")
    home = Path(__file__).resolve().parent
    lock = json.loads((home / "sources.lock.json").read_text())
    patch = home / lock["vrpn"]["patch"]
    digest = hashlib.sha256(patch.read_bytes()).hexdigest()
    if digest != lock["vrpn"]["patch_sha256"]:
        raise RuntimeError("native patch digest does not match sources.lock.json")
    work.mkdir(parents=True, exist_ok=True)
    flags = "-fsanitize=address,undefined -fno-omit-frame-pointer" if args.sanitize else ""
    for name in ("cares", "vrpn"):
        spec = lock[name]
        source = work / (name + "-src")
        marker = source / ".xgc2-native-build.json"
        stamp = {"commit": spec["commit"], "patch": digest if name == "vrpn" else None}
        if source.exists():
            if not marker.exists() or json.loads(marker.read_text()) != stamp:
                raise RuntimeError(f"{source}: refusing to reuse an unowned or differently patched source tree")
        else:
            run("git", "clone", "--no-checkout", "--filter=blob:none", spec["url"], source)
            run("git", "checkout", "--detach", spec["commit"], cwd=source)
            actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
            if actual != spec["commit"]:
                raise RuntimeError("source revision mismatch")
            if name == "vrpn":
                run("git", "apply", "--check", patch, cwd=source)
                run("git", "apply", patch, cwd=source)
            marker.write_text(json.dumps(stamp) + "\n")
        build = work / (name + "-build")
        options = ["-DCMAKE_BUILD_TYPE=Debug" if args.sanitize else "-DCMAKE_BUILD_TYPE=Release",
                   "-DCMAKE_INSTALL_PREFIX=" + str(prefix), "-DCMAKE_C_FLAGS=" + flags,
                   "-DCMAKE_CXX_FLAGS=" + flags, "-DCMAKE_EXE_LINKER_FLAGS=" + flags,
                   "-DCMAKE_CXX_STANDARD=20", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF"]
        if name == "cares":
            options += ["-DCARES_STATIC=ON", "-DCARES_SHARED=OFF", "-DCARES_BUILD_TESTS=OFF", "-DCARES_BUILD_TOOLS=OFF"]
        else:
            options += ["-DVRPN_INSTALL=ON", "-DVRPN_BUILD_CLIENTS=OFF", "-DVRPN_BUILD_SERVERS=OFF",
                        "-DVRPN_BUILD_CLIENT_LIBRARY=OFF", "-DVRPN_BUILD_SERVER_LIBRARY=ON",
                        "-DVRPN_BUILD_PYTHON=OFF", "-DVRPN_BUILD_PYTHON_HANDCODED_2X=OFF",
                        "-DVRPN_BUILD_PYTHON_HANDCODED_3X=OFF", "-DVRPN_BUILD_JAVA=OFF"]
        run("cmake", "-S", source, "-B", build, *options)
        run("cmake", "--build", build, "--parallel", "2")
        run("cmake", "--install", build)
    (prefix / "xgc2-vrpn-router-native.json").write_text(json.dumps(lock, indent=2) + "\n")


if __name__ == "__main__":
    main()
