# LLM Deck PC 도우미 설치 (Windows)
#   install.cmd    설치하고 로그인할 때 자동으로 켜지게 한다 (다시 실행해도 된다)
#   uninstall.cmd  자동 실행과 Claude Code 상태줄 설정을 되돌린다
#   mirror.cmd     켜져 있는 도우미의 미러링 창(기기 화면)을 브라우저로 연다
param([switch]$Uninstall, [switch]$Mirror)
$ErrorActionPreference = 'Stop'
$Dir = Split-Path -Parent $PSScriptRoot
$RunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$Name = 'LLMDeck'
$LogFile = Join-Path $env:USERPROFILE '.llm-deck\companion.log'

function Find-Uv {
    $cmd = Get-Command uv -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $local = Join-Path $env:USERPROFILE '.local\bin\uv.exe'
    if (Test-Path $local) { return $local }
    return $null
}

# Wi-Fi 로 연결했을 때 기기가 PC 로 보내는 것(알림·사진 알림·마이크 소리는 UDP 4010, 동영상은 TCP 4011)을
# Windows 방화벽이 막지 않도록, 같은 공유기 안에서 오는 것만 허용하는 규칙을 둔다. 관리자 권한이 필요하다.
$FirewallScript = @'
Get-NetFirewallRule -DisplayName 'LLM Deck *' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
if ($args[0] -ne 'remove') {
    New-NetFirewallRule -DisplayName 'LLM Deck UDP 4010' -Direction Inbound -Protocol UDP -LocalPort 4010 -RemoteAddress LocalSubnet -Action Allow -Profile Any | Out-Null
    New-NetFirewallRule -DisplayName 'LLM Deck TCP 4011' -Direction Inbound -Protocol TCP -LocalPort 4011 -RemoteAddress LocalSubnet -Action Allow -Profile Any | Out-Null
}
'@

