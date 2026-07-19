#!/usr/bin/env node
'use strict'

// Cross-platform build driver for the native addon.
//
// Why a script instead of an inline npm command:
//   1. Clearing inherited compiler flags (CFLAGS/... ) needs POSIX-shell syntax
//      that does not work in Windows cmd/PowerShell.
//   2. On Windows we build with the MSYS2 / MinGW-w64 GCC toolchain (the same
//      toolchain that builds libhttp2client.dll). node-gyp defaults to MSVC on
//      win32; we force the "make" generator so it skips Visual Studio detection
//      and produces GCC + make build files instead.
//
// Note on passing the format: node-gyp parses argv with nopt, which swallows
// "--format=make" into an option and never forwards it to the internal
// "configure" step. Passing it after "--" (i.e. `rebuild -- -f make`) makes nopt
// keep "-f make" in the remaining args, which `rebuild` then forwards to
// `configure` so the make generator actually takes effect.

const { spawnSync } = require('child_process')

// Clear inherited compiler flags so node-gyp uses its own defaults.
const env = { ...process.env, CFLAGS: '', CXXFLAGS: '', CPPFLAGS: '', LDFLAGS: '' }

const args = ['rebuild']
if (process.platform === 'win32') {
  // Force the make generator so node-gyp uses MinGW GCC instead of MSVC.
  // Must go after "--" so nopt keeps it for the configure step (see note above).
  args.push('--', '-f', 'make')
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
