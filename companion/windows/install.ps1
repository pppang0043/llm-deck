# LLM Deck PC 도우미 설치 (Windows)
#   install.cmd    설치하고 로그인할 때 자동으로 켜지게 한다 (다시 실행해도 된다)
#   uninstall.cmd  자동 실행과 Claude Code 상태줄 설정을 되돌린다
#   mirror.cmd     켜져 있는 도우미의 미러링 창(기기 화면)을 연다
#
# 이 PC 에 깔린 Python(3.11~3.14)으로 app\.venv 를 만들고 pip 로 부품을 깐다.
# 예전에는 uv 가 Python 까지 받아 깔았지만, 회사 보안이 uv 를 막아 이 PC 의 Python 과 pip 만 쓴다.
# 부품 목록은 requirements.txt (버전과 파일 지문을 고정해 둔 것. Mac 에서 make_zip.py 가 uv.lock 으로 만든다)
param([switch]$Uninstall, [switch]$Mirror)
$ErrorActionPreference = 'Stop'
$Dir = Split-Path -Parent $PSScriptRoot
$RunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$Name = 'LLMDeck'
$LogFile = Join-Path $env:USERPROFILE '.llm-deck\companion.log'
$Venv = Join-Path $Dir '.venv'
$VenvPy = Join-Path $Venv 'Scripts\python.exe'
$VenvPyw = Join-Path $Venv 'Scripts\pythonw.exe'
# 도우미 앱: 트레이 아이콘을 두고 도우미를 띄우며, 거기서 미러링 창을 연다 (Python 재단이 서명한 pythonw.exe 로 돌린다)
$AppPyw = Join-Path $PSScriptRoot 'app.pyw'
$AppIcon = Join-Path $PSScriptRoot 'llm-deck.ico'
$Programs = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$AppLink = Join-Path $Programs 'LLM Deck.lnk'
$MinPython = [version]'3.11'
$MaxPython = [version]'3.14'
# 예전에 쓰던 것들 (설치 때 컴파일한 exe, uv 설정). 보이면 지운다
$OldAppExe = Join-Path $PSScriptRoot 'LLM Deck.exe'
$OldMirrorExe = Join-Path $PSScriptRoot 'LLM Deck Mirror.exe'
$OldMirrorLink = Join-Path $Programs 'LLM Deck 미러링.lnk'
$OldHelperKey = 'HKCU:\Software\LLMDeck\Helper'

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

# config.toml 의 한 칸([storage] 등) 값. 파이썬이 준비되기 전에 써야 해서 줄 단위로 간단히 읽는다
function Read-Section([string]$path, [string]$section) {
    $values = @{}
    if (-not (Test-Path $path)) { return $values }
    $inside = $false
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        $t = $line.Trim()
        if ($t -match '^\[([^\]]+)\]') { $inside = $Matches[1].Trim() -eq $section; continue }
        if (-not $inside) { continue }
        if ($t -match "^(\w+)\s*=\s*'([^']*)'") { $values[$Matches[1]] = $Matches[2] }
        elseif ($t -match '^(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"') { $values[$Matches[1]] = $Matches[2] -replace '\\\\', '\' }
        elseif ($t -match '^(\w+)\s*=\s*(true|false)\b') { $values[$Matches[1]] = $Matches[2] -eq 'true' }
    }
    return $values
}

