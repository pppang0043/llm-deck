# LLM Deck PC 도우미 설치 (Windows)
#   install.cmd    설치하고 로그인할 때 자동으로 켜지게 한다 (다시 실행해도 된다)
#   uninstall.cmd  자동 실행과 Claude Code 상태줄 설정을 되돌린다
#   mirror.cmd     켜져 있는 도우미의 미러링 창(기기 화면)을 연다. 도우미 앱이 없으면 브라우저로
param([switch]$Uninstall, [switch]$Mirror)
$ErrorActionPreference = 'Stop'
$Dir = Split-Path -Parent $PSScriptRoot
$RunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$Name = 'LLMDeck'
$LogFile = Join-Path $env:USERPROFILE '.llm-deck\companion.log'
# 도우미 앱: 트레이 아이콘을 두고 도우미를 띄우며, 거기서 미러링 창을 연다 (설치할 때 app.cs 를 컴파일해 만든다)
$AppExe = Join-Path $PSScriptRoot 'LLM Deck.exe'
$Programs = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$AppLink = Join-Path $Programs 'LLM Deck.lnk'
$HelperKey = 'HKCU:\Software\LLMDeck\Helper'  # 앱이 도우미를 띄우는 방법 (uvw.exe, app 폴더, 기록 파일)
# 예전에 따로 만들던 미러링 앱
$OldMirrorExe = Join-Path $PSScriptRoot 'LLM Deck Mirror.exe'
$OldMirrorLink = Join-Path $Programs 'LLM Deck 미러링.lnk'

function Find-Uv {
    # 회사 보안 때문에 uv 를 저절로 깔 수 없으면, 직접 받은 uv.exe(와 uvw.exe)를 install.cmd 옆이나 app 폴더에 둔다 (INSTALL.txt)
    foreach ($near in (Join-Path (Split-Path -Parent $Dir) 'uv.exe'), (Join-Path $Dir 'uv.exe')) {
        if (Test-Path $near) { return $near }
    }
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

# 회사망 설정([network])을 이번 설치 동안만 적용한다. uv 가 모르는 설정을 uv.toml 에 적으면 uv 가 멈추므로 환경 변수로만 넘긴다
function Use-Network($values) {
    $proxy = $values['proxy']
    if ($proxy -is [string] -and $proxy.Trim()) {
        $proxy = $proxy.Trim()
        if ($proxy -notmatch '^https?://') { throw "config.toml [network] 의 proxy 는 http:// 로 시작해야 합니다: $proxy" }
        $env:HTTPS_PROXY = $proxy
        $env:HTTP_PROXY = $proxy
        $env:NO_PROXY = 'localhost,127.0.0.1'
        # uv 를 받는 Invoke-RestMethod 도 같은 프록시로 (이 PC 안 주소는 거치지 않는다)
        [Net.WebRequest]::DefaultWebProxy = New-Object Net.WebProxy($proxy, $true)
        Write-Host "      회사망: 프록시 $proxy"
    }
    $index = $values['index_url']
    if ($index -is [string] -and $index.Trim()) {
        if ($index.Trim() -notmatch '^https?://') { throw "config.toml [network] 의 index_url 은 https:// 로 시작해야 합니다: $index" }
        $env:UV_INDEX_URL = $index.Trim()
        Write-Host "      회사망: 부품 저장소 $($env:UV_INDEX_URL)"
    }
    $mirror = $values['python_mirror']
    if ($mirror -is [string] -and $mirror.Trim()) {
        $env:UV_PYTHON_INSTALL_MIRROR = $mirror.Trim()
        Write-Host "      회사망: Python 받는 곳 $($env:UV_PYTHON_INSTALL_MIRROR)"
    }
}

# 회사 인증서 믿기. 새 uv 는 UV_SYSTEM_CERTS, 예전 uv 는 UV_NATIVE_TLS 만 알아서 도움말에 있는 것을 고른다
function Use-SystemCerts([string]$uv) {
    $help = (& $uv sync --help) | Out-String
    if ($help -match 'system-certs') { $env:UV_SYSTEM_CERTS = 'true' } else { $env:UV_NATIVE_TLS = 'true' }
    Write-Host '      회사망: Windows 에 깔린 회사 인증서를 믿습니다'
}

function Stop-App {
    # 앱을 끄면 Windows 가 앱이 띄운 도우미도 같이 끈다
    $apps = @(Get-Process -Name 'LLM Deck', 'LLM Deck Mirror' -ErrorAction SilentlyContinue)
    if (-not $apps) { return }
    $apps | Stop-Process -Force -ErrorAction SilentlyContinue
    # 다 꺼져야 exe 를 지우거나 새로 쓸 수 있다
    $apps | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
}

# 도우미 앱을 만든다. Windows 에 처음부터 들어 있는 .NET Framework 4 의 C# 컴파일러(csc.exe)를 쓴다
function Build-App {
    $csc = $null
    foreach ($fw in 'Framework64', 'Framework') {
        $path = Join-Path $env:WINDIR "Microsoft.NET\$fw\v4.0.30319\csc.exe"
        if (Test-Path $path) { $csc = $path; break }
    }
    if (-not $csc) { throw '.NET Framework 4 의 csc.exe 를 찾지 못했습니다' }
    Stop-App
    Remove-Item $AppExe, $OldMirrorExe, $OldMirrorLink -Force -ErrorAction SilentlyContinue
    # csc 가 경고를 내도 멈추지 않게 이 안에서만 풀어 둔다
    $ErrorActionPreference = 'Continue'
    $cscArgs = @('/nologo', '/target:winexe', '/optimize+', '/codepage:65001', "/out:$AppExe",
        '/r:System.dll', '/r:System.Drawing.dll', '/r:System.Windows.Forms.dll', (Join-Path $PSScriptRoot 'app.cs'))
    $out = & $csc $cscArgs 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $AppExe)) {
        $out | ForEach-Object { Write-Host "      $_" }
        throw '컴파일에 실패했습니다'
    }
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($AppLink)
    $link.TargetPath = $AppExe
    $link.WorkingDirectory = $PSScriptRoot
    $link.Description = 'LLM Deck 도우미를 켜고 기기 화면(미러링)을 연다'
    $link.Save()
}

