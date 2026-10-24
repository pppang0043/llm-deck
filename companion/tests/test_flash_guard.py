"""실제 장치 없이 잘못된 worktree·하향 업로드·이전 로그의 성공 오인을 재현한다."""
import json
import sys
import subprocess
import runpy
from pathlib import Path

import pytest

SCRIPTS = Path(__file__).resolve().parents[2] / "firmware/scripts"
if not (SCRIPTS / "flash_guard.py").exists():
    pytest.skip("펌웨어 스크립트가 없는 배포본", allow_module_level=True)
pytest.importorskip("fcntl", reason="Mac/POSIX USB 업로더 테스트")
sys.path.insert(0, str(SCRIPTS))
import build_manifest as manifest
import flash_guard as guard
SCONS_RUNNER = Path.home() / ".platformio/packages/tool-scons/scons.py"


@pytest.fixture
def artifact(tmp_path):
    firmware = tmp_path / "project/firmware"
    (firmware / "src").mkdir(parents=True)
    (firmware / "src/link.h").write_text("#define FW_PROTO 5\n")
    (firmware / "platformio.ini").write_text("[platformio]\n")
    companion = firmware.parent / "companion"
    companion.mkdir()
    (companion / "version.py").write_text("PROTO = 5\n")
    build = firmware / ".pio/build/llm-deck"
    build.mkdir(parents=True)
    binary = build / "firmware.bin"
    binary.write_bytes(b"firmware image")
    binary.with_suffix(".elf").write_bytes(b"actual ELF")
    extras = []
    for offset, name in [(0, "bootloader.bin"), (0x8000, "partitions.bin"), (0xe000, "boot_app0.bin")]:
        image = build / name
        image.write_bytes(name.encode())
        extras.append({"offset": offset, "path": str(image), "sha256": manifest.image_digest(image), "role": "extra"})
    images = extras + [{"offset": 0x10000, "path": str(binary), "sha256": manifest.image_digest(binary), "role": "app"}]
    data = {"proto": 5, "build": manifest.image_digest(binary.with_suffix(".elf"))[:20],
            "source_digest": manifest.source_digest(firmware), "firmware_sha256": manifest.image_digest(binary), "images": images}
    manifest.write_json(build / "build-manifest.json", data)
    return firmware, binary, data


@pytest.mark.parametrize("name", ["bootloader.bin", "partitions.bin", "boot_app0.bin"])
def test_every_uploaded_image_is_checked(artifact, name):
    firmware, binary, data = artifact
    (binary.parent / name).write_bytes(b"wrong image")
    with pytest.raises(ValueError, match="이미지"):
        manifest.validate_artifact(firmware, data, binary)


def test_staged_images_survive_concurrent_build_output_replacement(artifact):
    firmware, binary, data = artifact
    with manifest.staged_artifact(firmware, data, binary) as (snapshot, staged_binary):
        for image in data["images"]:
            Path(image["path"]).write_bytes(b"concurrent build replacement")
        binary.with_suffix(".elf").write_bytes(b"another ELF")
        manifest.validate_artifact(firmware, snapshot, staged_binary)
        assert staged_binary.read_bytes() == b"firmware image"
        assert all(Path(image["path"]).parent == staged_binary.parent for image in snapshot["images"])
    assert not staged_binary.parent.exists()


def test_upload_arguments_use_only_staged_files_and_original_addresses(artifact):
    firmware, binary, data = artifact

    class Env(dict):
        def subst(self, value): return value.replace("$ESP32_APP_OFFSET", "0x10000")
        def Replace(self, **values): self.update(values)

    original = [(f'0x{image["offset"]:04x}', image["path"]) for image in data["images"] if image["role"] == "extra"]
    with manifest.staged_artifact(firmware, data, binary) as (snapshot, staged_binary):
        env = Env(FLASH_EXTRA_IMAGES=original, ESP32_APP_OFFSET="0x10000",
                  UPLOADERFLAGS=["--chip", "esp32s3"] + [part for pair in original for part in pair])
        manifest.pin_upload_environment(env, snapshot)
        assert "$SOURCE" not in env["UPLOADCMD"]
        assert str(staged_binary) in env["UPLOADCMD"]
        assert all(str(binary.parent) not in str(arg) for arg in env["UPLOADERFLAGS"])
        assert [int(offset, 0) for offset, _ in env["FLASH_EXTRA_IMAGES"]] == [0, 0x8000, 0xe000]
        env["ESP32_APP_OFFSET"] = "0x20000"
        env.subst = lambda value: value.replace("$ESP32_APP_OFFSET", env["ESP32_APP_OFFSET"])
        with pytest.raises(ValueError, match="주소"):
            manifest.pin_upload_environment(env, snapshot)