# 비어 있으면 $null. %USERPROFILE% 같은 것은 풀어 준다
function Get-StoragePath($values, [string]$name) {
    $v = $values[$name]
    if (-not ($v -is [string]) -or -not $v.Trim()) { return $null }
    $path = [Environment]::ExpandEnvironmentVariables($v.Trim()).TrimEnd('\')
    if ($path.EndsWith(':')) { $path += '\' }  # D:\ 처럼 드라이브만 적었을 때
    # 없는 드라이브(빼 둔 USB 등)나 C:\ 로 시작하지 않는 경로면 무엇이 틀렸는지 알려 주고 멈춘다
    $root = [IO.Path]::GetPathRoot($path)
    if (-not $root -or -not (Test-Path -LiteralPath $root)) {
        throw "config.toml [storage] 의 $name 를 쓸 수 없습니다: $path  (D:\폴더 처럼 드라이브부터 적고, 그 드라이브가 있는지 확인하세요)"
    }
    return $path
}

# 회사망 설정([network])을 이번 설치 동안만 적용한다. pip 는 이 환경 변수들을 읽는다
function Use-Network($values) {
    $proxy = $values['proxy']
    if ($proxy -is [string] -and $proxy.Trim()) {
        $proxy = $proxy.Trim()
        if ($proxy -notmatch '^https?://') { throw "config.toml [network] 의 proxy 는 http:// 로 시작해야 합니다: $proxy" }
        $env:HTTPS_PROXY = $proxy
        $env:HTTP_PROXY = $proxy
        $env:NO_PROXY = 'localhost,127.0.0.1'
        Write-Host "      회사망: 프록시 $proxy"
    }
    $index = $values['index_url']
    if ($index -is [string] -and $index.Trim()) {
        if ($index.Trim() -notmatch '^https?://') { throw "config.toml [network] 의 index_url 은 https:// 로 시작해야 합니다: $index" }
        $env:PIP_INDEX_URL = $index.Trim()
        Write-Host "      회사망: 부품 저장소 $($env:PIP_INDEX_URL)"
    }
    if ($values['system_certs'] -eq $true) {
        # 보안 프로그램이 연결을 검사하는 회사: Windows 에 깔린 (회사가 넣은) 인증서를 파일로 꺼내 pip 에 준다
        $pem = Join-Path $Dir '.windows-certs.pem'
        $text = New-Object Text.StringBuilder
        foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\CurrentUser\Root', 'Cert:\LocalMachine\CA') {
            foreach ($cert in @(Get-ChildItem $store -ErrorAction SilentlyContinue)) {
                [void]$text.Append("-----BEGIN CERTIFICATE-----`n")
                [void]$text.Append([Convert]::ToBase64String($cert.RawData, 'InsertLineBreaks'))
                [void]$text.Append("`n-----END CERTIFICATE-----`n")
            }
        }
        [IO.File]::WriteAllText($pem, $text.ToString(), (New-Object Text.UTF8Encoding $false))
        $env:PIP_CERT = $pem
        Write-Host '      회사망: Windows 에 깔린 회사 인증서를 믿습니다'
    }
}

# 프로그램을 돌려 출력을 받는다. Windows PowerShell 5.1 은 Stop 일 때 프로그램의 오류 출력만으로도 멈추므로 이 안에서만 풀어 둔다
function Invoke-Quiet([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $exe @arguments 2>$null
        return @{ Ok = ($LASTEXITCODE -eq 0); Out = @($out) }
    } catch {
        return @{ Ok = $false; Out = @() }
    }
}

# Python 하나를 돌려 버전을 본다. 64비트 Python 만 쓴다 (부품이 64비트용이다)
function Get-PythonInfo([string]$exe) {
    if (-not $exe -or -not (Test-Path -LiteralPath $exe -PathType Leaf)) { return $null }
    $r = Invoke-Quiet $exe @('-c', "import sys; print('%d.%d.%d' % sys.version_info[:3]); print(sys.maxsize > 2**32); print(sys.executable)")
    if (-not $r.Ok -or $r.Out.Count -lt 3) { return $null }
    return @{ Version = [version]$r.Out[0].Trim(); Bits64 = $r.Out[1].Trim() -eq 'True'; Exe = $r.Out[2].Trim() }
}

# 이 PC 에 깔린 Python 을 모두 찾는다 (앞에 있을수록 먼저 고른다)
function Find-PythonCandidates($storage) {
    $list = New-Object Collections.Generic.List[string]
    # config.toml [storage] python 에 적은 것 (python.exe 나 그 폴더)
    foreach ($key in 'python', 'python_dir') {
        $v = Get-StoragePath $storage $key
        if (-not $v) { continue }
        if (Test-Path -LiteralPath $v -PathType Leaf) { $list.Add($v) }
        else {
            $list.Add((Join-Path $v 'python.exe'))
            # 예전 python_dir 는 uv 가 Python 을 받아 두던 곳이라 한 단계 아래에 있다
            Get-ChildItem -LiteralPath $v -Directory -ErrorAction SilentlyContinue | ForEach-Object { $list.Add((Join-Path $_.FullName 'python.exe')) }
        }
    }
    # Python 실행기(py.exe)가 아는 것
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        foreach ($line in (Invoke-Quiet $py.Source @('-0p')).Out) {
            if ("$line" -match '([A-Za-z]:\\.*?\.exe)\s*$') { $list.Add($Matches[1]) }
        }
    }
    # PATH 에 있는 것 (Microsoft Store 의 바로 가기는 Python 이 없으면 실행되지 않아 걸러진다)
    foreach ($cmd in @(Get-Command python -All -ErrorAction SilentlyContinue)) { $list.Add($cmd.Source) }
    # python.org 설치 프로그램이 적어 두는 곳
    foreach ($root in 'HKCU:\Software\Python\PythonCore', 'HKLM:\Software\Python\PythonCore', 'HKLM:\Software\WOW6432Node\Python\PythonCore') {
        foreach ($k in @(Get-ChildItem $root -ErrorAction SilentlyContinue)) {
            $ip = Get-ItemProperty (Join-Path $k.PSPath 'InstallPath') -ErrorAction SilentlyContinue
            if (-not $ip) { continue }
            if ($ip.ExecutablePath) { $list.Add($ip.ExecutablePath) }
            elseif ($ip.'(default)') { $list.Add((Join-Path $ip.'(default)' 'python.exe')) }
        }
    }
    foreach ($base in (Join-Path $env:LOCALAPPDATA 'Programs\Python'), $env:ProgramFiles) {
        if ($base) { Get-ChildItem -Path $base -Filter 'Python3*' -Directory -ErrorAction SilentlyContinue | ForEach-Object { $list.Add((Join-Path $_.FullName 'python.exe')) } }
    }
    # 예전 설치 때 uv 가 받아 둔 Python (다른 것이 없을 때만)
    Get-ChildItem -Path (Join-Path $env:APPDATA 'uv\python') -Directory -ErrorAction SilentlyContinue | ForEach-Object { $list.Add((Join-Path $_.FullName 'python.exe')) }
    return $list
}

