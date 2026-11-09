<#
.SYNOPSIS
    一条命令完成 BBK 的版本号更新、编译打包、提交推送、创建 GitHub Release 并上传 BBK.bar。

.DESCRIPTION
    需要：
      - WSL 里 bbndk-term50 容器在运行，并且能通过 build.sh 编译
      - Windows 凭据管理器里有 github.com 的凭据（git push 能用即可）
      - 当前仓库的 origin 指向 github.com/chnak/BBK

.EXAMPLE
    .\release.ps1 -Version 1.0.1 -Notes "修了触控板三指中键"

.EXAMPLE
    .\release.ps1 -Version 1.1.0 -NotesFile .\notes.md -Prerelease

.EXAMPLE
    .\release.ps1 -Version 1.0.0 -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$Notes,
    [string]$NotesFile,

    [switch]$Prerelease,
    [switch]$SkipBuild,
    [switch]$NoPush,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent $MyInvocation.MyCommand.Path
$Tag  = "v$Version"
$Bar  = Join-Path $Repo 'BBK.bar'
$Desc = Join-Path $Repo 'bar-descriptor.xml'
$Attribution = 'Co-Authored-By: Claude Code <noreply@anthropic.com>'

function Say($text) { Write-Host "==> $text" -ForegroundColor Cyan }

function Get-WslPath([string]$windowsPath) {
    $p = & wsl.exe -d Ubuntu -e wslpath -u $windowsPath
    if ($LASTEXITCODE -ne 0 -or -not $p) {
        throw "wslpath 转换失败：$windowsPath"
    }
    return $p.Trim()
}

function Get-GitHubToken {
    $f = Join-Path $env:TEMP "bbk-ghcred-$PID.txt"
    "protocol=https`nhost=github.com`n" | Set-Content -NoNewline -Encoding ascii $f
    try {
        # cmd 的输入重定向比 PowerShell 管道可靠：GCM 能读到完整的凭据请求。
        $out = cmd /c "git credential fill < `"$f`" 2>&1"
        $line = $out | Where-Object { $_ -like 'password=*' } | Select-Object -First 1
        if (-not $line) {
            throw '凭据管理器里找不到 github.com 的凭据，先用 git push 登录一次'
        }
        return ($line -replace '^password=', '').Trim()
    }
    finally {
        Remove-Item $f -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-GitHub([string]$method, [string]$uri, $headers, $body) {
    $params = @{ Method = $method; Uri = $uri; Headers = $headers }
    if ($null -ne $body) {
        $params.Body = [System.Text.Encoding]::UTF8.GetBytes(($body | ConvertTo-Json -Depth 6))
        $params.ContentType = 'application/json; charset=utf-8'
    }
    Invoke-RestMethod @params
}

# --- 版本号 ---------------------------------------------------------------

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "版本号要写成 x.y.z，收到的是 '$Version'"
}

Say "版本 $Version（tag $Tag）"

$xml = Get-Content -Raw -Encoding UTF8 $Desc
if ($xml -notmatch '<versionNumber>[^<]*</versionNumber>') {
    throw "bar-descriptor.xml 里找不到 <versionNumber>"
}
$newXml = $xml -replace '<versionNumber>[^<]*</versionNumber>', "<versionNumber>$Version</versionNumber>"
$descChanged = ($newXml -ne $xml)

if ($descChanged) {
    if ($DryRun) {
        Say "（演练）会写入 bar-descriptor.xml"
    }
    else {
        [IO.File]::WriteAllText($Desc, $newXml, (New-Object Text.UTF8Encoding($false)))
        Say "已写入 bar-descriptor.xml"
    }
}
else {
    Say "版本号没有变化"
}

# --- 编译打包 -------------------------------------------------------------

if ($SkipBuild) {
    Say "跳过编译"
    if (-not (Test-Path $Bar)) { throw "没有找到 $Bar" }
}
else {
    $script = Get-WslPath (Join-Path $Repo 'build.sh')
    Say "编译（WSL / bbndk-term50）"
    if ($DryRun) {
        Say "（演练）会执行：wsl -d Ubuntu -e bash $script"
    }
    else {
        & wsl.exe -d Ubuntu -e bash $script
        if ($LASTEXITCODE -ne 0) { throw "编译失败（exit $LASTEXITCODE）" }
        if (-not (Test-Path $Bar)) { throw "编译完成但没有生成 $Bar" }
    }
}

# --- 提交与推送 -----------------------------------------------------------

if ($descChanged -and -not $DryRun) {
    Push-Location $Repo
    try {
        git add bar-descriptor.xml
        $msg = "release: $Version`n`n$Attribution"
        git commit -m $msg | Out-Null
        Say "已提交版本号"
    }
    finally {
        Pop-Location
    }
}

if ($NoPush) {
    Say "按参数跳过推送"
}
elseif ($DryRun) {
    Say "（演练）会执行：git push origin main"
}
else {
    Push-Location $Repo
    try {
        git push origin main
        if ($LASTEXITCODE -ne 0) { throw "git push 失败" }
        Say "已推送到 origin/main"
    }
    finally {
        Pop-Location
    }
}

# --- GitHub Release -------------------------------------------------------

$notesText = $Notes
if ($NotesFile) {
    $notesText = Get-Content -Raw -Encoding UTF8 $NotesFile
}
if (-not $notesText) {
    $notesText = @"
BBK $Version —— 把 BlackBerry Q20 当成 PC 的蓝牙键盘 / 鼠标。

- 键盘、触控板、音量/媒体键全部转成标准 HID 报告
- 双击拖选、双指右键、三指中键、双指捏合缩放
- 物理导航键转鼠标移动
- 振动反馈，可在界面上开关
- 屏幕常亮，断线自动重连

安装：开发者模式设备上安装 BBK.bar。
"@
}

if ($DryRun) {
    Say "（演练）会创建 Release $Tag 并上传 $(Split-Path -Leaf $Bar)"
    Say "（演练）完成，没有改动远端"
    return
}

Say "读取 GitHub 凭据"
$token = Get-GitHubToken
$headers = @{
    Authorization = "token $token"
    'User-Agent'  = 'bbk-release'
    Accept        = 'application/vnd.github+json'
}

$api = 'https://api.github.com'
$repoApi = "$api/repos/chnak/BBK"

# 标签已经有了就停下来，避免把同一个版本发两次。
try {
    Invoke-GitHub 'GET' "$repoApi/releases/tags/$Tag" $headers $null | Out-Null
    throw "Release $Tag 已经存在"
}
catch {
    if ($_.Exception.Message -match '已经存在') { throw }
    # 404 是正常情况：还没有这个 release。
}

Say "创建 Release $Tag"
$release = Invoke-GitHub 'POST' "$repoApi/releases" $headers @{
    tag_name         = $Tag
    name             = "BBK $Version"
    body             = $notesText
    draft            = $false
    prerelease       = [bool]$Prerelease
    target_commitish = 'main'
}

Say "上传 BBK.bar"
$upload = "$($release.upload_url -replace '\{.*$', '')?name=BBK.bar"
$asset = Invoke-RestMethod -Method Post -Uri $upload -Headers $headers `
    -ContentType 'application/octet-stream' -InFile $Bar

Write-Host ''
Say "完成：$($release.html_url)"
Say "附件：$($asset.browser_download_url)（$($asset.size) 字节）"