def test_real_scons_executes_staged_command_once_and_skips_cached_original(artifact, tmp_path):
    if not SCONS_RUNNER.exists():
        pytest.skip("로컬 SCons 실행기가 필요합니다")
    firmware, binary, data = artifact
    extras = [(hex(image["offset"]), image["path"]) for image in data["images"] if image["role"] == "extra"]
    with manifest.staged_artifact(firmware, data, binary) as (snapshot, staged_binary):
        script = tmp_path / "SConstruct"
        script.write_text(
            "import sys, shlex\n"
            f"sys.path.insert(0, {str(SCRIPTS)!r})\n"
            "from build_manifest import run_staged_uploader\n"
            f"extras = {extras!r}\n"
            "env=Environment(UPLOADER=shlex.join([sys.executable,'-c',"
            "'import json,sys; print(\"ARGS=\"+json.dumps(sys.argv[1:]))']),"
            "UPLOADERFLAGS=['--chip','esp32s3']+[x for p in extras for x in p],"
            "FLASH_EXTRA_IMAGES=extras,ESP32_APP_OFFSET='0x10000',"
            "UPLOADCMD='$UPLOADER $UPLOADERFLAGS $ESP32_APP_OFFSET $SOURCE')\n"
            f"def staged(source,target,env): run_staged_uploader(env, {snapshot!r}, '/fake/usb')\n"
            "node=env.Alias('check_arguments',[],env.Action('$UPLOADCMD'))\n"
            "env.AddPreAction(node,staged)\nenv.AlwaysBuild(node)\nDefault(node)\n")
        result = subprocess.run([sys.executable, str(SCONS_RUNNER), "-Q", "-f", str(script)],
                                cwd=tmp_path, capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stdout + result.stderr
        commands = [json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("ARGS=")]
        assert len(commands) == 1, result.stdout
        assert commands[0][-1] == str(staged_binary)
        assert all(str(binary.parent) not in arg for arg in commands[0])


@pytest.mark.parametrize("invalid", [None, "missing_digest", "missing_images", "changed_partition"])
def test_manifest_generation_and_upload_hook_are_connected(artifact, monkeypatch, invalid):
    firmware, binary, data = artifact

    class Env(dict):
        def subst(self, text):
            return (text.replace("$PROJECT_DIR", str(firmware))
                    .replace("$BUILD_DIR", str(binary.parent)).replace("${PROGNAME}", "firmware")
                    .replace("$ESP32_APP_OFFSET", "0x10000"))
        def PioPlatform(self): return self
        def get_package_dir(self, name): return None
        def GetProjectOption(self, name, default=None): return default
        def Append(self, **values): self.update(values)
        def AddPostAction(self, name, callback): self.post = callback
        def AddPreAction(self, name, callback): self.pre = callback

    env = Env(FLASH_EXTRA_IMAGES=[(hex(i["offset"]), i["path"]) for i in data["images"] if i["role"] == "extra"])
    monkeypatch.setattr(sys, "path", sys.path.copy())
    hooks = runpy.run_path(str(SCRIPTS / "build_identity.py"), init_globals={"env": env, "Import": lambda *a: None})
    env.post(None, None, env)
    generated = json.loads((binary.parent / "build-manifest.json").read_text())
    manifest.validate_artifact(firmware, generated, binary)
    assert [i["offset"] for i in generated["images"]] == [0, 0x8000, 0xe000, 0x10000]
    with manifest.staged_artifact(firmware, generated, binary) as (snapshot, staged_binary):
        for name, value in {
            "LLMDECK_EXPECTED_BUILD": snapshot["build"], "LLMDECK_EXPECTED_SOURCE": snapshot["source_digest"],
            "LLMDECK_EXPECTED_SHA": snapshot["firmware_sha256"],
            "LLMDECK_EXPECTED_IMAGES": json.dumps(manifest.upload_spec(snapshot["images"])),
            "LLMDECK_STAGED_MANIFEST": snapshot["staged_manifest"], "LLMDECK_UPLOAD_PORT": "/fake/usb",
        }.items():
            monkeypatch.setenv(name, value)
        called = []
        # 훅 연결만 검증하며 외부 업로더는 호출하지 않는다.
        env.pre.__globals__["run_staged_uploader"] = lambda env, md, port: called.append((md, port))
        if invalid == "missing_digest":
            monkeypatch.delenv("LLMDECK_EXPECTED_SHA")
        elif invalid == "missing_images":
            monkeypatch.delenv("LLMDECK_EXPECTED_IMAGES")
        elif invalid == "changed_partition":
            part = Path(snapshot["images"][1]["path"])
            part.chmod(0o600)
            part.write_bytes(b"tampered partition")
        if invalid:
            with pytest.raises((ValueError, RuntimeError)):
                env.pre(None, None, env)
            assert called == []
        else:
            env.pre(None, None, env)
            assert len(called) == 1 and called[0][1] == "/fake/usb"
            assert all(Path(i["path"]).parent == staged_binary.parent for i in called[0][0]["images"])


def test_missing_port_never_starts_external_uploader(monkeypatch):
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: pytest.fail("외부 업로더 실행"))
    with pytest.raises(ValueError, match="포트"):
        manifest.run_staged_uploader(None, {}, None)


