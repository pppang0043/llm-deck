"""stt = "auto" 의 받아쓰기 순서: 여유가 있으면 Whisper, 없으면 ChatGPT, 그다음 OpenAI API, 마지막은 Whisper.
S3 와 S31 이 함께 쓰는 테스트다. 양쪽 Assistant 에 공통인 것만 쓴다."""

import json
import urllib.error
from types import SimpleNamespace
from unittest import mock

import numpy as np
import pytest

import assistant
import chatgpt_stt
import openai_stt
import stt_chain


class FakeWhisper:
    def __init__(self, calls):
        self.calls = calls

    def transcribe(self, *args, **kwargs):
        self.calls.append("whisper")
        return [SimpleNamespace(text="whisper 글", no_speech_prob=0.0, avg_logprob=0.0)], None


@pytest.fixture
def run(monkeypatch):
    """받아쓰기 한 번을 돌려 (쓴 엔진 순서, 받아 적은 글) 을 돌려준다."""

    def go(busy=False, chatgpt="chatgpt 글", api="api 글", model_ok=True, stt="auto"):
        calls = []
        a = assistant.Assistant({"stt": stt})

        def load():
            if not model_ok and not a.load_failed_once:
                a.load_failed_once = True
                raise OSError("모델 없음")
            a.model = FakeWhisper(calls)

        a.load_failed_once = False
        monkeypatch.setattr(a, "_load_model", load)
        monkeypatch.setattr(a, "_set", lambda *args, **kwargs: None)
        done = []
        monkeypatch.setattr(a, "_finish_dictation", lambda take, text: done.append(text))
        monkeypatch.setattr(assistant, "_has_speech", lambda pcm: True)
        monkeypatch.setattr(stt_chain, "pc_busy", lambda: busy)
        monkeypatch.setattr(chatgpt_stt, "text", lambda *args: calls.append("chatgpt") or chatgpt)
        monkeypatch.setattr(openai_stt, "text", lambda *args: calls.append("api") or api)
        take = assistant._Take(dictate=True)
        take.chunks = [np.full((1600, 1), 1000, dtype=np.int16)]
        a._answer(take)
        return calls, done[0] if done else None

    return go


def test_idle_pc_uses_only_whisper(run):
    assert run() == (["whisper"], "whisper 글")


def test_busy_pc_uses_chatgpt(run):
    assert run(busy=True) == (["chatgpt"], "chatgpt 글")


def test_chatgpt_failure_goes_to_api(run):
    assert run(busy=True, chatgpt=None) == (["chatgpt", "api"], "api 글")


def test_all_cloud_failure_falls_back_to_whisper(run):
    assert run(busy=True, chatgpt=None, api=None) == (["chatgpt", "api", "whisper"], "whisper 글")


def test_broken_whisper_goes_to_cloud(run):
    assert run(model_ok=False) == (["chatgpt"], "chatgpt 글")


def test_chatgpt_mode_is_unchanged(run):
    assert run(stt="chatgpt", chatgpt=None) == (["chatgpt", "whisper"], "whisper 글")


@pytest.mark.parametrize("busy, loaded", [(True, False), (False, True)])
def test_warm_loads_whisper_only_when_idle(monkeypatch, busy, loaded):
    a = assistant.Assistant({"stt": "auto"})
    load = mock.Mock()
    monkeypatch.setattr(a, "_load_model", load)
    monkeypatch.setattr(stt_chain, "pc_busy", lambda: busy)
    a._warm(assistant._Take(dictate=True))
    assert load.called is loaded


def test_cloud_text_names_the_engine(monkeypatch):
    monkeypatch.setattr(chatgpt_stt, "text", lambda *args: "글")
    assert stt_chain.cloud_text(_pcm(), 16000, "ko") == ("글", "ChatGPT")
    monkeypatch.setattr(chatgpt_stt, "text", lambda *args: None)
    monkeypatch.setattr(openai_stt, "text", lambda *args: "글")
    assert stt_chain.cloud_text(_pcm(), 16000, "ko") == ("글", "OpenAI API")
    monkeypatch.setattr(openai_stt, "text", lambda *args: None)
    assert stt_chain.cloud_text(_pcm(), 16000, "ko") == (None, "OpenAI API")


def test_busy_rules(monkeypatch):
    monkeypatch.setattr(stt_chain.psutil, "cpu_count", lambda: 8)
    free = SimpleNamespace(available=8 * 1024**3)
    monkeypatch.setattr(stt_chain.psutil, "virtual_memory", lambda: free)
    monkeypatch.setattr(stt_chain.psutil, "getloadavg", lambda: (7.9, 0, 0))
    assert stt_chain.pc_busy() is False
    monkeypatch.setattr(stt_chain.psutil, "getloadavg", lambda: (8.0, 0, 0))
    assert stt_chain.pc_busy() is True
    monkeypatch.setattr(stt_chain.psutil, "getloadavg", lambda: (1.0, 0, 0))
    free.available = 2 * 1024**3
    assert stt_chain.pc_busy() is False
    free.available = 2 * 1024**3 - 1
    assert stt_chain.pc_busy() is True


def _pcm():
    return np.zeros(1600, dtype=np.float32)


def test_api_sends_model_language_and_key(monkeypatch):
    seen = {}

    def urlopen(request, timeout):
        seen["request"] = request
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = json.dumps({"text": " 안녕 "}).encode()
        return response

    monkeypatch.setattr(openai_stt, "load_keys", lambda path: {"OPENAI_API_KEY": "test-key"})
    monkeypatch.setattr(openai_stt.urllib.request, "urlopen", urlopen)
    assert openai_stt.text(_pcm(), 16000, "ko") == "안녕"
    assert seen["request"].get_header("Authorization") == "Bearer test-key"
    assert b'name="model"\r\n\r\ngpt-4o-transcribe' in seen["request"].data
    assert b'name="language"\r\n\r\nko' in seen["request"].data


def test_api_without_key_returns_none(monkeypatch):
    monkeypatch.setattr(openai_stt, "load_keys", lambda path: {})
    assert openai_stt.text(_pcm(), 16000, "ko") is None


def test_load_keys_reads_file_and_ignores_missing(tmp_path):
    keys = tmp_path / "keys.env"
    keys.write_text('# 설명\nOPENAI_API_KEY="test-key"\nOTHER = x\n빈줄\n', encoding="utf-8")
    assert openai_stt.load_keys(str(keys)) == {"OPENAI_API_KEY": "test-key", "OTHER": "x"}
    assert openai_stt.load_keys(str(tmp_path / "없음.env")) == {}


def test_api_failure_returns_none_and_hides_key(monkeypatch, capsys):
    def urlopen(request, timeout):
        raise urllib.error.HTTPError(openai_stt.URL, 500, "error", None, None)

    monkeypatch.setattr(openai_stt, "load_keys", lambda path: {"OPENAI_API_KEY": "test-key"})
    monkeypatch.setattr(openai_stt.urllib.request, "urlopen", urlopen)
    assert openai_stt.text(_pcm(), 16000, "ko") is None
    assert "test-key" not in capsys.readouterr().out