# 3.11~3.14 중 가장 새 것을 고른다. 없으면 무엇이 있었는지 알려 주고 멈춘다
function Select-Python($storage) {
    $seen = @{}
    $best = $null
    $others = @()
    foreach ($exe in (Find-PythonCandidates $storage)) {
        $info = Get-PythonInfo $exe
        if (-not $info -or $seen.ContainsKey($info.Exe.ToLower())) { continue }
        $seen[$info.Exe.ToLower()] = $true
        $minor = [version]("{0}.{1}" -f $info.Version.Major, $info.Version.Minor)
        if (-not $info.Bits64 -or $minor -lt $MinPython -or $minor -gt $MaxPython) {
            $others += "$($info.Version)$(if (-not $info.Bits64) { ' (32비트)' }) $($info.Exe)"
            continue
        }
        if (-not $best -or $info.Version -gt $best.Version) { $best = $info }
    }
    if ($best) { return $best }
    Write-Host '      쓸 수 있는 Python 을 찾지 못했습니다. (64비트 Python 3.11 ~ 3.14 가 필요합니다)'
    foreach ($o in $others) { Write-Host "      맞지 않는 버전: $o" }
    Write-Host '      Python 을 설치한 뒤 install.cmd 를 다시 실행하세요. (INSTALL.txt 의 "1. 준비")'
    Write-Host '        - 회사 PC: 회사 소프트웨어 센터(포털)에서 Python 3.12 또는 3.13 설치'
    Write-Host '        - 개인 PC: https://www.python.org/downloads/windows/ 의 "Windows installer (64-bit)"'
    Write-Host '                   설치 화면의 "tcl/tk" 와 "py launcher" 는 켜 둡니다 (기본값)'
    Write-Host '        - 깔려 있는데 못 찾으면 config.toml [storage] 에 python = ''C:\경로\python.exe'' 로 적습니다'
    throw 'Python 이 필요합니다.'
}