function Stop-Companion {
    Stop-App
    # 앱 없이 켠 도우미(예전 방식이나 앱을 만들지 못했을 때)도 끈다. 이 폴더의 도우미만
    Get-CimInstance Win32_Process |
        Where-Object { $_.CommandLine -and $_.CommandLine -like '*main.py*' -and ($_.CommandLine -like "*$Dir*" -or $_.CommandLine -like '*llm-deck*') } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Mirror) {
    # 도우미 앱이 있으면 직접 띄운다 (uv 를 거치면 uv 가 끝날 때 앱도 같이 꺼질 수 있다).
    # 앱이 떠 있으면 그 앱이 창을 열고, 도우미가 꺼져 있으면 창에 안내가 뜬다
    if (Test-Path $AppExe) {
        Start-Process -FilePath $AppExe -ArgumentList '--mirror' -WorkingDirectory $PSScriptRoot
        exit 0
    }
    # 앱이 없으면 브라우저로 연다 (mirror.py)
    $uv = Find-Uv
    if (-not $uv) { Write-Host '먼저 install.cmd 로 설치하세요.'; exit 1 }
    & $uv run --directory $Dir main.py --mirror
    exit $LASTEXITCODE
}

if ($Uninstall) {
    Stop-Companion
    Remove-Item $AppExe, $AppLink, $OldMirrorExe, $OldMirrorLink -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\LLMDeck' -Recurse -ErrorAction SilentlyContinue  # 도우미 앱 설정 (도우미 띄우는 방법, 미러링 창의 항상 위·배율·위치)
    Remove-ItemProperty -Path $RunKey -Name $Name -ErrorAction SilentlyContinue
    Set-Firewall 'remove' | Out-Null
    $uv = Find-Uv
    if ($uv -and (Test-Path (Join-Path $env:USERPROFILE '.claude'))) {
        & $uv run --directory $Dir main.py --uninstall-statusline
    }
    Write-Host '제거했습니다. 이 폴더는 지워도 됩니다.'
    return
}

