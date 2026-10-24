#!/bin/sh
# Mac 에서 PC 도우미를 로그인할 때 자동으로 실행되게 한다.
#   sh macos/install.sh            설치 (다시 실행하면 새로 만든다)
#   sh macos/install.sh uninstall  제거
#
# 블루투스 권한은 "앱" 단위로만 받을 수 있어서, 도우미를 작은 앱(~/Applications/LLM Deck.app)으로 감싸
# launchd 가 그 앱을 바로 띄운다. 처음 실행할 때 블루투스 허용 창이 뜨면 "허용"을 누른다.
# 이 앱은 메뉴 막대에 아이콘을 두고, 거기서 기기 화면을 보는 미러링 창을 연다 (macos/app.swift).
set -e
COMPANION=$(cd "$(dirname "$0")/.." && pwd)
APP="$HOME/Applications/LLM Deck.app"
NEW_APP="$HOME/Applications/.LLM Deck.app.new"  # 여기서 다 만든 뒤에 바꿔 넣는다 (빌드가 실패해도 쓰던 앱이 남는다)
LABEL=com.llmdeck.companion
DOMAIN="gui/$(id -u)"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/.llm-deck/companion.log"
OLD_MIRROR_APP="$HOME/Applications/LLM Deck Mirror.app"  # 예전에 따로 만들던 미러링 앱
SIGN_NAME="LLM Deck Local Signing"
LOGIN_KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"  # 예전에는 여기에 만들었다
# 서명 전용 키체인. 로그인 키체인의 키는 codesign 이 쓸 때마다 "키체인 접근 허용" 창을 띄워
# (창이 안 보이는 곳에서 설치하면 멈추거나 실패한다) 따로 만든다. 암호는 이 키 하나만 지키는 것이라 고정값을 쓴다
SIGN_KEYCHAIN="$HOME/.llm-deck/signing.keychain-db"
SIGN_PASS=llmdeck
# 터미널의 orca 명령 (macos/orca.sh). ORCA 가 거는 링크는 끊어질 수 있어 부를 때마다 앱을 찾는 이것으로 바꾼다
ORCA_SHIM="$HOME/.local/bin/orca"
SHIM_MARK="LLM Deck orca shim"

# 서명용 인증서의 SHA-1. 그냥(ad-hoc) 서명하면 빌드할 때마다 앱 지문이 바뀌어 macOS 가 마이크·블루투스 허용을 잊는다.
# 이 Mac 에서만 쓰는 자체 서명 인증서로 서명하면 다시 빌드해도 허용이 남는다. 없으면 한 번 만든다
# (자체 서명이라 "신뢰하지 않음"으로 나오지만 서명에는 쓸 수 있다. 그래서 이름 대신 SHA-1 로 고른다)
signing_hash() {
  security find-identity -p codesigning "$SIGN_KEYCHAIN" 2>/dev/null | awk '/^ *1\)/ { print $2; exit }'
}
signing_identity() {
  if [ ! -f "$SIGN_KEYCHAIN" ]; then
    security create-keychain -p "$SIGN_PASS" "$SIGN_KEYCHAIN" || return 1
    security set-keychain-settings "$SIGN_KEYCHAIN"  # 저절로 잠기지 않게
  fi
  security unlock-keychain -p "$SIGN_PASS" "$SIGN_KEYCHAIN" || return 1
  if [ -z "$(signing_hash)" ]; then
    tmp=$(mktemp -d)
    printf '%s\n' '[req]' 'distinguished_name = dn' 'prompt = no' 'x509_extensions = ext' '[dn]' "CN = $SIGN_NAME" '[ext]' \
      'basicConstraints = critical, CA:false' 'keyUsage = critical, digitalSignature' 'extendedKeyUsage = critical, codeSigning' \
      > "$tmp/cert.cnf"
    # 시스템 openssl(LibreSSL)로 만든 p12 라야 security 가 읽는다
    /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 7300 -config "$tmp/cert.cnf" \
      -keyout "$tmp/key.pem" -out "$tmp/cert.pem" 2>/dev/null &&
      /usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -name "$SIGN_NAME" \
        -passout pass:"$SIGN_PASS" -out "$tmp/id.p12" &&
      security import "$tmp/id.p12" -k "$SIGN_KEYCHAIN" -P "$SIGN_PASS" -T /usr/bin/codesign >/dev/null &&
      # codesign 이 허용 창 없이 키를 쓰게 한다 (이 키체인 암호로 되므로 로그인 암호는 필요 없다)
      security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$SIGN_PASS" "$SIGN_KEYCHAIN" >/dev/null
    rm -rf "$tmp"
  fi
  signing_hash
}