def test_artifact_detects_changed_source_and_swapped_images(artifact):
    firmware, binary, data = artifact
    manifest.validate_artifact(firmware, data, binary)
    binary.write_bytes(b"other worktree image")
    with pytest.raises(ValueError, match="이미지"):
        manifest.validate_artifact(firmware, data, binary)
    binary.write_bytes(b"firmware image")
    (firmware / "src/link.h").write_text("#define FW_PROTO 3\n")
    with pytest.raises(ValueError, match="소스"):
        manifest.validate_artifact(firmware, data, binary)


def test_old_worktree_cannot_downgrade_even_with_matching_old_companion(artifact):
    firmware, _, data = artifact
    old = {**data, "proto": 3}
    with pytest.raises(ValueError, match="하향"):
        manifest.validate_plan(old, 3, {"fw_proto": 5}, {}, firmware.parent)
    with pytest.raises(ValueError, match="버전"):
        manifest.validate_plan(old, 5, {"fw_proto": 3}, {}, firmware.parent)
    with pytest.raises(ValueError, match="하향"):
        manifest.validate_plan(data, 5, {"fw_proto": 3}, {"proto": 6}, firmware.parent)


def test_different_worktree_requires_explicit_switch(artifact):
    firmware, _, data = artifact
    previous = {"root": "/other/worktree", "proto": 5, "build": "old"}
    with pytest.raises(ValueError, match="worktree"):
        manifest.validate_plan(data, 5, {"fw_proto": 5}, previous, firmware.parent)
    manifest.validate_plan(data, 5, {"fw_proto": 5}, previous, firmware.parent, allow_worktree_switch=True)


def test_post_upload_requires_exact_device_protocol_and_binary(artifact):
    _, _, data = artifact
    current = {"id": "aabbccddeeff", "fw_proto": 5, "build": data["build"]}
    manifest.validate_running(data, current, current["id"])
    for changed in ({"id": "112233445566"}, {"fw_proto": 3}, {"build": "older"}):
        with pytest.raises(ValueError):
            manifest.validate_running(data, {**current, **changed}, current["id"])


def fake_upload(monkeypatch, artifact, *, current_proto=5, running_build=None):
    firmware, _, data = artifact
    calls = []
    monkeypatch.setattr(guard, "FIRMWARE", firmware)
    monkeypatch.setattr(sys, "argv", ["flash_guard.py"])
    monkeypatch.setattr(guard, "run_pio", lambda pio, *args, **kw: calls.append(args) or True)
    monkeypatch.setattr(guard, "find_device", lambda *a, **kw: ("/fake/usb", "AA:BB:CC:DD:EE:FF", "aabbccddeeff"))
    monkeypatch.setattr(guard.subprocess, "run", lambda *a, **kw: type("Result", (), {"stdout": b""})())

    class Helper:
        def __init__(self, state): pass
        def stop(self): calls.append("stopped")
        def restore(self): calls.append("restored")

    monkeypatch.setattr(guard, "Helper", Helper)

    def probe(port, **kw):
        if "legacy_proto" in kw:
            return {"id": "aabbccddeeff", "fw_proto": current_proto}
        return {"id": "aabbccddeeff", "fw_proto": 5, "build": running_build or data["build"]}

    monkeypatch.setattr(guard, "probe", probe)
    return calls


