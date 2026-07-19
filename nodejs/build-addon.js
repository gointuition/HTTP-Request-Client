#!/usr/bin/env node
'use strict'

// Cross-platform build driver for the native addon.
//
// Why a script instead of an inline npm command:
//   1. Clearing inherited compiler flags (CFLAGS/... ) needs POSIX-shell syntax
//      that does not work in Windows cmd/PowerShell.
//   2. On Windows the C library (libhttp2client.dll) is built with MinGW, but
//      the Node.js addon is built with MSVC (node-gyp's default on win32).
//      After the build we copy libhttp2client.dll next to the addon so it can
//      be found at runtime (Windows has no rpath).

const { spawnSync, execFileSync } = require('child_process')
const fs = require('fs')
const path = require('path')

// Locate MSVC lib.exe. Tries PATH first, then falls back to vswhere.
function findLibExe() {
  // 1. Already on PATH?
  const which = spawnSync('where', ['lib'], { stdio: 'pipe', env: process.env })
  if (!which.error && which.status === 0) return 'lib'

  // 2. Use vswhere (ships with VS Installer) to find the installation root.
  const vswhere = path.join(
    process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)',
    'Microsoft Visual Studio', 'Installer', 'vswhere.exe'
  )
  if (!fs.existsSync(vswhere)) return null

  let installPath
  try {
    installPath = execFileSync(vswhere, [
      '-latest', '-products', '*',
      '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
      '-property', 'installationPath'
    ], { encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }).trim()
  } catch (_) { return null }
  if (!installPath) return null

  // 3. Construct path to x64 lib.exe
  const libPath = path.join(installPath, 'VC', 'Tools', 'MSVC')
  if (!fs.existsSync(libPath)) return null
  // Pick the latest MSVC toolset version
  const versions = fs.readdirSync(libPath).sort().reverse()
  for (const ver of versions) {
    const candidate = path.join(libPath, ver, 'bin', 'Hostx64', 'x64', 'lib.exe')
    if (fs.existsSync(candidate)) return candidate
  }
  return null
}

// Clear inherited compiler flags so node-gyp uses its own defaults.
const env = { ...process.env, CFLAGS: '', CXXFLAGS: '', CPPFLAGS: '', LDFLAGS: '' }

// On Windows, MSVC needs an import library (http2client.lib) to link against
// the MinGW-built libhttp2client.dll. Generate it automatically if missing.
if (process.platform === 'win32') {
  const libDir = path.join(__dirname, '..', 'lib', 'shared')
  const dll = path.join(libDir, 'libhttp2client.dll')
  const lib = path.join(libDir, 'http2client.lib')

  if (!fs.existsSync(dll)) {
    console.error('build-addon: expected ' + dll + ' but it does not exist; ' +
      'build the C library first (cmake --build) so libhttp2client.dll is produced.')
    process.exit(1)
  }

  if (!fs.existsSync(lib)) {
    console.log('build-addon: generating http2client.lib from libhttp2client.dll ...')

    // Step 1: gendef (from MSYS2/MinGW) extracts exported symbols into a .def
    const defFile = path.join(libDir, 'libhttp2client.def')
    const gendef = spawnSync('gendef', [dll], { cwd: libDir, stdio: 'inherit', env })
    if (gendef.error || gendef.status !== 0) {
      console.error('build-addon: gendef failed. Make sure gendef is on PATH ' +
        '(pacman -S mingw-w64-x86_64-tools-git).')
      process.exit(1)
    }

    // Step 2: lib.exe (MSVC) creates the import library from the .def
    const libExePath = findLibExe()
    if (!libExePath) {
      console.error('build-addon: cannot find MSVC lib.exe. Install Visual Studio ' +
        '2022 with "Desktop development with C++" workload, or run this script ' +
        'from a "x64 Native Tools Command Prompt".')
      process.exit(1)
    }
    console.log('build-addon: using ' + libExePath)
    const libExe = spawnSync(libExePath, [
      '/def:' + defFile,
      '/out:' + lib,
      '/machine:x64'
    ], { cwd: libDir, stdio: 'inherit', env })
    if (libExe.error || libExe.status !== 0) {
      console.error('build-addon: lib.exe failed.')
      process.exit(1)
    }

    // Clean up intermediate .def file
    try { fs.unlinkSync(defFile) } catch (_) { /* ignore */ }
    console.log('build-addon: generated ' + lib)
  }
}

const args = ['rebuild']

// Resolve the locally installed node-gyp entry point.
const nodeGyp = require.resolve('node-gyp/bin/node-gyp.js')

const result = spawnSync(process.execPath, [nodeGyp, ...args], {
  stdio: 'inherit',
  env
})

if (result.error) {
  console.error(result.error)
  process.exit(1)
}
if (result.status !== 0) {
  process.exit(result.status === null ? 1 : result.status)
}

// Windows has no rpath, so the addon can only find libhttp2client.dll if it
// sits next to it. Copy it after a successful build.
if (process.platform === 'win32') {
  const dll = path.join(__dirname, '..', 'lib', 'shared', 'libhttp2client.dll')
  const destDir = path.join(__dirname, 'build', 'Release')
  fs.mkdirSync(destDir, { recursive: true })
  fs.copyFileSync(dll, path.join(destDir, 'libhttp2client.dll'))
  console.log('build-addon: copied libhttp2client.dll -> ' + destDir)
}

process.exit(0)