function Stop-App {
    # 트레이 앱을 끄면 Windows 가 앱이 띄운 도우미도 같이 끈다. 예전 exe 로 된 앱도 끈다
    $apps = @(Get-Process -Name 'LLM Deck', 'LLM Deck Mirror' -ErrorAction SilentlyContinue)
    $apps += @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine -like '*app.pyw*' -and $_.CommandLine -like "*$PSScriptRoot*" } |
        ForEach-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue })
    if (-not $apps) { return }
    $apps | Stop-Process -Force -ErrorAction SilentlyContinue
    # 다 꺼져야 .venv 를 지우거나 새로 쓸 수 있다
    $apps | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
}

function Stop-Companion {
    Stop-App
    # 앱 없이 켠 도우미(예전 방식이나 앱을 쓸 수 없을 때)도 끈다. 이 폴더의 도우미만
    Get-CimInstance Win32_Process |
        Where-Object { $_.CommandLine -and $_.CommandLine -like '*main.py*' -and ($_.CommandLine -like "*$Dir*" -or $_.CommandLine -like '*llm-deck*') } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

# 예전 설치가 남긴 것 (설치 때 컴파일한 exe, uv 설정)
function Remove-Old {
    Remove-Item $OldAppExe, $OldMirrorExe, $OldMirrorLink, (Join-Path $Dir 'uv.toml') -Force -ErrorAction SilentlyContinue
    Remove-Item $OldHelperKey -Recurse -ErrorAction SilentlyContinue
}

# 도우미 폴더에서 Python 으로 main.py 를 돌린다 (설정을 고치는 일들)
function Invoke-Main([string[]]$arguments) {
    Push-Location $Dir
    try { & $VenvPy (Join-Path $Dir 'main.py') @arguments } finally { Pop-Location }
}

if ($Mirror) {
    # 도우미 앱이 떠 있으면 그 앱이 창을 열고, 없으면 창만 띄운다. 도우미가 꺼져 있으면 창에 안내가 뜬다
    if ((Test-Path $VenvPyw) -and (Test-Path $AppPyw)) {
        Start-Process -FilePath $VenvPyw -ArgumentList @("`"$AppPyw`"", '--mirror') -WorkingDirectory $Dir
        exit 0
    }
    Write-Host '먼저 install.cmd 로 설치하세요.'
    exit 1
}

if ($Uninstall) {
    Stop-Companion
    Remove-Old
    Remove-Item $AppLink, $AppIcon -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\LLMDeck' -Recurse -ErrorAction SilentlyContinue  # 미러링 창의 항상 위·배율·위치
    Remove-ItemProperty -Path $RunKey -Name $Name -ErrorAction SilentlyContinue
    Set-Firewall 'remove' | Out-Null
    if ((Test-Path $VenvPy) -and (Test-Path (Join-Path $env:USERPROFILE '.claude'))) {
        Invoke-Main @('--uninstall-statusline')
    }
    Write-Host '제거했습니다. 이 폴더는 지워도 됩니다.'
    return
}

Write-Host '[1/7] 설정 파일과 저장 위치'
$config = Join-Path $Dir 'config.toml'
if (-not (Test-Path $config)) {
    Copy-Item (Join-Path $Dir 'config.example.toml') $config
    Write-Host '      config.example.toml 을 config.toml 로 복사했습니다.'
} else {
    Write-Host '      기존 config.toml 을 그대로 씁니다.'
    # 예전 설정에는 저장 위치·회사망 칸이 없다. 예시 파일 끝의 그 부분을 그대로 붙여 둔다
    $example = [IO.File]::ReadAllText((Join-Path $Dir 'config.example.toml'))
    $marks = @(@('storage', '# 저장 위치:'), @('network', '# 회사망:'))
    foreach ($m in $marks) {
        if (Select-String -Path $config -Pattern "^\s*\[$($m[0])\]" -Quiet) { continue }
        $at = $example.IndexOf($m[1])
        if ($at -lt 0) { continue }
        # 다음 칸의 설명이 나오기 전까지
        $end = $example.Length
        foreach ($other in $marks) {
            $next = $example.IndexOf($other[1], $at + 1)
            if ($next -gt $at -and $next -lt $end) { $end = $next }
        }
        $block = "`r`n" + ($example.Substring($at, $end - $at).TrimEnd() -replace "`r?`n", "`r`n") + "`r`n"
        [IO.File]::AppendAllText($config, $block, (New-Object Text.UTF8Encoding $false))
        Write-Host "      [$($m[0])] 칸을 config.toml 끝에 더했습니다. (INSTALL.txt 참고)"
    }
}
$network = Read-Section $config 'network'
Use-Network $network
# 용량이 적은 PC 에서 큰 것을 다른 드라이브에 둘 때
$storage = Read-Section $config 'storage'
$modelDir = Get-StoragePath $storage 'model_dir'
$cacheDir = Get-StoragePath $storage 'cache_dir'
if (-not $cacheDir) { $cacheDir = Get-StoragePath $storage 'uv_cache_dir' }  # 예전 이름
if ($cacheDir) {
    New-Item -ItemType Directory -Force $cacheDir | Out-Null
    $env:PIP_CACHE_DIR = $cacheDir
    Write-Host "      설치 부품 보관함: $cacheDir"
}
if ($modelDir) {
    New-Item -ItemType Directory -Force $modelDir | Out-Null
}

Write-Host '[2/7] Python 찾기'
$python = Select-Python $storage
Write-Host "      Python $($python.Version)  $($python.Exe)"

Write-Host '[3/7] 도우미 준비 (처음에는 몇 분 걸릴 수 있습니다)'
# 예전에 uv 로 만든 .venv(pip 가 없다)나 다른 Python 으로 만든 것은 새로 만든다
$venvCfg = Join-Path $Venv 'pyvenv.cfg'
$remake = $false
if (Test-Path $venvCfg) {
    $cfgText = [IO.File]::ReadAllText($venvCfg)
    $venvHome = if ($cfgText -match '(?m)^home\s*=\s*(.+?)\s*$') { $Matches[1] } else { '' }
    if ($cfgText -match '(?m)^uv\s*=' -or $venvHome.TrimEnd('\') -ne (Split-Path $python.Exe).TrimEnd('\') -or -not (Get-PythonInfo $VenvPy)) { $remake = $true }
} elseif (Test-Path $Venv) {
    $remake = $true
}
if ($remake) {
    Stop-Companion
    Remove-Item $Venv -Recurse -Force
    Write-Host '      예전 도우미 부품을 지우고 새로 만듭니다.'
}
if (-not (Test-Path $VenvPy)) {
    $r = Invoke-Quiet $python.Exe @('-m', 'venv', $Venv)
    if (-not $r.Ok -or -not (Test-Path $VenvPy)) { throw "도우미 부품 폴더(.venv)를 만들지 못했습니다. ($($python.Exe) -m venv)" }
}
# 버전과 파일 지문이 고정된 목록대로, 미리 만들어진 파일(wheel)만 받는다 (이 PC 에서 컴파일하지 않는다)
$ErrorActionPreference = 'Continue'
& $VenvPy -m pip install --disable-pip-version-check --no-input --quiet --only-binary ':all:' --require-hashes -r (Join-Path $Dir 'requirements.txt')
$pipOk = $LASTEXITCODE -eq 0
$ErrorActionPreference = 'Stop'
if (-not $pipOk) { throw '도우미 준비에 실패했습니다. 위 오류 문구를 보고 INSTALL.txt 의 "9. 회사 PC" 를 확인하세요.' }
# 고친 config.toml 에 틀린 곳이 없는지 본다 (틀리면 도우미가 켜지지 않는다)
$check = Invoke-Quiet $VenvPy @('-c', "import tomllib, pathlib, sys; tomllib.loads(pathlib.Path(sys.argv[1]).read_bytes().decode('utf-8-sig'))", $config)
if (-not $check.Ok) {
    $bad = Invoke-Quiet $VenvPy @('-c', "import tomllib, pathlib, sys`ntry: tomllib.loads(pathlib.Path(sys.argv[1]).read_bytes().decode('utf-8-sig'))`nexcept Exception as e: print(e)", $config)
    Write-Host "      config.toml 에 틀린 곳이 있습니다: $($bad.Out | Select-Object -Last 1)"
    Write-Host "      Windows 경로는 작은따옴표로 적으세요. 예: model_dir = 'D:\LLM Deck\models'"
    throw 'config.toml 을 고친 뒤 다시 실행하세요.'
}
# 음성 질문용 부품(faster-whisper)이 이 PC 에서 켜지는지 본다. 안 돼도 나머지 기능은 쓸 수 있다.
if (-not (Invoke-Quiet $VenvPy @('-c', 'import ctranslate2, onnxruntime, av, faster_whisper')).Ok) {
    Write-Host '      주의: 음성 질문 부품을 켜지 못했습니다. "Microsoft Visual C++ 재배포 가능 패키지(x64)"를 설치한 뒤 다시 실행하세요.'
    Write-Host '            https://aka.ms/vs/17/release/vc_redist.x64.exe  (음성 질문 말고는 그대로 쓸 수 있습니다)'
}
$modelRoot = if ($modelDir) { $modelDir } else { Join-Path $env:USERPROFILE '.llm-deck\models' }
$whisper = (Read-Section $config 'assistant')['whisper_model']
if (-not ($whisper -is [string]) -or -not $whisper) { $whisper = 'small' }
# whisper_model 에 폴더 경로를 적었으면 그 폴더를 쓴다 (assistant.py). 없는 드라이브여도 멈추지 않게 글자로만 잇는다
$model = if ($whisper -match '[\\/:]') { [IO.Path]::Combine($whisper, 'model.bin') } else { [IO.Path]::Combine($modelRoot, "faster-whisper-$whisper", 'model.bin') }
if (Test-Path $model) {
    Write-Host "      음성 인식 모델: 복사해 둔 폴더를 씁니다 ($(Split-Path $model))"
} else {
    $where = if ($modelDir) { $modelDir } else { Join-Path $env:USERPROFILE '.cache\huggingface' }
    Write-Host "      음성 인식 모델($whisper): 처음 말로 질문할 때 인터넷으로 받습니다 ($where). 회사망에서 막히면 INSTALL.txt 의 5 참고"
}

Write-Host '[4/7] Claude Code 한도 사용률 읽기'
if (Test-Path (Join-Path $env:USERPROFILE '.claude')) {
    Invoke-Main @('--install-statusline')
} else {
    Write-Host '      이 PC 에는 Claude Code 가 없어 건너뜁니다. (다른 PC 값은 gist 로 받아 옵니다)'
}

Write-Host '[5/7] Windows 방화벽 (Wi-Fi 연결용, 관리자 확인 창이 뜨면 [예])'
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

Write-Host '[6/7] 도우미 앱 (트레이 아이콘, 미러링 창)'
Stop-Companion
Remove-Old
$appOk = $false
if (-not (Invoke-Quiet $VenvPy @('-c', 'import tkinter; tkinter.Tcl()')).Ok) {
    Write-Host '      건너뜁니다: 이 Python 에 창을 그리는 부품(tcl/tk)이 없습니다.'
    Write-Host '      Python 설치 프로그램을 다시 열어 [Modify] > "tcl/tk and IDLE" 를 켜고 install.cmd 를 다시 실행하세요.'
    Write-Host '      그 전까지 도우미는 트레이 아이콘 없이 돌고, mirror.cmd 를 누르면 브라우저로 엽니다 (http://127.0.0.1:4013/).'
} elseif (-not (Invoke-Quiet $VenvPy @('-c', 'import pystray, PIL')).Ok) {
    Write-Host '      건너뜁니다: 트레이 아이콘 부품(pystray)을 켜지 못했습니다. 도우미는 트레이 아이콘 없이 돕니다.'
} else {
    Invoke-Quiet $VenvPy @($AppPyw, '--write-icon', $AppIcon) | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($AppLink)
    $link.TargetPath = $VenvPyw
    $link.Arguments = "`"$AppPyw`""
    $link.WorkingDirectory = $Dir
    if (Test-Path $AppIcon) { $link.IconLocation = $AppIcon }
    $link.Description = 'LLM Deck 도우미를 켜고 기기 화면(미러링)을 연다'
    $link.Save()
    $appOk = $true
    Write-Host '      작업 표시줄 오른쪽 아래의 LLM Deck 아이콘이나 시작 메뉴의 "LLM Deck" 으로 미러링 창을 엽니다.'
}

Write-Host '[7/7] 로그인할 때 자동 실행'
# 도우미를 끈 뒤에 비운다 (켜져 있으면 쓰는 중인 파일이 있어 지우지 못할 수 있다)
if ($storage['clean_cache'] -eq $true) {
    Invoke-Quiet $VenvPy @('-m', 'pip', 'cache', 'purge') | Out-Null
    Write-Host '      설치 부품 보관함을 비웠습니다 (clean_cache = true)'
}
# 예전 방식(공유만 하는 도우미)이 등록되어 있으면 지운다. 이제 이 도우미가 공유도 함께 한다.
Remove-ItemProperty -Path $RunKey -Name 'LLMDeckSync' -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Split-Path $LogFile) | Out-Null
if ($appOk) {
    # 앱이 도우미를 띄운다. 로그인할 때는 미러링 창 없이 트레이 아이콘만
    Set-ItemProperty -Path $RunKey -Name $Name -Value "`"$VenvPyw`" `"$AppPyw`" --background"
    Start-Process -FilePath $VenvPyw -ArgumentList @("`"$AppPyw`"", '--background') -WorkingDirectory $Dir
} else {
    $mainPy = Join-Path $Dir 'main.py'
    Set-ItemProperty -Path $RunKey -Name $Name -Value "`"$VenvPyw`" `"$mainPy`" --log `"$LogFile`""
    Start-Process -FilePath $VenvPyw -ArgumentList @("`"$mainPy`"", '--log', "`"$LogFile`"") -WorkingDirectory $Dir
}

Write-Host ''
if ($appOk) {
    Write-Host '설치했습니다. 도우미가 실행 중입니다 (작업 표시줄 오른쪽 아래 LLM Deck 아이콘).'
} else {
    Write-Host '설치했습니다. 도우미가 창 없이 실행 중입니다.'
}
Write-Host "기록: $LogFile"
Write-Host '기기 연결: USB 케이블, 블루투스(LLM-Deck), 같은 Wi-Fi 중 되는 것으로 자동 연결합니다.'