def test_downgrade_never_invokes_upload_and_restores_helper(monkeypatch, artifact):
    calls = fake_upload(monkeypatch, artifact, current_proto=6)
    with pytest.raises(ValueError, match="하향"):
        guard.main()
    assert calls == [(), "stopped", "restored"]


def test_only_verified_running_build_is_committed(monkeypatch, artifact):
    calls = fake_upload(monkeypatch, artifact)
    guard.main()
    assert ("-t", "upload", "--upload-port", "/fake/usb") in calls
    assert calls[-1] == "restored"
    saved = json.loads((Path.home() / ".llm-deck/firmware-deployments.json").read_text())
    assert saved["aabbccddeeff"]["build"] == artifact[2]["build"]


def test_wrong_post_upload_image_fails_and_restores_helper(monkeypatch, artifact):
    calls = fake_upload(monkeypatch, artifact, running_build="old image")
    with pytest.raises(ValueError, match="일치"):
        guard.main()
    assert calls[-1] == "restored"
    assert not (Path.home() / ".llm-deck/firmware-deployments.json").exists()


def test_changed_source_during_device_probe_never_uploads(monkeypatch, artifact):
    calls = fake_upload(monkeypatch, artifact)

    def probe(*a, **kw):
        (artifact[0] / "src/new.cpp").write_text("// simultaneous edits\n")
        return {"fw_proto": 5, "id": "aabbccddeeff"}

    monkeypatch.setattr(guard, "probe", probe)
    with pytest.raises(ValueError, match="소스"):
        guard.main()
    assert calls == [(), "stopped", "restored"]


def test_probe_rejects_stale_unchallenged_event(monkeypatch):
    class Connection:
        def __enter__(self): return self
        def __exit__(self, *a): pass
        def reset_input_buffer(self): pass
        def write(self, data): pass
    import serial
    monkeypatch.setattr(serial, "Serial", lambda *a, **kw: Connection())
    monkeypatch.setattr(guard, "read_events", lambda *a: iter([{"fw_proto": 5, "probe": "old-challenge"}]))
    with pytest.raises(RuntimeError, match="새 버전"):
        guard.probe("/fake/usb")


def test_live_upload_lock_blocks_another_uploader(tmp_path):
    with guard.flash_lock(tmp_path, timeout=1):
        with pytest.raises(RuntimeError, match="잠금"):
            with guard.flash_lock(tmp_path, timeout=0):
                pytest.fail("동시 업로더가 잠금을 통과했습니다")
    assert not (tmp_path / "flash.lock").exists()


def test_interrupted_upload_releases_lock(tmp_path):
    with pytest.raises(RuntimeError, match="interrupted"):
        with guard.flash_lock(tmp_path):
            raise RuntimeError("interrupted")
    with guard.flash_lock(tmp_path, timeout=0):
        pass


def test_probe_frame_parser_recovers_after_partial_audio_and_noise():
    body = b'{"fw_proto":5,"build":"current"}'
    frame = b"\xa5\x5a\x02" + len(body).to_bytes(2, "little") + body
    chunks = iter([b"boot log\n\xa5\x5a\x02\xff\xffnoise" + frame[:3], frame[3:8], frame[8:]])

    class Connection:
        def read(self, count): return next(chunks)

    assert next(guard.read_events(Connection(), 1)) == json.loads(body)


def test_upload_hook_rejects_a_rebuild_after_preflight(artifact):
    firmware, binary, data = artifact
    manifest.validate_upload(firmware, data, binary, data)
    for key in ("build", "source_digest", "firmware_sha256"):
        with pytest.raises(ValueError, match="업로드 준비"):
            manifest.validate_upload(firmware, data, binary, {**data, key: "different"})
