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
// Supported triplet suffixes match the artifacts published by CI:
//   linux-x64   (libhttp2client.so)
//   darwin-x64  (libhttp2client.dylib)
//   win32-x64   (libhttp2client.dll + MinGW runtimes)

const fs = require('fs')
const path = require('path')

function detectTriplet () {
  const plat = process.platform // linux | darwin | win32
  const arch = process.arch // x64 | arm64 | ...
  const map = {
    'linux-x64': 'linux-x64',
    'darwin-x64': 'darwin-x64',
    'win32-x64': 'win32-x64'
  }
  return map[`${plat}-${arch}`] || null
}

function loadAddon () {
  const triplet = detectTriplet()
  if (triplet) {
    const prebuilt = path.join(__dirname, 'prebuilds', triplet, 'http2addon.node')
    if (fs.existsSync(prebuilt)) {
      return require(prebuilt)
    }
  }
  // Fallback: compile from source (binding.gyp) — requires the C library built.
  const compiled = path.join(__dirname, 'build', 'Release', 'http2addon.node')
  if (fs.existsSync(compiled)) {
    return require(compiled)
  }
  throw new Error(
    'http2 client native addon not found. Install a prebuilt package for ' +
    `${process.platform}-${process.arch}, or run \`npm run build\` after building the C library.`
  )
}

module.exports = loadAddon()
