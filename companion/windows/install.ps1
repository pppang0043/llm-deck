# LLM Deck PC 도우미 설치 (Windows)
#   install.cmd    설치하고 로그인할 때 자동으로 켜지게 한다 (다시 실행해도 된다)
#   uninstall.cmd  자동 실행과 Claude Code 상태줄 설정을 되돌린다
param([switch]$Uninstall)
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

function Stop-Companion {
    # 이 폴더의 도우미만 끈다
    Get-CimInstance Win32_Process |
        Where-Object { $_.CommandLine -and $_.CommandLine -like '*main.py*' -and ($_.CommandLine -like "*$Dir*" -or $_.CommandLine -like '*llm-deck*') } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Uninstall) {
    Stop-Companion
    Remove-ItemProperty -Path $RunKey -Name $Name -ErrorAction SilentlyContinue
    $uv = Find-Uv
    if ($uv -and (Test-Path (Join-Path $env:USERPROFILE '.claude'))) {
        & $uv run --directory $Dir main.py --uninstall-statusline
    }
    Write-Host '제거했습니다. 이 폴더는 지워도 됩니다.'
    return
}

Write-Host '[1/5] Python 도구(uv) 확인'
$uv = Find-Uv
if (-not $uv) {
    Write-Host '      uv 를 설치합니다 (https://astral.sh/uv)'
    Invoke-RestMethod https://astral.sh/uv/install.ps1 | Invoke-Expression
    $uv = Find-Uv
    if (-not $uv) { throw 'uv 설치에 실패했습니다. https://docs.astral.sh/uv/ 를 참고해 직접 설치한 뒤 다시 실행하세요.' }
}
$uvw = Join-Path (Split-Path $uv) 'uvw.exe'

Write-Host '[2/5] 도우미 준비 (처음에는 몇 분 걸릴 수 있습니다)'
& $uv sync --directory $Dir --quiet
if ($LASTEXITCODE -ne 0) { throw '도우미 준비에 실패했습니다.' }

Write-Host '[3/5] 설정 파일'
$config = Join-Path $Dir 'config.toml'
if (-not (Test-Path $config)) {
    Copy-Item (Join-Path $Dir 'config.example.toml') $config
    Write-Host '      config.example.toml 을 config.toml 로 복사했습니다.'
} else {
    Write-Host '      기존 config.toml 을 그대로 씁니다.'
}

Write-Host '[4/5] Claude Code 한도 사용률 읽기'
if (Test-Path (Join-Path $env:USERPROFILE '.claude')) {
    & $uv run --directory $Dir main.py --install-statusline
} else {
    Write-Host '      이 PC 에는 Claude Code 가 없어 건너뜁니다. (다른 PC 값은 gist 로 받아 옵니다)'
}

Write-Host '[5/5] 로그인할 때 자동 실행'
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
