# Broadcast Graphics Live — OBS Flatpak build from Windows/WSL2

Run from PowerShell in the root of the BGL source tree:

```powershell
powershell -ExecutionPolicy Bypass -File .\build-ubuntu-wsl.ps1 -Clean
```

The launcher uses WSL2 only as a host. The actual plugin is compiled by
`flatpak-builder` as an extension of `com.obsproject.Studio//stable`, using
`org.kde.Sdk//6.8`. Therefore the generated `.so` does not link against the
Ubuntu 26.04 host glibc.

## What the first run installs

Inside the selected WSL distribution, as root:

- `flatpak`
- `flatpak-builder`
- OBS Studio Flatpak, branch `stable`
- KDE SDK Flatpak, branch `6.8`

Subsequent builds reuse the downloaded runtimes and the persistent workspace.
Use `-SkipDependencies` to prevent package/runtime installation.

## Output

Archives and SHA-256 files are written to the configured Windows `build`
directory. The archive contains:

```text
broadcast-graphics-live/
  bin/64bit/broadcast-graphics-live.so
  data/...
```

Copy that directory to the Linux machine running OBS Flatpak:

```text
~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/
```

Then restart OBS.

## Configuration

Edit `build-ubuntu-wsl.config.json` when needed:

- `DistroName`: WSL2 distribution used only to host Flatpak Builder.
- `ObsFlatpakBranch`: normally `stable`.
- `KdeSdkVersion`: Qt/KDE SDK used by the OBS Flatpak build, currently `6.8`.
- `ArchiveFormat`: `zip`, `tar.gz`, or `both`.
- `Workspace`: persistent Linux-side build/cache directory.

The helper prints the highest `GLIBC_x.y` symbol required by the generated
plugin after each build.


## Flatpak builder filesystem requirement

The build directory and the `flatpak-builder` state directory are both stored under the configured WSL ext4 workspace. This avoids the `state dir is not on the same filesystem as the target dir` failure when the project itself is under `/mnt/c`.
