"""stt = "auto" 의 판단과 클라우드 순서. S3 와 S31 도우미가 함께 쓰는 원본이다 (S31 은 이 파일을 링크로 쓴다)."""

import psutil

import chatgpt_stt
import openai_stt

# PC 에 여유가 없다고 보는 기준. 1분 부하가 코어 수 이상이거나 남은 메모리가 이보다 적을 때
BUSY_LOAD_PER_CORE = 1.0
BUSY_MEMORY_FREE = 2 * 1024**3


def pc_busy() -> bool:
    load = psutil.getloadavg()[0] / (psutil.cpu_count() or 1)
    return load >= BUSY_LOAD_PER_CORE or psutil.virtual_memory().available < BUSY_MEMORY_FREE


def cloud_text(pcm, rate: int, language: str) -> tuple[str | None, str]:
    """(받아 적은 글, 쓴 엔진). ChatGPT 가 안 되면 OpenAI API. 둘 다 안 되면 글은 None."""
    text = chatgpt_stt.text(pcm, rate, language)
    if text is not None:
        return text, "ChatGPT"
    return openai_stt.text(pcm, rate, language), "OpenAI API"