function Set-Firewall([string]$mode) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $file = Join-Path $env:TEMP 'llm-deck-firewall.ps1'
    Set-Content -Path $file -Value $FirewallScript -Encoding UTF8
    try {
        if ($admin) {
            & powershell -NoProfile -ExecutionPolicy Bypass -File $file $mode
        } else {
            # 관리자 확인 창(UAC)이 뜬다
            Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$file`"", $mode)
        }
        return $true
    } catch {
        return $false
    } finally {
        Remove-Item $file -ErrorAction SilentlyContinue
    }
}

function Stop-Companion {
    # 이 폴더의 도우미만 끈다
    Get-CimInstance Win32_Process |
        Where-Object { $_.CommandLine -and $_.CommandLine -like '*main.py*' -and ($_.CommandLine -like "*$Dir*" -or $_.CommandLine -like '*llm-deck*') } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Mirror) {
    $uv = Find-Uv
    if (-not $uv) { Write-Host '먼저 install.cmd 로 설치하세요.'; exit 1 }
    & $uv run --directory $Dir main.py --mirror
    exit $LASTEXITCODE
}

if ($Uninstall) {
    Stop-Companion
    Remove-ItemProperty -Path $RunKey -Name $Name -ErrorAction SilentlyContinue
    Set-Firewall 'remove' | Out-Null
    $uv = Find-Uv
    if ($uv -and (Test-Path (Join-Path $env:USERPROFILE '.claude'))) {
        & $uv run --directory $Dir main.py --uninstall-statusline
    }
    Write-Host '제거했습니다. 이 폴더는 지워도 됩니다.'
    return
}

Write-Host '[1/6] Python 도구(uv) 확인'
$uv = Find-Uv
if (-not $uv) {
    Write-Host '      uv 를 설치합니다 (https://astral.sh/uv)'
    Invoke-RestMethod https://astral.sh/uv/install.ps1 | Invoke-Expression
    $uv = Find-Uv
    if (-not $uv) { throw 'uv 설치에 실패했습니다. https://docs.astral.sh/uv/ 를 참고해 직접 설치한 뒤 다시 실행하세요.' }
}
$uvw = Join-Path (Split-Path $uv) 'uvw.exe'

Write-Host '[2/6] 도우미 준비 (처음에는 몇 분 걸릴 수 있습니다)'
& $uv sync --directory $Dir --quiet
if ($LASTEXITCODE -ne 0) { throw '도우미 준비에 실패했습니다.' }
# 음성 질문용 부품(faster-whisper)이 이 PC 에서 켜지는지 본다. 안 돼도 나머지 기능은 쓸 수 있다.
# Windows PowerShell 5.1 은 Stop 일 때 프로그램의 오류 출력만으로도 멈추므로, 이 확인 동안만 풀어 둔다
$ErrorActionPreference = 'Continue'
& $uv run --directory $Dir python -c 'import ctranslate2, onnxruntime, av, faster_whisper' 2>$null
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) {
    Write-Host '      주의: 음성 질문 부품을 켜지 못했습니다. "Microsoft Visual C++ 재배포 가능 패키지(x64)"를 설치한 뒤 다시 실행하세요.'
    Write-Host '            https://aka.ms/vs/17/release/vc_redist.x64.exe  (음성 질문 말고는 그대로 쓸 수 있습니다)'
}
$model = Join-Path $env:USERPROFILE '.llm-deck\models\faster-whisper-small\model.bin'
if (Test-Path $model) {
    Write-Host '      음성 인식 모델: 복사해 둔 폴더를 씁니다 (.llm-deck\models\faster-whisper-small)'
} else {
    Write-Host '      음성 인식 모델: 처음 질문할 때 인터넷으로 받습니다 (약 480MB). 회사망에서 막히면 README.txt 참고'
}

Write-Host '[3/6] 설정 파일'
$config = Join-Path $Dir 'config.toml'
if (-not (Test-Path $config)) {
    Copy-Item (Join-Path $Dir 'config.example.toml') $config
    Write-Host '      config.example.toml 을 config.toml 로 복사했습니다.'
} else {
    Write-Host '      기존 config.toml 을 그대로 씁니다.'
}

Write-Host '[4/6] Claude Code 한도 사용률 읽기'
if (Test-Path (Join-Path $env:USERPROFILE '.claude')) {
    & $uv run --directory $Dir main.py --install-statusline
} else {
    Write-Host '      이 PC 에는 Claude Code 가 없어 건너뜁니다. (다른 PC 값은 gist 로 받아 옵니다)'
}

Write-Host '[5/6] Windows 방화벽 (Wi-Fi 연결용, 관리자 확인 창이 뜨면 [예])'
if (Set-Firewall 'add') {
    $rules = @(Get-NetFirewallRule -DisplayName 'LLM Deck *' -ErrorAction SilentlyContinue)
    Write-Host "      허용 규칙 $($rules.Count)개를 두었습니다."
} else {
    Write-Host '      건너뜁니다. USB·블루투스는 상관없고, Wi-Fi 로 쓸 때 사진·동영상·알림이 안 올 수 있습니다.'
}
# 예전에 방화벽 창에서 [취소]를 누르면 python.exe 를 막는 규칙이 생긴다. 막는 규칙이 허용보다 앞선다.
$blocked = Get-NetFirewallRule -Direction Inbound -Action Block -Enabled True -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -like '*python*' }
if ($blocked) {
    Write-Host '      주의: python 을 막는 방화벽 규칙이 있습니다. Wi-Fi 로 쓸 때 문제가 되면'
    Write-Host '            Windows 보안 > 방화벽 및 네트워크 보호 > 고급 설정 > 인바운드 규칙에서 지우세요.'
}

Write-Host '[6/6] 로그인할 때 자동 실행'
Stop-Companion
# 예전 방식(공유만 하는 도우미)이 등록되어 있으면 지운다. 이제 이 도우미가 공유도 함께 한다.
Remove-ItemProperty -Path $RunKey -Name 'LLMDeckSync' -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Split-Path $LogFile) | Out-Null
$command = "`"$uvw`" run --directory `"$Dir`" main.py --log `"$LogFile`""
Set-ItemProperty -Path $RunKey -Name $Name -Value $command
Start-Process -FilePath $uvw -ArgumentList @('run', '--directory', "`"$Dir`"", 'main.py', '--log', "`"$LogFile`"") -WindowStyle Hidden

Write-Host ''
Write-Host '설치했습니다. 도우미가 창 없이 실행 중입니다.'
Write-Host "기록: $LogFile"
Write-Host '기기 연결: USB 케이블, 블루투스(LLM-Deck), 같은 Wi-Fi 중 되는 것으로 자동 연결합니다.'
