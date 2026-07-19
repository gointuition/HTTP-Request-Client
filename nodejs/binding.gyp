{
  "targets": [
    {
      "target_name": "http2addon",
      # Disable node-gyp's Windows delay-load hook. The hook is only needed
      # when node.exe is renamed; we load a normally-named node.exe, so it is
      # unnecessary.
      "win_delay_load_hook": "false",
      "sources": ["http2-addon.cc"],
      "library_dirs": [
        "<(module_root_dir)/../lib/shared"
      ],
      "libraries": [
        "-lhttp2client"
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "MACOSX_DEPLOYMENT_TARGET": "13.3",
        "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
        "CLANG_CXX_LIBRARY": "libc++",
        "OTHER_CPLUSPLUSFLAGS": [
          "-std=c++17",
          "-stdlib=libc++"
        ]
      },
      "conditions": [
        ["OS=='mac'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "OTHER_LDFLAGS": ["-Wl,-rpath,@loader_path/../../../lib/shared"]
          }
        }],
        ["OS=='linux'", {
          "cflags_cc": ["-std=c++17"],
          "ldflags": ["-Wl,-rpath,\$$ORIGIN/../../../lib/shared"]
        }],
        ["OS=='win'", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": ["/std:c++17"]
            }
          }
          # C library (libhttp2client.dll) is built with MinGW; the addon itself
          # is built with MSVC (node-gyp default on win32). -lhttp2client resolves
          # to http2client.lib in library_dirs (../lib/shared). Windows has no
          # rpath, so build-addon.js copies libhttp2client.dll next to the
          # compiled addon after the build.
          # The addon forward-declares the C API (no project headers included),
          # so no extra include_dirs are needed for MSVC.
        }]
      ]
    }
  ]
}