Write-Host '[1/7] Python 도구(uv) 확인'
# 회사망 설정은 uv 를 받기 전부터 쓴다 (압축 파일에는 app\config.toml 이 들어 있다)
$network = Read-Section (Join-Path $Dir 'config.toml') 'network'
Use-Network $network
$uv = Find-Uv
if (-not $uv) {
    Write-Host '      uv 를 설치합니다 (https://astral.sh/uv)'
    try {
        Invoke-RestMethod https://astral.sh/uv/install.ps1 | Invoke-Expression
    } catch {
        Write-Host "      받지 못했습니다: $($_.Exception.Message)"
    }
    $uv = Find-Uv
    if (-not $uv) { throw 'uv 를 설치하지 못했습니다. 회사 보안이 막은 것이면 INSTALL.txt 의 "9. 회사 PC" 대로 uv.exe 를 직접 받아 install.cmd 옆에 두고 다시 실행하세요.' }
}
Write-Host "      $uv"
$uvw = Join-Path (Split-Path $uv) 'uvw.exe'
if (-not (Test-Path $uvw)) { throw "uvw.exe 가 없습니다 ($uvw). uv 를 받은 압축 파일 안의 uvw.exe 도 uv.exe 옆에 두세요." }
if ($network['system_certs'] -eq $true) { Use-SystemCerts $uv }

Write-Host '[2/7] 설정 파일과 저장 위치'
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
# 용량이 적은 PC 에서 큰 것을 다른 드라이브에 둘 때
$storage = Read-Section $config 'storage'
$modelDir = Get-StoragePath $storage 'model_dir'
$cacheDir = Get-StoragePath $storage 'uv_cache_dir'
$pythonDir = Get-StoragePath $storage 'python_dir'
# 부품 보관함: uv 는 이 폴더의 uv.toml 을 읽는다 (로그인할 때 켜는 uvw 도 같은 것을 읽는다)
$uvToml = Join-Path $Dir 'uv.toml'
if ($cacheDir) {
    New-Item -ItemType Directory -Force $cacheDir | Out-Null
    $text = "# install.ps1 이 config.toml 의 [storage] uv_cache_dir 로 만든다. 이 파일 말고 config.toml 을 고치세요`r`ncache-dir = '$cacheDir'`r`n"
    [IO.File]::WriteAllText($uvToml, $text, (New-Object Text.UTF8Encoding $false))
    Write-Host "      설치 부품 보관함: $cacheDir"
} elseif (Test-Path $uvToml) {
    Remove-Item $uvToml
}
# Python: uv.toml 로는 정할 수 없어서 이번 설치 동안 환경 변수로 알려 준다
if ($pythonDir) {
    New-Item -ItemType Directory -Force $pythonDir | Out-Null
    $env:UV_PYTHON_INSTALL_DIR = $pythonDir
    Write-Host "      Python: $pythonDir"
    # 이미 깔린 도우미 부품이 기본 자리의 Python 을 쓰고 있으면, 새 자리의 Python 으로 새로 만든다
    $venvCfg = Join-Path $Dir '.venv\pyvenv.cfg'
    $oldPython = Join-Path $env:APPDATA 'uv\python'
    $found = if (Test-Path $venvCfg) { Select-String -Path $venvCfg -Pattern '^home\s*=\s*(.+)$' } else { $null }
    if ($found -and -not $pythonDir.StartsWith($oldPython, 'OrdinalIgnoreCase')) {
        $pyHome = $found.Matches[0].Groups[1].Value.Trim()
        if ($pyHome.StartsWith($oldPython, 'OrdinalIgnoreCase')) {
            Stop-Companion
            Remove-Item (Join-Path $Dir '.venv') -Recurse -Force
            Write-Host '      Python 자리를 옮겨 도우미 부품을 새로 만듭니다.'
            Write-Host "      예전 Python 이 $oldPython 에 남습니다. 다른 곳에서 쓰지 않으면 지워도 됩니다."
        }
    }
}
if ($modelDir) {
    New-Item -ItemType Directory -Force $modelDir | Out-Null
}

