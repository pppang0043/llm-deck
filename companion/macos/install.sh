#!/bin/sh
# Mac 에서 PC 도우미를 로그인할 때 자동으로 실행되게 한다.
#   sh macos/install.sh            설치 (다시 실행하면 새로 만든다)
#   sh macos/install.sh uninstall  제거
#
# 블루투스 권한은 "앱" 단위로만 받을 수 있어서, 도우미를 작은 앱(~/Applications/LLM Deck.app)으로 감싸
# open 으로 띄운다. 처음 실행할 때 블루투스 허용 창이 뜨면 "허용"을 누른다.
# 이 앱은 메뉴 막대에 아이콘을 두고, 거기서 기기 화면을 보는 미러링 창을 연다 (macos/app.swift).
set -e
COMPANION=$(cd "$(dirname "$0")/.." && pwd)
APP="$HOME/Applications/LLM Deck.app"
LABEL=com.llmdeck.companion
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/.llm-deck/companion.log"
OLD_MIRROR_APP="$HOME/Applications/LLM Deck Mirror.app"  # 예전에 따로 만들던 미러링 앱
SIGN_NAME="LLM Deck Local Signing"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"

# 서명용 인증서. 그냥(ad-hoc) 서명하면 빌드할 때마다 앱 지문이 바뀌어 macOS 가 마이크·블루투스 허용을 잊는다.
# 이 Mac 에서만 쓰는 자체 서명 인증서로 서명하면 다시 빌드해도 허용이 남는다. 없으면 한 번 만든다
signing_identity() {
  if ! security find-certificate -c "$SIGN_NAME" "$KEYCHAIN" >/dev/null 2>&1; then
    tmp=$(mktemp -d)
    printf '%s\n' '[req]' 'distinguished_name = dn' 'prompt = no' 'x509_extensions = ext' '[dn]' "CN = $SIGN_NAME" '[ext]' \
      'basicConstraints = critical, CA:false' 'keyUsage = critical, digitalSignature' 'extendedKeyUsage = critical, codeSigning' \
      > "$tmp/cert.cnf"
    # 시스템 openssl(LibreSSL)로 만든 p12 라야 security 가 읽는다
    /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 7300 -config "$tmp/cert.cnf" \
      -keyout "$tmp/key.pem" -out "$tmp/cert.pem" 2>/dev/null &&
      /usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -name "$SIGN_NAME" \
        -passout pass:llmdeck -out "$tmp/id.p12" &&
      security import "$tmp/id.p12" -k "$KEYCHAIN" -P llmdeck -T /usr/bin/codesign >/dev/null
    rm -rf "$tmp"
  fi
  security find-certificate -c "$SIGN_NAME" "$KEYCHAIN" >/dev/null 2>&1 && echo "$SIGN_NAME"
}

stop() {
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  pkill -f "LLM Deck.app/Contents/MacOS/llm-deck" 2>/dev/null || true
}

if [ "$1" = "uninstall" ]; then
  stop
  rm -f "$PLIST"
  rm -rf "$APP"
  security delete-identity -c "$SIGN_NAME" "$KEYCHAIN" >/dev/null 2>&1 || true
  echo "제거했습니다."
  exit 0
fi

stop
# 서명이 바뀌었는지 보려고 (바뀌면 예전 허용은 듣지 않는다)
OLD_REQ=$(codesign -dr - "$APP" 2>/dev/null | grep designated || true)
pkill -f "LLM Deck Mirror.app/Contents/MacOS/" 2>/dev/null || true
rm -rf "$OLD_MIRROR_APP"
mkdir -p "$APP/Contents/MacOS" "$HOME/.llm-deck" "$HOME/Library/LaunchAgents"
cat > "$APP/Contents/Info.plist" <<EOF
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
if ! swiftc -O -swift-version 6 -o "$APP/Contents/MacOS/llm-deck" "$COMPANION/macos/app.swift"; then
  rm -rf "$APP"
  echo "앱을 만들지 못했습니다. xcode-select --install 로 Command Line Tools 를 깔고 다시 실행하세요."
  exit 1
fi
IDENTITY=$(signing_identity || true)
if [ -z "$IDENTITY" ] || ! codesign --force -s "$IDENTITY" --keychain "$KEYCHAIN" "$APP"; then
  codesign --force -s - "$APP"
  echo "주의: 서명용 인증서를 쓰지 못해 그냥 서명했습니다. 다시 설치할 때마다 마이크·블루투스 허용을 다시 눌러야 할 수 있습니다."
fi
NEW_REQ=$(codesign -dr - "$APP" 2>/dev/null | grep designated || true)
if [ -n "$OLD_REQ" ] && [ "$OLD_REQ" != "$NEW_REQ" ]; then
  # 예전 서명에 묶인 허용은 macOS 가 묻지도 않고 막는다. 지워서 다시 묻게 한다
  tccutil reset Microphone "$LABEL" >/dev/null 2>&1 || true
  tccutil reset BluetoothAlways "$LABEL" >/dev/null 2>&1 || true
  echo "앱 서명이 바뀌었습니다. 마이크·블루투스 허용 창이 다시 뜨면 \"허용\"을 누르세요."
fi

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/bin/open</string>
    <string>-W</string>
    <string>$APP</string>
    <string>--stdout</string><string>$LOG</string>
    <string>--stderr</string><string>$LOG</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>30</integer>
</dict>
</plist>
EOF
launchctl bootstrap "gui/$(id -u)" "$PLIST"

echo "설치했습니다. 기록: $LOG"
echo "미러링: 메뉴 막대의 LLM Deck 아이콘 > 미러링 열기 (또는 open -a 'LLM Deck')"
echo "잠시 멈추기: launchctl bootout gui/\$(id -u)/$LABEL && pkill -f 'LLM Deck.app/Contents/MacOS/llm-deck'"
