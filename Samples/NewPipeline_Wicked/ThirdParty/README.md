# NewPipeline third-party SDKs

## Open Image Denoise

Extract the platform-specific official OIDN 2.x package so that its contents
sit directly under:

```text
Samples/NewPipeline_Wicked/ThirdParty/OpenImageDenoise/
  include/OpenImageDenoise/oidn.hpp
  lib/...
  bin/...                         # Windows
```

Do not keep the downloaded package's top-level `oidn-*.windows` or
`oidn-*.macos` directory as an additional nested level. CMake automatically
finds this SDK on Windows and deploys every DLL from `bin`. The macOS Xcode
targets link `libOpenImageDenoise.dylib` and copy every dylib from `lib` and
`bin` into the application Frameworks directory.

The SDK payload is intentionally ignored by Git; this README is the versioned
directory contract.