Write-Host '[3/7] 도우미 준비 (처음에는 몇 분 걸릴 수 있습니다)'
& $uv sync --directory $Dir --quiet
if ($LASTEXITCODE -ne 0) { throw '도우미 준비에 실패했습니다.' }
# Windows PowerShell 5.1 은 Stop 일 때 프로그램의 오류 출력만으로도 멈추므로, 아래 확인 동안만 풀어 둔다
$ErrorActionPreference = 'Continue'
# 고친 config.toml 에 틀린 곳이 없는지 본다 (틀리면 도우미가 켜지지 않는다)
$bad = & $uv run --directory $Dir python -c "import tomllib, pathlib; tomllib.loads(pathlib.Path('config.toml').read_bytes().decode('utf-8-sig'))" 2>&1
$configOk = $LASTEXITCODE -eq 0
# 음성 질문용 부품(faster-whisper)이 이 PC 에서 켜지는지 본다. 안 돼도 나머지 기능은 쓸 수 있다.
& $uv run --directory $Dir python -c 'import ctranslate2, onnxruntime, av, faster_whisper' 2>$null
$voiceOk = $LASTEXITCODE -eq 0
$ErrorActionPreference = 'Stop'
if (-not $configOk) {
    Write-Host "      config.toml 에 틀린 곳이 있습니다: $($bad | Select-Object -Last 1)"
    Write-Host "      Windows 경로는 작은따옴표로 적으세요. 예: model_dir = 'D:\LLM Deck\models'"
    throw 'config.toml 을 고친 뒤 다시 실행하세요.'
}
if (-not $voiceOk) {
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
    & $uv run --directory $Dir main.py --install-statusline
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
$appOk = $false
try {
    Build-App
    $appOk = $true
    Write-Host '      작업 표시줄 오른쪽 아래의 LLM Deck 아이콘이나 시작 메뉴의 "LLM Deck" 으로 미러링 창을 엽니다.'
} catch {
    Write-Host "      건너뜁니다: $($_.Exception.Message)"
    Write-Host '      도우미는 창 없이 돌고, mirror.cmd 를 누르면 브라우저로 엽니다 (http://127.0.0.1:4013/).'
}

Write-Host '[7/7] 로그인할 때 자동 실행'
Stop-Companion
# 도우미를 끈 뒤에 비운다 (켜져 있으면 쓰는 중인 파일이 있어 지우지 못할 수 있다)
if ($storage['clean_cache'] -eq $true) {
    & $uv cache clean --directory $Dir --quiet
    Write-Host '      설치 부품 보관함을 비웠습니다 (clean_cache = true)'
}
# 예전 방식(공유만 하는 도우미)이 등록되어 있으면 지운다. 이제 이 도우미가 공유도 함께 한다.
Remove-ItemProperty -Path $RunKey -Name 'LLMDeckSync' -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Split-Path $LogFile) | Out-Null
if ($appOk) {
    # 앱이 도우미를 띄운다. 로그인할 때는 미러링 창 없이 트레이 아이콘만
    New-Item -Path $HelperKey -Force | Out-Null
    Set-ItemProperty -Path $HelperKey -Name 'Uvw' -Value $uvw
    Set-ItemProperty -Path $HelperKey -Name 'Dir' -Value $Dir
    Set-ItemProperty -Path $HelperKey -Name 'Log' -Value $LogFile
    Set-ItemProperty -Path $RunKey -Name $Name -Value "`"$AppExe`" --background"
    Start-Process -FilePath $AppExe -ArgumentList '--background' -WorkingDirectory $PSScriptRoot
} else {
    $command = "`"$uvw`" run --directory `"$Dir`" main.py --log `"$LogFile`""
    Set-ItemProperty -Path $RunKey -Name $Name -Value $command
    Start-Process -FilePath $uvw -ArgumentList @('run', '--directory', "`"$Dir`"", 'main.py', '--log', "`"$LogFile`"") -WindowStyle Hidden
}

Write-Host ''
if ($appOk) {
    Write-Host '설치했습니다. 도우미가 실행 중입니다 (작업 표시줄 오른쪽 아래 LLM Deck 아이콘).'
} else {
    Write-Host '설치했습니다. 도우미가 창 없이 실행 중입니다.'
}
Write-Host "기록: $LogFile"
Write-Host '기기 연결: USB 케이블, 블루투스(LLM-Deck), 같은 Wi-Fi 중 되는 것으로 자동 연결합니다.'
