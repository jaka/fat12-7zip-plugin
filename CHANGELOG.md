# Changelog

## [1.1.0] - 2026-11-09

- Updating an image keeps existing files in place (DOS system disks stay bootable).
- New options: `boot=<file>` (boot sector from a file or bootable image), `lfn=off` (8.3 names only).
- DOS 3.x boot code is no longer overwritten when an image is updated.
- The volume label no longer takes the first root entry, which made updated MS-DOS system disks unbootable.
- Installer: asks when 7-Zip is using the plugin, no restart prompt, removes the copy from a previous 7-Zip folder, silent mode never stops at a dialog.
- CI: format check, Node.js 24 actions.

## [1.0.0] - 2026-11-07

First release.

[1.1.0]: https://github.com/jaka/fat12-7zip-plugin/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/jaka/fat12-7zip-plugin/releases/tag/v1.0.0
