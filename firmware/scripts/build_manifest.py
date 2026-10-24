"""빌드 입력과 실제 업로드 이미지의 식별자. 비밀 파일은 해시만 사용한다."""
import hashlib
import json
import re
import tempfile
from contextlib import contextmanager
from pathlib import Path


def source_digest(firmware):
    firmware = Path(firmware)
    files = [firmware / "platformio.ini"]
    for directory in ("src", "include", "scripts"):
        files += [p for p in (firmware / directory).rglob("*") if p.is_file()
                  and "__pycache__" not in p.parts and p.suffix in {".h", ".c", ".cpp", ".py", ".csv", ".ld", ".S"}]
    files += list(firmware.glob("*.csv"))
    files += list((firmware.parent / "speaker/include").glob("*.h"))
    digest = hashlib.sha256()
    for path in sorted(files):
        name = path.relative_to(firmware.parent).as_posix().encode()
        content = path.read_bytes()
        digest.update(len(name).to_bytes(4, "little") + name)
        digest.update(len(content).to_bytes(8, "little") + content)
    return digest.hexdigest()


def protocol(firmware):
    match = re.search(r"^#define FW_PROTO (\d+)\s*$", (Path(firmware) / "src/link.h").read_text(), re.M)
    if not match:
        raise ValueError("FW_PROTO 를 읽지 못했습니다")
    return int(match[1])


def companion_protocol(firmware):
    match = re.search(r"^PROTO = (\d+)\s*$", (Path(firmware).parent / "companion/version.py").read_text(), re.M)
    if not match:
        raise ValueError("도우미 PROTO 를 읽지 못했습니다")
    return int(match[1])


def image_digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def uploaded_images(binary, extras, app_offset):
    images = [{"offset": int(str(offset), 0), "path": str(Path(path).resolve()),
               "sha256": image_digest(path), "role": "extra"} for offset, path in extras]
    images.append({"offset": int(str(app_offset), 0), "path": str(Path(binary).resolve()),
                   "sha256": image_digest(binary), "role": "app"})
    return images


def upload_spec(images):
    return [{key: image[key] for key in ("offset", "sha256", "role")} for image in images]


def validate_artifact(firmware, manifest, binary):
    if manifest.get("source_digest") != source_digest(firmware):
        raise ValueError("빌드 뒤 소스가 변경됐습니다. 다시 빌드하세요")
    if manifest.get("proto") != protocol(firmware):
        raise ValueError("빌드 결과와 소스의 프로토콜이 다릅니다")
    if manifest.get("firmware_sha256") != image_digest(binary):
        raise ValueError("빌드 이미지가 manifest 와 다릅니다. 다시 빌드하세요")
    if manifest.get("build") != image_digest(Path(binary).with_suffix(".elf"))[:20]:
        raise ValueError("실행 파일과 빌드 식별자가 다릅니다")
    if not re.fullmatch(r"[0-9a-f]{20}", manifest.get("build", "")):
        raise ValueError("빌드 식별자가 올바르지 않습니다")
    images = manifest.get("images")
    if not isinstance(images, list) or not images:
        raise ValueError("전체 업로드 이미지 목록이 없습니다. 다시 빌드하세요")
    offsets = set()
    apps = []
    for image in images:
        if (type(image.get("offset")) is not int or not 0 <= image["offset"] <= 0xffffffff
                or image["offset"] in offsets or image.get("role") not in {"app", "extra"}):
            raise ValueError("업로드 이미지 주소/역할이 올바르지 않습니다")
        offsets.add(image["offset"])
        if image.get("sha256") != image_digest(image["path"]):
            raise ValueError("업로드 이미지가 manifest 와 다릅니다")
        if image["role"] == "app":
            apps.append(image)
    if (len(apps) != 1 or Path(apps[0]["path"]).resolve() != Path(binary).resolve()
            or apps[0]["sha256"] != manifest["firmware_sha256"]):
        raise ValueError("앱 이미지와 업로드 목록이 다릅니다")


def validate_plan(manifest, companion_proto, current, previous, root, *, allow_worktree_switch=False):
    target = manifest["proto"]
    if target != companion_proto:
        raise ValueError(f"펌웨어 proto {target} / 도우미 proto {companion_proto}: 버전이 다릅니다")
    floor = max(current["fw_proto"], previous.get("proto", 0))
    if target < floor:
        raise ValueError(f"프로토콜 하향 업로드 차단: 실행/배포 기록 proto {floor} → 대상 {target}")
    if (previous.get("root") and previous["root"] != str(Path(root).resolve())
            and previous.get("build") != manifest["build"] and not allow_worktree_switch):
        raise ValueError("다른 worktree 에서 다른 빌드를 덮어쓰려 합니다. 내용을 확인한 뒤 --allow-worktree-switch 를 명시하세요")


