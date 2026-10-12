"""IMA ADPCM 풀기. 블루투스로 오는 마이크 소리는 이 방식으로 1/4 로 압축되어 있다."""

import struct

INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8)
STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28,
    31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)  # fmt: skip


def decode(data: bytes, predictor: int, index: int) -> bytes:
    """4비트 코드 묶음을 16비트 PCM 으로 되돌린다. 바이트마다 아래 4비트가 먼저다."""
    samples = []
    for byte in data:
        for code in (byte & 0x0F, byte >> 4):
            step = STEP_TABLE[index]
            delta = step >> 3
            if code & 4:
                delta += step
            if code & 2:
                delta += step >> 1
            if code & 1:
                delta += step >> 2
            predictor = max(-32768, min(32767, predictor - delta if code & 8 else predictor + delta))
            index = max(0, min(88, index + INDEX_TABLE[code & 7]))
            samples.append(predictor)
    return struct.pack(f"<{len(samples)}h", *samples)
