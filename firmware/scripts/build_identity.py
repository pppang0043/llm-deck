"""PlatformIO: 실제 입력에서 빌드 ID 를 만들고 완성된 bin 의 manifest 를 남긴다."""
import hashlib
import json
import os
import sys
from pathlib import Path

Import("env")
project = Path(env.subst("$PROJECT_DIR"))
sys.path.insert(0, str(project / "scripts"))
from build_manifest import source_digest, protocol, image_digest, write_json, validate_upload, uploaded_images, run_staged_uploader

build = Path(env.subst("$BUILD_DIR"))
generated = build / "generated"
generated.mkdir(parents=True, exist_ok=True)
snapshot = source_digest(project)
context = {"source": snapshot, "flags": env.get("BUILD_FLAGS", []), "packages": []}
for package in ("framework-arduinoespressif32", "framework-arduinoespressif32-libs", "toolchain-xtensa-esp-elf"):
    directory = env.PioPlatform().get_package_dir(package)
    if directory and (Path(directory) / "package.json").exists():
        context["packages"].append(json.loads((Path(directory) / "package.json").read_text()).get("version"))
identity = hashlib.sha256(json.dumps(context, sort_keys=True).encode()).hexdigest()[:20]
header = generated / "build_identity.h"
content = '#pragma once\n#define LLMDECK_SOURCE_ID "' + identity + '"\n'
if not header.exists() or header.read_text() != content:
    header.write_text(content)
env.Append(CPPPATH=[str(generated)])
proto = protocol(project)


def record_manifest(source, target, env):
    binary = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin"))
    write_json(build / "build-manifest.json", {"proto": proto, "build": image_digest(binary.with_suffix(".elf"))[:20], "source_id": identity,
               "source_digest": snapshot, "firmware_sha256": image_digest(binary),
               "images": uploaded_images(binary, [(env.subst(str(offset)), env.subst(str(path)))
                           for offset, path in env.get("FLASH_EXTRA_IMAGES", [])], env.subst("$ESP32_APP_OFFSET"))})


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", record_manifest)


def verify_before_upload(source, target, env):
    if env.GetProjectOption("upload_protocol", "esptool") == "espota":
        return  # 이 도구의 보호 범위는 USB. OTA 는 별도 경로다.
    expected = {key: os.environ.get(name) for key, name in (
        ("build", "LLMDECK_EXPECTED_BUILD"), ("source_digest", "LLMDECK_EXPECTED_SOURCE"),
        ("firmware_sha256", "LLMDECK_EXPECTED_SHA"))}
    if not all(expected.values()):
        raise RuntimeError("USB 업로드는 버전 검증을 하는 ./flash.sh 로 실행하세요")
    staged = os.environ.get("LLMDECK_STAGED_MANIFEST")
    if not staged:
        raise RuntimeError("검증된 이미지 스냅샷이 없습니다. ./flash.sh 로 실행하세요")
    manifest = json.loads(Path(staged).read_text())
    expected["images"] = json.loads(os.environ.get("LLMDECK_EXPECTED_IMAGES", "[]"))
    validate_upload(project, manifest, Path(staged).parent / "firmware.bin", expected)
    run_staged_uploader(env, manifest, os.environ.get("LLMDECK_UPLOAD_PORT"))


env.AddPreAction("upload", verify_before_upload)
