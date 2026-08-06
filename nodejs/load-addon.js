'use strict'

// Standard prebuild loader (prebuildify / node-gyp-build compatible layout).
//
// Resolution order:
//   1. A prebuilt addon shipped inside this package under
//      prebuilds/<platform>-<arch>/http2addon.node  (produced by CI and packed
//      into the published tarball). This lets `npm install ./pkg.tgz` work with
//      zero compilation on the supported platforms.
//   2. A locally compiled addon at build/Release/http2addon.node (source build).
//
// Supported triplet suffixes match the artifacts published by CI (the release
// job's plat_of() maps windows-latest -> "win", macos-latest -> "macos"):
//   linux-x64    (libhttp2client.so)
//   linux-arm64  (libhttp2client.so)
//   macos-x64    (libhttp2client.dylib)
//   macos-arm64  (libhttp2client.dylib, Apple Silicon)
//   win-x64      (libhttp2client.dll + MinGW runtimes)
// Note: the directory uses "win"/"macos" (not "win32"/"darwin" from
// process.platform), so we map process.platform to the published name.

const fs = require('fs')
const path = require('path')

function detectTriplet () {
  const plat = process.platform // linux | darwin | win32
  const arch = process.arch // x64 | arm64 | ...
  // Map Node's process.platform to the prebuilds directory name used by CI.
  const platName = plat === 'win32' ? 'win' : plat === 'darwin' ? 'macos' : plat
  const map = {
    'linux-x64': 'linux-x64',
    'linux-arm64': 'linux-arm64',
    'macos-x64': 'macos-x64',
    'macos-arm64': 'macos-arm64',
    'win-x64': 'win-x64'
  }
  return map[`${platName}-${arch}`] || null
}

// On Linux and macOS the addon links against libhttp2client.so / .dylib which
// is shipped in the same prebuilds/ dir (see CI release job). The dynamic
// linker won't look there by default, so prepend the dir to LD_LIBRARY_PATH
// (Linux) / DYLD_FALLBACK_LIBRARY_PATH (macOS) before loading the .node.
function ensureLibPath (addonDir) {
  if (process.platform === 'linux') {
    const sep = process.env.LD_LIBRARY_PATH ? ':' : ''
    process.env.LD_LIBRARY_PATH = addonDir + sep + (process.env.LD_LIBRARY_PATH || '')
  } else if (process.platform === 'darwin') {
    const sep = process.env.DYLD_FALLBACK_LIBRARY_PATH ? ':' : ''
    process.env.DYLD_FALLBACK_LIBRARY_PATH = addonDir + sep + (process.env.DYLD_FALLBACK_LIBRARY_PATH || '')
  }
}

function loadAddon () {
  const triplet = detectTriplet()
  if (triplet) {
    const prebuildDir = path.join(__dirname, 'prebuilds', triplet)
    const prebuild = path.join(prebuildDir, 'http2addon.node')
    if (fs.existsSync(prebuild)) {
      ensureLibPath(prebuildDir)
      return require(prebuild)
    }
  }
  // Fallback: compile from source (binding.gyp) — requires the C library built.
  const compiledDir = path.join(__dirname, 'build', 'Release')
  const compiled = path.join(compiledDir, 'http2addon.node')
  if (fs.existsSync(compiled)) {
    ensureLibPath(compiledDir)
    return require(compiled)
  }
  throw new Error(
    'http2 client native addon not found. Install a prebuilt package for ' +
    `${process.platform}-${process.arch}, or run \`npm run build\` after building the C library.`
  )
}

module.exports = loadAddon()