# codesign 은 검색 목록에 있는 키체인에서만 인증서를 찾는다. 서명하는 동안만 넣었다가 원래대로 돌린다
sign_app() {
  hash=$1
  saved=$(security list-keychains -d user | sed 's/^ *"//; s/"$//')
  # shellcheck disable=SC2086
  security list-keychains -d user -s $saved "$SIGN_KEYCHAIN"
  # 허용 창이 떠서 멈추면 1분 뒤 그만두고 그냥 서명으로 넘어간다
  codesign --force -s "$hash" --keychain "$SIGN_KEYCHAIN" "$NEW_APP" & pid=$!
  (sleep 60; kill "$pid" 2>/dev/null) & guard=$!
  rc=0; wait "$pid" || rc=$?
  kill "$guard" 2>/dev/null || true
  wait "$guard" 2>/dev/null || true
  # shellcheck disable=SC2086
  security list-keychains -d user -s $saved
  return "$rc"
}

# 바꿔도 되는 orca: 없음, 끊어진 링크, ORCA 가 건 링크, 예전에 깐 것. 사용자가 따로 둔 것은 건드리지 않는다
orca_shim_replaceable() {
  [ ! -e "$ORCA_SHIM" ] && return 0
  case "$(readlink "$ORCA_SHIM")" in *Orca.app/*) return 0 ;; esac
  grep -q "$SHIM_MARK" "$ORCA_SHIM" 2>/dev/null
}

install_orca_shim() {
  if ! orca_shim_replaceable; then
    echo "orca 명령: $ORCA_SHIM 이 이미 있어 그대로 둡니다."
    return
  fi
  mkdir -p "$(dirname "$ORCA_SHIM")"
  rm -f "$ORCA_SHIM"  # 링크면 cp 가 링크가 가리키는 파일에 덮어쓴다
  cp "$COMPANION/macos/orca.sh" "$ORCA_SHIM"
  chmod +x "$ORCA_SHIM"
  case ":$PATH:" in
    *":$HOME/.local/bin:"*) ;;
    *) echo "orca 명령: ~/.zprofile 에 export PATH=\"\$HOME/.local/bin:\$PATH\" 를 더해야 터미널에서 쓸 수 있습니다." ;;
  esac
}

# 끝날 때까지 기다린다 (아직 등록돼 있으면 bootstrap 이 "Bootstrap failed: 5" 로 실패한다)
stop() {
  launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
  i=0
  while launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1 && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
  # 예전 설치(open -W 로 띄움)나 터미널에서 띄운 앱은 bootout 으로 꺼지지 않는다
  pkill -f "LLM Deck.app/Contents/MacOS/llm-deck" 2>/dev/null || true
  i=0
  while pgrep -f "LLM Deck.app/Contents/MacOS/llm-deck" >/dev/null && [ $i -lt 25 ]; do sleep 0.2; i=$((i + 1)); done
  # 앱이 먼저 죽어 남은 도우미가 USB 포트를 붙잡고 있지 않게
  pkill -f "\.venv/bin/python main.py" 2>/dev/null || true
}

if [ "$1" = "uninstall" ]; then
  stop
  rm -f "$PLIST"
  rm -rf "$APP" "$NEW_APP"
  security delete-keychain "$SIGN_KEYCHAIN" >/dev/null 2>&1 || true
  security delete-identity -c "$SIGN_NAME" "$LOGIN_KEYCHAIN" >/dev/null 2>&1 || true
  grep -q "$SHIM_MARK" "$ORCA_SHIM" 2>/dev/null && rm -f "$ORCA_SHIM"
  echo "제거했습니다."
  exit 0
fi

mkdir -p "$HOME/.llm-deck" "$HOME/Library/LaunchAgents"
rm -rf "$NEW_APP"
mkdir -p "$NEW_APP/Contents/MacOS"
cat > "$NEW_APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>$LABEL</string>
  <key>CFBundleName</key><string>LLM Deck</string>
  <key>CFBundleExecutable</key><string>llm-deck</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSUIElement</key><true/>
  <key>LLMDeckCompanionDir</key><string>$COMPANION</string>
  <key>NSAppTransportSecurity</key><dict><key>NSAllowsLocalNetworking</key><true/></dict>
  <key>NSBluetoothAlwaysUsageDescription</key><string>LLM Deck 기기와 블루투스로 연결합니다.</string>
  <key>NSMicrophoneUsageDescription</key><string>기기에서 말로 질문할 때 이 Mac 마이크로 받아 적습니다.</string>
</dict>
</plist>
EOF
# Swift 6 로 먼저, 안 되면(예전 Command Line Tools) Swift 5 로
BIN="$NEW_APP/Contents/MacOS/llm-deck"
if ! swiftc -O -swift-version 6 -o "$BIN" "$COMPANION/macos/app.swift" 2>/dev/null &&
   ! swiftc -O -swift-version 5 -o "$BIN" "$COMPANION/macos/app.swift"; then
  rm -rf "$NEW_APP"
  echo "앱을 만들지 못했습니다. xcode-select --install 로 Command Line Tools 를 깔거나 업데이트하고 다시 실행하세요."
  [ -d "$APP" ] && echo "쓰던 앱($APP)은 그대로 두었습니다."
  exit 1
fi
IDENTITY=$(signing_identity || true)
if [ -z "$IDENTITY" ] || ! sign_app "$IDENTITY"; then
  codesign --force -s - "$NEW_APP"
  echo "주의: 서명용 인증서를 쓰지 못해 그냥 서명했습니다. 다시 설치할 때마다 마이크·블루투스 허용을 다시 눌러야 할 수 있습니다."
fi

stop
# 서명이 바뀌었는지 보려고 (바뀌면 예전 허용은 듣지 않는다)
OLD_REQ=$(codesign -dr - "$APP" 2>/dev/null | grep designated || true)
pkill -f "LLM Deck Mirror.app/Contents/MacOS/" 2>/dev/null || true
rm -rf "$OLD_MIRROR_APP" "$APP"
mv "$NEW_APP" "$APP"
NEW_REQ=$(codesign -dr - "$APP" 2>/dev/null | grep designated || true)
if [ -n "$OLD_REQ" ] && [ "$OLD_REQ" != "$NEW_REQ" ]; then
  # 예전 서명에 묶인 허용은 macOS 가 묻지도 않고 막는다. 지워서 다시 묻게 한다
  # (손쉬운 사용은 설정 화면에 켜진 채로 보여서 더 헷갈린다: 받아쓰기·키워드 입력이 막힌다)
  tccutil reset Microphone "$LABEL" >/dev/null 2>&1 || true
  tccutil reset BluetoothAlways "$LABEL" >/dev/null 2>&1 || true
  tccutil reset Accessibility "$LABEL" >/dev/null 2>&1 || true
  echo "앱 서명이 바뀌었습니다. 마이크·블루투스·손쉬운 사용 허용 창이 다시 뜨면 \"허용\"을 누르세요."
fi

# 앱을 open 으로 거치지 않고 바로 띄운다. 그래야 bootout·kickstart -k 의 종료 신호가 앱에 닿는다
# (앱은 그 신호를 도우미에게 넘기고 같이 끝난다)
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$APP/Contents/MacOS/llm-deck</string>
  </array>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
  <key>LimitLoadToSessionType</key><string>Aqua</string>
  <key>ProcessType</key><string>Interactive</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>30</integer>
</dict>
</plist>
EOF
launchctl bootstrap "$DOMAIN" "$PLIST"
install_orca_shim

echo "설치했습니다. 기록: $LOG"
echo "미러링: 메뉴 막대의 LLM Deck 아이콘 > 미러링 열기 (또는 open -a 'LLM Deck')"
echo "잠시 멈추기: launchctl bootout gui/\$(id -u)/$LABEL"