def validate_running(manifest, current, device):
    if current.get("id") != device or current.get("fw_proto") != manifest["proto"] or current.get("build") != manifest["build"]:
        raise ValueError("실행 중인 기기/프로토콜/빌드가 업로드 대상과 일치하지 않습니다")


def validate_upload(firmware, manifest, binary, expected):
    validate_artifact(firmware, manifest, binary)
    for key in ("build", "source_digest", "firmware_sha256"):
        if not expected.get(key) or manifest.get(key) != expected[key]:
            raise ValueError("업로드 준비 중 빌드가 변경됐습니다. 기기에 쓰기 전에 중단합니다")
    if upload_spec(manifest["images"]) != upload_spec(expected.get("images", [])):
        raise ValueError("업로드 이미지 목록/주소가 변경됐습니다")


@contextmanager
def staged_artifact(firmware, manifest, binary):
    """공유 빌드 폴더에서 분리한 검증된 바이트만 외부 업로더에 전달한다."""
    validate_artifact(firmware, manifest, binary)
    with tempfile.TemporaryDirectory(prefix="llm-deck-image-") as directory:
        folder = Path(directory)  # 생성 시 0700, 다른 빌드는 이 경로를 사용하지 않는다
        snapshot = {**manifest, "images": []}
        for index, image in enumerate(manifest["images"]):
            data = Path(image["path"]).read_bytes()
            if hashlib.sha256(data).hexdigest() != image["sha256"]:
                raise ValueError("복사 중 업로드 이미지가 변경됐습니다")
            name = "firmware.bin" if image["role"] == "app" else f"{index}.bin"
            destination = folder / name
            destination.write_bytes(data)
            destination.chmod(0o400)
            snapshot["images"].append({**image, "path": str(destination)})
        elf = Path(binary).with_suffix(".elf").read_bytes()
        if hashlib.sha256(elf).hexdigest()[:20] != manifest["build"]:
            raise ValueError("복사 중 ELF 가 변경됐습니다")
        staged_binary = folder / "firmware.bin"
        staged_binary.with_suffix(".elf").write_bytes(elf)
        staged_binary.with_suffix(".elf").chmod(0o400)
        validate_artifact(firmware, snapshot, staged_binary)
        snapshot["staged_manifest"] = str(folder / "manifest.json")
        write_json(folder / "manifest.json", snapshot)
        (folder / "manifest.json").chmod(0o400)
        yield snapshot, staged_binary


def pin_upload_environment(env, manifest):
    """PlatformIO 가 공유 $SOURCE 대신 스냅샷 파일들만 esptool 에 넘기게 한다."""
    import shlex
    current = [(int(env.subst(str(offset)), 0), env.subst(str(path))) for offset, path in env.get("FLASH_EXTRA_IMAGES", [])]
    extras = [image for image in manifest["images"] if image["role"] == "extra"]
    app = next(image for image in manifest["images"] if image["role"] == "app")
    if ([offset for offset, _ in current] != [image["offset"] for image in extras]
            or int(env.subst("$ESP32_APP_OFFSET"), 0) != app["offset"]):
        raise ValueError("PlatformIO 의 업로드 주소가 검증한 이미지와 다릅니다")
    flags = list(env["UPLOADERFLAGS"])
    for offset, path in current:
        for index in range(len(flags) - 1):
            try:
                candidate = int(env.subst(str(flags[index])), 0)
            except ValueError:
                continue
            if candidate == offset and env.subst(str(flags[index + 1])) == path:
                del flags[index:index + 2]
                break
        else:
            raise ValueError("업로드 인자의 원본 이미지 경로를 확인하지 못했습니다")
    for image in extras:
        flags += [hex(image["offset"]), shlex.quote(image["path"])]
    env.Replace(UPLOADERFLAGS=flags,
                UPLOADCMD='$UPLOADER $UPLOADERFLAGS $ESP32_APP_OFFSET ' + shlex.quote(app["path"]),
                FLASH_EXTRA_IMAGES=[(hex(image["offset"]), image["path"]) for image in extras])


def run_staged_uploader(env, manifest, port):
    """SCons 가 이미 캐시한 원본 명령을 실행하지 않고 검증된 명령만 실행한다."""
    import shlex
    import subprocess
    if not port:
        raise ValueError("검증된 업로드 포트가 없습니다")
    pin_upload_environment(env, manifest)
    env.Replace(UPLOAD_PORT=port)
    command = shlex.split(env.subst("$UPLOADCMD"))
    result = subprocess.run(command)
    # PreAction 에서 env.Replace 만 하면 뒤의 캐시된 명령에는 반영되지 않는다.
    # 이 프로세스를 끝내 원본 $SOURCE 를 쓰는 기존 action 이 뒤따르지 않게 한다.
    env.Exit(result.returncode)


def write_json(path, data):
    path = Path(path)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n")
    tmp.replace(path)
