# Copyright 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE.  See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program.  If not, see <https://www.gnu.org/licenses/>.

# INSTALL THE LATEST FEATHERS RELEASE ON WINDOWS, in one command (PowerShell):
#
#   irm https://raw.githubusercontent.com/owlet-labs/feathers/main/scripts/install.ps1 | iex
#
# It finds the newest release on GitHub, downloads the Windows zip, checks it against the release's SHA256SUMS.txt,
# and copies the synth and the Chain (CLAP and VST3) into YOUR plugin folders, which need no administrator rights:
#   %LOCALAPPDATA%\Programs\Common\CLAP   and   %LOCALAPPDATA%\Programs\Common\VST3
# Options, as environment variables set before the command (a piped script takes no arguments):
#   $env:FEATHERS_VERSION = 'v0.1.0'   install that release instead of the latest
#   $env:FEATHERS_SYSTEM  = '1'        install into C:\Program Files\Common Files\ instead (run PowerShell as administrator)
#   $env:FEATHERS_DRY_RUN = '1'        download and check, but copy nothing
# Written for Windows PowerShell 5.1, the one every Windows 10 and 11 has, and runs on PowerShell 7 too.

& {
	$ErrorActionPreference = 'Stop'
	$ProgressPreference = 'SilentlyContinue'  # 5.1's progress bar makes Invoke-WebRequest many times slower
	[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

	$repo = 'owlet-labs/feathers'
	$version = $env:FEATHERS_VERSION
	$system = $env:FEATHERS_SYSTEM -eq '1'
	$dryRun = $env:FEATHERS_DRY_RUN -eq '1'
	$headers = @{ 'User-Agent' = 'feathers-installer' }

	if ($version) {
		$release = Invoke-RestMethod -Headers $headers "https://api.github.com/repos/$repo/releases/tags/$version"
	} else {
		# `releases/latest` skips pre-releases; while every release is one, take the newest of all.
		$release = @(Invoke-RestMethod -Headers $headers "https://api.github.com/repos/$repo/releases?per_page=1")[0]
	}
	if (-not $release) { throw "release $(if ($version) { $version } else { 'latest' }) not found" }
	$tag = $release.tag_name
	$zipAsset = $release.assets | Where-Object { $_.name -like '*-windows-x86_64.zip' } | Select-Object -First 1
	$sumsAsset = $release.assets | Where-Object { $_.name -eq 'SHA256SUMS.txt' } | Select-Object -First 1
	if (-not $zipAsset) { throw "release $tag has no Windows build" }
	if (-not $sumsAsset) { throw "release $tag has no SHA256SUMS.txt, so the download cannot be checked" }

	$work = Join-Path ([IO.Path]::GetTempPath()) ("feathers-install-" + [Guid]::NewGuid())
	New-Item -ItemType Directory -Path $work | Out-Null
	try {
		Write-Host "Feathers $tag for Windows x86-64" -ForegroundColor Cyan
		$zip = Join-Path $work $zipAsset.name
		Invoke-WebRequest -UseBasicParsing -Headers $headers -Uri $zipAsset.browser_download_url -OutFile $zip
		$sums = (Invoke-WebRequest -UseBasicParsing -Headers $headers -Uri $sumsAsset.browser_download_url).Content
		if ($sums -is [byte[]]) { $sums = [Text.Encoding]::UTF8.GetString($sums) }
		$line = ($sums -split "`n") | Where-Object { $_ -match ("\s" + [regex]::Escape($zipAsset.name) + "\s*$") } | Select-Object -First 1
		if (-not $line) { throw "SHA256SUMS.txt does not list $($zipAsset.name)" }
		$want = ($line -split '\s+')[0].ToLowerInvariant()
		$got = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
		if ($want -ne $got) { throw "checksum MISMATCH for $($zipAsset.name) (expected $want, got $got); nothing installed" }
		Write-Host 'checksum ok'

		$x = Join-Path $work 'x'
		Expand-Archive -Path $zip -DestinationPath $x

		if ($system) {
			$clapDir = Join-Path $env:CommonProgramFiles 'CLAP'
			$vst3Dir = Join-Path $env:CommonProgramFiles 'VST3'
		} else {
			$clapDir = Join-Path $env:LOCALAPPDATA 'Programs\Common\CLAP'
			$vst3Dir = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'
		}
		$plan = @()
		foreach ($n in 'feathers.clap', 'feathers-chain.clap') {
			$s = Join-Path $x "CLAP\$n"; if (Test-Path $s) { $plan += ,@($s, $clapDir) }
		}
		foreach ($n in 'feathers.vst3', 'feathers-chain.vst3') {
			$s = Join-Path $x "VST3\$n"; if (Test-Path $s) { $plan += ,@($s, $vst3Dir) }
		}
		if ($plan.Count -eq 0) { throw 'the zip held no plugins' }

		if ($dryRun) {
			Write-Host "dry run: would install into $clapDir and $vst3Dir :" -ForegroundColor Cyan
			$plan | ForEach-Object { Write-Host "  $(Split-Path -Leaf $_[0])" }
			return
		}

		$installed = @()
		foreach ($p in $plan) {
			New-Item -ItemType Directory -Force -Path $p[1] | Out-Null
			$dest = Join-Path $p[1] (Split-Path -Leaf $p[0])
			if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
			Copy-Item -Recurse -Force $p[0] $dest
			Get-ChildItem -Recurse -File $dest | Unblock-File
			if ((Get-Item $dest) -isnot [IO.DirectoryInfo]) { Unblock-File $dest }
			$installed += $dest
		}
		Write-Host "Installed Feathers $tag :" -ForegroundColor Cyan
		$installed | ForEach-Object { Write-Host "  $_" }
		if (-not $system) {
			Write-Host "If your DAW does not find them, add those two folders to its plugin paths, or rerun as administrator with `$env:FEATHERS_SYSTEM='1'."
		}
		Write-Host 'Restart your DAW, or have it rescan its plugins.'
	} finally {
		Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
	}
}
