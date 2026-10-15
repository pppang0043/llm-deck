#!/bin/sh
# Mac 에서 PC 도우미를 로그인할 때 자동으로 실행되게 한다.
#   sh macos/install.sh            설치 (다시 실행하면 새로 만든다)
#   sh macos/install.sh uninstall  제거
#
# 블루투스 권한은 "앱" 단위로만 받을 수 있어서, 도우미를 작은 앱(~/Applications/LLM Deck.app)으로 감싸
# open 으로 띄운다. 처음 실행할 때 블루투스 허용 창이 뜨면 "허용"을 누른다.
set -e
COMPANION=$(cd "$(dirname "$0")/.." && pwd)
APP="$HOME/Applications/LLM Deck.app"
LABEL=com.llmdeck.companion
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/.llm-deck/companion.log"

stop() {
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  pkill -f "LLM Deck.app/Contents/MacOS/llm-deck" 2>/dev/null || true
}

if [ "$1" = "uninstall" ]; then
  stop
  rm -f "$PLIST"
  rm -rf "$APP"
  echo "제거했습니다."
  exit 0
fi

stop
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
  <key>NSBluetoothAlwaysUsageDescription</key><string>LLM Deck 기기와 블루투스로 연결합니다.</string>
</dict>
</plist>
EOF
cc -O2 -DCOMPANION_DIR="\"$COMPANION\"" -o "$APP/Contents/MacOS/llm-deck" "$COMPANION/macos/launcher.c"
codesign --force -s - "$APP"

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
echo "잠시 멈추기: launchctl bootout gui/\$(id -u)/$LABEL && pkill -f 'LLM Deck.app'"
