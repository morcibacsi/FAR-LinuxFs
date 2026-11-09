# UBIFS Extract for FAR Manager

A FAR Manager 3 plugin which opens Linux file system images as read-only panels, like archives.

| Format | Support |
| --- | --- |
| UBIFS, raw volume image | LZO, zlib and uncompressed data. The journal is replayed, so the result matches what a mounted file system shows. |
| ext2, ext3, ext4 | Extents, indirect blocks, 64-bit, flex_bg, htree directories. The journal is not replayed. |

Formats are detected from the file's contents, so the file extension does not matter.

Not supported: UBIFS images with zstd-compressed files (those files report an error), encrypted or authenticated UBIFS, images still wrapped in a UBI container (`UBI#` at offset 0), and ext4 with inline data, encryption or `meta_bg`.

## Usage

Press <kbd>Enter</kbd> or <kbd>Ctrl+PgDn</kbd> on an image to open it. Browse it as usual, view files with <kbd>F3</kbd>, and copy files out with <kbd>F5</kbd>. The description column (panel mode <kbd>LeftCtrl+6</kbd>) shows the Unix permissions and symlink targets.

Windows cannot represent everything a Linux file system holds, so extraction differs in a few ways:

- **Symlinks** become small text files holding the link target. This is deliberate: most targets are absolute paths like `/bin/busybox`, which would dangle on Windows.
- **Device nodes, FIFOs and sockets** become empty files.
- **Names NTFS cannot store** have the offending characters replaced with `_`. Names which differ only by case get a `~1` suffix so that neither file is lost, for example `xt_HL.ko` and `xt_hl.ko~1`.
- **Ownership and permissions** are not applied to extracted files. Modification times are.

## Building

Requires Visual Studio with the C++ workload, CMake 3.21 or newer, and the FAR Manager source tree in a sibling folder named `FarManager`.

```powershell
cmake -S . -B out/x86 -G "Visual Studio 18 2026" -A Win32
cmake --build out/x86 --config Release

cmake -S . -B out/x64 -G "Visual Studio 18 2026" -A x64
cmake --build out/x64 --config Release
```

Copy `UbiFsExtract.dll` from `out/<arch>/bin/Release` into `Plugins\UbiFsExtract` under your FAR folder and restart FAR. Use the architecture of **Far.exe**, not of Windows.

The build also produces two console programs in the same folder:

- `ubifsx info|list <image>` and `ubifsx extract <image> <directory>` expose the readers without FAR.
- `test_decompress` checks the decompressors against known vectors.
