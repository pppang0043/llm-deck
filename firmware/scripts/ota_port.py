# 무선 업로드 전에 llm-deck.local 을 집 안 주소(192.168.x.x 등)로 바꿔 둔다.
# 인터넷 회사 DNS 가 .local 이름에도 바깥 주소를 대답하는 일이 있어서, 그 주소로 보내면 "No response" 가 난다.
import ipaddress
import socket

Import("env")  # noqa: F821 (PlatformIO 가 넣어 준다)

port = env.subst("$UPLOAD_PORT")  # noqa: F821
if port.endswith(".local"):
    try:
        found = {info[4][0] for info in socket.getaddrinfo(port, 3232, socket.AF_INET)}
    except OSError:
        found = set()
    private = sorted(a for a in found if ipaddress.ip_address(a).is_private)
    if private:
        print(f"[ota] {port} -> {private[0]} (집 안 주소만 사용, 받은 주소: {', '.join(sorted(found))})")
        env.Replace(UPLOAD_PORT=private[0])  # noqa: F821
    else:
        print(f"[ota] {port} 의 집 안 주소를 찾지 못했습니다 ({', '.join(sorted(found)) or '없음'}). --upload-port <IP> 로 지정하세요.")
