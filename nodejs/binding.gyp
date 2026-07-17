{
  "targets": [
    {
      "target_name": "http2addon",
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
        }]
      ]
    }
  ]
}
