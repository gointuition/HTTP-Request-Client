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

const { spawnSync } = require('child_process')
const fs = require('fs')
const path = require('path')

// Clear inherited compiler flags so node-gyp uses its own defaults.
const env = { ...process.env, CFLAGS: '', CXXFLAGS: '', CPPFLAGS: '', LDFLAGS: '' }

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
  if (!fs.existsSync(dll)) {
    console.error('build-addon: expected ' + dll + ' but it does not exist; ' +
      'build the C library first (cmake --build) so libhttp2client.dll is produced.')
    process.exit(1)
  }
  fs.mkdirSync(destDir, { recursive: true })
  fs.copyFileSync(dll, path.join(destDir, 'libhttp2client.dll'))
  console.log('build-addon: copied libhttp2client.dll -> ' + destDir)
}

process.exit(0)
