#!/usr/bin/env node
'use strict'

// Cross-platform build driver for the native addon.
//
// Why a script instead of an inline npm command:
//   1. Clearing inherited compiler flags (CFLAGS/... ) needs POSIX-shell syntax
//      that does not work in Windows cmd/PowerShell.
//   2. On Windows we build with the MSYS2 / MinGW-w64 GCC toolchain (the same
//      toolchain that builds libhttp2client.dll). node-gyp defaults to MSVC on
//      win32; passing "--format=make" makes it skip Visual Studio detection and
//      generate GCC + make build files instead.

const { spawnSync } = require('child_process')

// Clear inherited compiler flags so node-gyp uses its own defaults.
const env = { ...process.env, CFLAGS: '', CXXFLAGS: '', CPPFLAGS: '', LDFLAGS: '' }

const args = ['rebuild']
if (process.platform === 'win32') {
  // Force the make generator so node-gyp uses MinGW GCC instead of MSVC.
  args.push('--format=make')
}

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
process.exit(result.status === null ? 1 : result.status)
