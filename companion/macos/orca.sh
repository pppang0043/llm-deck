#!/bin/sh
# LLM Deck orca shim
# 터미널에서 쓰는 orca 명령. install.sh 가 ~/.local/bin/orca 로 깐다.
# ORCA 가 직접 거는 링크는 앱을 처음 연 자리를 가리켜, 다운로드한 자리에서 연 앱(macOS 가 임시 경로로 옮겨 실행)이면
# 재부팅 뒤 끊어진다. 그래서 고정 경로 대신 부를 때마다 ORCA 앱을 찾는다.
for app in /Applications/Orca.app "$HOME/Applications/Orca.app"; do
  [ -x "$app/Contents/Resources/bin/orca" ] && exec "$app/Contents/Resources/bin/orca" "$@"
done
app=$(mdfind "kMDItemCFBundleIdentifier == 'com.stablyai.orca'" 2>/dev/null | grep -v AppTranslocation | head -n1)
[ -n "$app" ] && [ -x "$app/Contents/Resources/bin/orca" ] && exec "$app/Contents/Resources/bin/orca" "$@"
echo "orca: Orca.app 을 찾지 못했습니다. ORCA 를 /Applications 에 옮겨 두세요." >&2
exit 127
