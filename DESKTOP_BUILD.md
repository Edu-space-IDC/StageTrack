# StageTracks desktop builds

The repository root contains the StageTracks Windows application source and the official Vite website. Build requirements: Visual Studio 2022 with the Desktop C++ workload, CMake, .NET 8 SDK, Node.js 20 or newer, and npm.

From the repository root, install the website dependencies and compile the release:

```powershell
npm ci
.\packaging\windows\build-release.ps1
```

The script builds StageTracks, Reaper Control Studio, the StageTracks installer, the portable package, and the Reaper files archive. Artifacts are written to `release-artifacts/`.

The large pad audio loops are included in the portable release and installer, but are not stored in Git. To rebuild a release with pads, extract the `Pads/` directory from the latest `StageTracks-portable-windows-x64.zip` into the repository root before running the build script. The Steinberg ASIO SDK is optional and is also excluded; install it separately and set `ASIO_SDK_DIR` if ASIO support is needed.

StageTracks can export the active song as a `.rpseq` package. Upload that package through **Comunidad → Subir secuencia** on the official site.
