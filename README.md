# LinuxFS for FAR Manager

A FAR Manager 3 plugin which opens Linux file system images as read-only panels, like archives.

| Format | Support |
| --- | --- |
| UBIFS, raw volume image | LZO, zlib and uncompressed data. The journal is replayed, so the result matches what a mounted file system shows. |
| ext2, ext3, ext4 | Extents, indirect blocks, 64-bit, flex_bg, htree directories. The journal is not replayed. |

Formats are detected from the file's contents, so the file extension does not matter.

Not supported: UBIFS images with zstd-compressed files (those files report an error), encrypted or authenticated UBIFS, images still wrapped in a UBI container (`UBI#` at offset 0), and ext4 with inline data, encryption or `meta_bg`.

## Usage

Press <kbd>Enter</kbd> or <kbd>Ctrl+PgDn</kbd> on an image to open it. Browse it as usual, view files with <kbd>F3</kbd>, and copy files out with <kbd>F5</kbd>. The description column (panel mode <kbd>LeftCtrl+6</kbd>) shows the Unix permissions and symlink targets.

ArcLite also opens ext2/3/4 images, and FAR uses the first plugin in alphabetical order of DLL name, which is ArcLite. To choose between them, enable **File processing** under Options → Plugins manager settings.

Windows cannot represent everything a Linux file system holds, so extraction differs in a few ways:

- **Symlinks** become small text files holding the link target. This is deliberate: most targets are absolute paths like `/bin/busybox`, which would dangle on Windows.
- **Device nodes, FIFOs and sockets** become empty files.
- **Names NTFS cannot store** have the offending characters replaced with `_`. Names which differ only by case get a `~1` suffix so that neither file is lost, for example `xt_HL.ko` and `xt_hl.ko~1`.
- **Ownership and permissions** are not applied to extracted files. Modification times are.

## Building

Requires Visual Studio 2026 with the C++ workload, CMake 3.21 or newer, and the FAR Manager source tree in a sibling folder named `FarManager`.

```powershell
cmake -S . -B out/x86 -G "Visual Studio 18 2026" -A Win32
cmake --build out/x86 --config Release

cmake -S . -B out/x64 -G "Visual Studio 18 2026" -A x64
cmake --build out/x64 --config Release
```

Everything is written to `out/<arch>/bin/Release`:

- `LinuxFS.dll` is the plugin.
- `linuxfs.exe` reads images without FAR: `linuxfs info|list <image>` or `linuxfs extract <image> <directory>`.
- `README.md` is this file.
- `test_decompress.exe` checks the decompressors against known vectors.

To install, copy `LinuxFS.dll` and `README.md` into `Plugins\LinuxFS` under your FAR folder and restart FAR. Use the architecture of **Far.exe**, not of Windows.

Earlier builds were called UbiFsExtract. Delete the old `Plugins\UbiFsExtract` folder when upgrading: both are the same plugin to FAR, so it would load only one of them.

## Updates through Renewal

Copy `RenewalCustom.xml` from the release ZIP next to `Renewal.dll`. If a `RenewalCustom.xml` is already there, add the LinuxFS `<mod>` entry to its `<pd>` section instead. Then check for updates in Renewal. If you set up LinuxFS in Renewal before, enable the custom source for its entry.

## Versions and releases

The version defaults to 1.0.0. Pass `-DLINUXFS_VERSION=1.2.3` when configuring to change it. It is written into the version resource of `LinuxFS.dll` and `linuxfs.exe`, and into the plugin version FAR displays.

To publish a release, push a tag such as `v1.2.3` to GitHub. The release workflow builds both architectures with the version from the tag, runs `test_decompress`, and attaches `LinuxFS-1.2.3-x86.zip` and `LinuxFS-1.2.3-x64.zip` to a draft release. Each ZIP contains `LinuxFS.dll`, `linuxfs.exe`, `README.md` and `RenewalCustom.xml`. Check the draft, then publish it as a stable release: Renewal only sees the latest published release, and offers an update only when its version is higher than the installed one.

To build the ZIPs locally, configure both architectures with the version, build the Release configurations, then run:

```powershell
cpack --config out/x86/CPackConfig.cmake -C Release -B out/packages
cpack --config out/x64/CPackConfig.cmake -C Release -B out/packages
```
