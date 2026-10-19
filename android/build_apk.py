#!/usr/bin/env python3
"""Builds, signs and (optionally) installs a NativeActivity APK for the Retroid.

The APK holds the native library, plus, when android/<SOURCE_DIR>/ has them, Java classes from java/
(javac + d8) and a second JNI library from jni/ (with the vendored unshield). Steps: NDK clang builds lib<name>.so for
arm64-v8a; aapt2 links the manifest into base.apk; the library is added to the zip; zipalign and
apksigner finish it. The debug keystore and all outputs live under dump/android/, outside the repo.

Usage: build_apk.py SOURCE_DIR NAME [--so LIBRARY] [--asset FILE]... [--install SERIAL]
  SOURCE_DIR holds the C sources (compiled with NDK clang), or is ignored when --so names a library
  that is already built. The manifest is android/<SOURCE_DIR's name>/AndroidManifest.xml.
  --asset FILE   bundle FILE as assets/<its name> (the game takes shaders.pack this way)
"""
import os
import platform
import shutil
import subprocess
import sys
import zipfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# Outputs and the signing key: ../dump/android in the maintainer's tree, else build/android.
DUMP = os.path.join(os.path.dirname(REPO), "dump", "android")
if not os.path.isdir(DUMP):
    DUMP = os.path.join(REPO, "build", "android")
SDK = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or "/opt/homebrew/share/android-commandlinetools"
NDK = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT") or "/opt/homebrew/share/android-ndk"
JAVA_HOME = os.environ.get("JAVA_HOME") or "/opt/homebrew/opt/openjdk@21"
WIN = os.name == "nt"
HOST = {"Darwin": "darwin-x86_64", "Linux": "linux-x86_64", "Windows": "windows-x86_64"}[platform.system()]
API = 29
TARGET_API = 35
ADB = os.path.join(SDK, "platform-tools", "adb")
KS_PASS = "android"
KS_ALIAS = "vt3port"


def tool(*parts):
    """A tool path, with the suffix Windows needs (.exe binaries, .bat for d8 and apksigner)."""
    p = os.path.join(*parts)
    if WIN and not os.path.splitext(p)[1]:
        p += ".bat" if os.path.basename(p) in ("d8", "apksigner") else ".exe"
    return p


def ndk_tool(name):
    return tool(NDK, "toolchains", "llvm", "prebuilt", HOST, "bin", name)


def ndk_clang():
    c = ndk_tool("aarch64-linux-android%d-clang" % API)
    return c[:-4] + ".cmd" if WIN else c


def run(cmd, cwd=None):
    env = dict(os.environ, JAVA_HOME=JAVA_HOME, PATH=os.path.join(JAVA_HOME, "bin") + os.pathsep + os.environ["PATH"])
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("failed: " + " ".join(cmd[:3]))
    return r.stdout


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        sys.exit(2)
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
        clang = ndk_clang()
        glue_dir = tool(NDK, "sources", "android", "native_app_glue")
        sources = [os.path.join(src_dir, f) for f in sorted(os.listdir(src_dir)) if f.endswith(".c")]
        run([clang, "-shared", "-fPIC", "-O2", "-Wall", "-DANDROID", "-I", glue_dir,
             "-Wl,-z,max-page-size=16384", "-o", so, os.path.join(glue_dir, "android_native_app_glue.c")] + sources
            + ["-llog", "-landroid", "-lvulkan", "-lm"])

    bt = tool(SDK, "build-tools", "36.1.0")
    jar = tool(SDK, "platforms", "android-%d" % TARGET_API, "android.jar")
    app_dir = os.path.dirname(manifest)
    clang = ndk_clang()
    jni_dir = os.path.join(app_dir, "jni")
    if os.path.isdir(jni_dir):
        us = os.path.join(REPO, "android", "third_party", "unshield")
        us_src = [os.path.join(us, "lib", f) for f in sorted(os.listdir(os.path.join(us, "lib"))) if f.endswith(".c")]
        run([clang, "-shared", "-fPIC", "-O2", "-I", us, "-I", os.path.join(us, "lib"),
             "-Wl,-z,max-page-size=16384", "-o", os.path.join(stage, "lib", "arm64-v8a", "libvt3install.so")]
            + [os.path.join(jni_dir, f) for f in sorted(os.listdir(jni_dir)) if f.endswith(".c")]
            + us_src + [os.path.join(us, "lib", "md5", "md5c.c"), "-lz"])
    java_dir = os.path.join(app_dir, "java")
    if os.path.isdir(java_dir):
        classes = os.path.join(build, "classes")
        javas = [os.path.join(d, f) for d, _, fs in os.walk(java_dir) for f in fs if f.endswith(".java")]
        run([tool(JAVA_HOME, "bin", "javac"), "--release", "11", "-classpath", jar, "-d", classes] + javas)
        class_files = [os.path.join(d, f) for d, _, fs in os.walk(classes) for f in fs if f.endswith(".class")]
        run([tool(bt, "d8"), "--min-api", str(API), "--lib", jar, "--output", stage] + class_files)
    assets = [argv[i + 1] for i, a in enumerate(argv) if a == "--asset"]
    for a in assets:
        os.makedirs(os.path.join(stage, "assets"), exist_ok=True)
        shutil.copy(a, os.path.join(stage, "assets", os.path.basename(a)))
    base = os.path.join(build, "base.apk")
    res_dir = os.path.join(os.path.dirname(manifest), "res")
    link_inputs = []
    if os.path.isdir(res_dir):
        compiled = os.path.join(build, "compiled_res.zip")
        run([tool(bt, "aapt2"), "compile", "--dir", res_dir, "-o", compiled])
        link_inputs = [compiled]
    run([tool(bt, "aapt2"), "link", "-o", base, "-I", jar] + link_inputs +
        ["--manifest", manifest,
         "--min-sdk-version", str(API), "--target-sdk-version", str(TARGET_API)])
    with zipfile.ZipFile(base, "a", zipfile.ZIP_DEFLATED) as z:
        for d, _, fs in os.walk(stage):
            for f in fs:
                path = os.path.join(d, f)
                z.write(path, os.path.relpath(path, stage).replace(os.sep, "/"))
    aligned = os.path.join(build, "aligned.apk")
    run([tool(bt, "zipalign"), "-f", "-p", "4", base, aligned])

    ks = os.path.join(DUMP, "debug.keystore")
    if not os.path.exists(ks):
        os.makedirs(DUMP, exist_ok=True)
        run([tool(JAVA_HOME, "bin", "keytool"), "-genkeypair", "-keystore", ks,
             "-storepass", KS_PASS, "-keypass", KS_PASS, "-alias", KS_ALIAS, "-keyalg", "RSA",
             "-keysize", "2048", "-validity", "36500", "-dname", "CN=vt3 port"])
    final = os.path.join(build, "%s.apk" % name)
    run([tool(bt, "apksigner"), "sign", "--ks", ks, "--ks-pass", "pass:" + KS_PASS,
         "--key-pass", "pass:" + KS_PASS, "--ks-key-alias", KS_ALIAS, "--out", final, aligned])
    print("built", final)
    if install:
        print(run([ADB, "-s", install, "install", "-r", final]).strip())
    return final


if __name__ == "__main__":
    main(sys.argv)
