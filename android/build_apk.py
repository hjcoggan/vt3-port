#!/usr/bin/env python3
"""Builds, signs and (optionally) installs a NativeActivity APK for the Retroid.

The APK holds one native library and no Java code. Steps: NDK clang builds lib<name>.so for
arm64-v8a; aapt2 links the manifest into base.apk; the library is added to the zip; zipalign and
apksigner finish it. The debug keystore and all outputs live under dump/android/, outside the repo.

Usage: build_apk.py SOURCE_DIR NAME [--so LIBRARY] [--install SERIAL]
  SOURCE_DIR holds the C sources (compiled with NDK clang), or is ignored when --so names a library
  that is already built. The manifest is android/<SOURCE_DIR's name>/AndroidManifest.xml.
"""
import os
import shutil
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DUMP = os.path.join(os.path.dirname(REPO), "dump", "android")  # Virtua Tennis 3/dump/android
NDK = "/opt/homebrew/share/android-ndk"
SDK = "/opt/homebrew/share/android-commandlinetools"
JAVA_HOME = "/opt/homebrew/opt/openjdk@21"
API = 29
TARGET_API = 35
ADB = os.path.join(SDK, "platform-tools", "adb")
KS_PASS = "android"
KS_ALIAS = "vt3port"


def tool(*parts):
    return os.path.join(*parts)


def run(cmd, cwd=None):
    env = dict(os.environ, JAVA_HOME=JAVA_HOME, PATH=os.path.join(JAVA_HOME, "bin") + ":" + os.environ["PATH"])
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("failed: " + " ".join(cmd[:3]))
    return r.stdout


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src_dir = os.path.abspath(argv[1])
    name = argv[2]
    install = argv[argv.index("--install") + 1] if "--install" in argv else None
    prebuilt = argv[argv.index("--so") + 1] if "--so" in argv else None
    manifest = os.path.join(REPO, "android", os.path.basename(src_dir), "AndroidManifest.xml")

    build = os.path.join(DUMP, "build", name)
    stage = os.path.join(build, "stage")
    shutil.rmtree(build, ignore_errors=True)
    os.makedirs(os.path.join(stage, "lib", "arm64-v8a"))

    so = os.path.join(stage, "lib", "arm64-v8a", "lib%s.so" % name)
    if prebuilt:
        shutil.copy(prebuilt, so)
    else:
        clang = tool(NDK, "toolchains", "llvm", "prebuilt", "darwin-x86_64", "bin",
                     "aarch64-linux-android%d-clang" % API)
        glue_dir = tool(NDK, "sources", "android", "native_app_glue")
        sources = [os.path.join(src_dir, f) for f in sorted(os.listdir(src_dir)) if f.endswith(".c")]
        run([clang, "-shared", "-fPIC", "-O2", "-Wall", "-DANDROID", "-I", glue_dir,
             "-Wl,-z,max-page-size=16384", "-o", so, os.path.join(glue_dir, "android_native_app_glue.c")] + sources
            + ["-llog", "-landroid", "-lvulkan", "-lm"])

    bt = tool(SDK, "build-tools", "36.1.0")
    jar = tool(SDK, "platforms", "android-%d" % TARGET_API, "android.jar")
    base = os.path.join(build, "base.apk")
    run([tool(bt, "aapt2"), "link", "-o", base, "-I", jar,
         "--manifest", manifest,
         "--min-sdk-version", str(API), "--target-sdk-version", str(TARGET_API),
         "--version-code", "1", "--version-name", "0.1"])
    run(["zip", "-q", "-r", base, "lib"], cwd=stage)
    aligned = os.path.join(build, "aligned.apk")
    run([tool(bt, "zipalign"), "-f", "-p", "4", base, aligned])

    ks = os.path.join(DUMP, "debug.keystore")
    if not os.path.exists(ks):
        os.makedirs(DUMP, exist_ok=True)
        run([os.path.join(JAVA_HOME, "bin", "keytool"), "-genkeypair", "-keystore", ks,
             "-storepass", KS_PASS, "-keypass", KS_PASS, "-alias", KS_ALIAS, "-keyalg", "RSA",
             "-keysize", "2048", "-validity", "36500", "-dname", "CN=vt3 port"])
    final = os.path.join(build, "%s.apk" % name)
    run([tool(bt, "apksigner"), "sign", "--ks", ks, "--ks-pass", "pass:" + KS_PASS,
         "--key-pass", "pass:" + KS_PASS, "--ks-key-alias", KS_ALIAS, "--out", final, aligned])
    print("built", final)
    if install:
        print(run([ADB, "-s", install, "install", "-r", final]).strip())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
