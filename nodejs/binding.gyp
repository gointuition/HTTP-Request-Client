{
  "targets": [
    {
      "target_name": "http2addon",
      # Disable node-gyp's Windows delay-load hook. It pulls in
      # win_delay_load_hook.cc via an absolute path (E:/.../node-gyp/src/...),
      # which node-gyp's make generator bakes into a rule as a drive-letter path
      # and breaks GNU make ("target pattern contains no '%'"). The hook is also
      # MSVC-specific (<delayimp.h>) and unusable under MinGW. We load in a
      # normally-named node.exe, so the hook is unnecessary.
      "win_delay_load_hook": "false",
      "sources": ["http2-addon.cc"],
      "include_dirs": [
        "<(module_root_dir)/../include"
      ],
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
          "cflags_cc": ["-std=c++17"]
          # Built with MinGW/GCC (MSYS2). -lhttp2client resolves against the
          # import/DLL in library_dirs (../lib/shared). Windows has no rpath,
          # so build-addon.js copies libhttp2client.dll next to the compiled
          # addon after the build. (We don't use gyp "copies" here: its make
          # generator emits drive-letter paths like E:/... that break GNU make.)
        }]
      ]
    }
  ]
}
