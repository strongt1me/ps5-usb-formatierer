# Changelog

All notable changes to the PS5 USB Formatter. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
version numbers follow [Semantic Versioning](https://semver.org/). The files for every version are under
[Releases](https://github.com/strongt1me/ps5-usb-formatierer/releases). German version: [CHANGELOG.md](CHANGELOG.md).

## [Unreleased]

### Added

- Changelog in English (CHANGELOG.en.md).
- "What's new" in the PC programs shows the changes of every version since the installed one; in the web interface a click on "New version … available" shows them as well.

## [2.6.64] – 2026-10-31

### Removed

- Web interface: tab switching with L1/R1 – it did not work on the PS5. L2/R2 still scroll.

## [2.6.63] – 2026-10-31

### Changed

- Web interface: "Stop formatter" is now called "Turn off formatter" (button, dialog and message, all languages).

### Removed

- Help: note about tab switching with L1/R1.

## [2.6.62] – 2026-10-31

### Added

- "Mounted" badge in the game list, filter "Only mounted" (web interface) and sort order "Mounted" (program).
- "Unmount and continue" when "Re-register tile" fails because of a mounted game.
- Web interface on the PS5: L1/R1 switch tabs.
- The PS5 notification "stopped" names the source of the command; every stop is logged to `/data/usb_format/quit.log`.
- Diagnostic report with the last stop commands and notable kernel log lines.
- Linux AppImage: "Update now" downloads only the changed parts (zsync, also usable with AppImageUpdate).

### Changed

- Web interface: "Quit" is called "Stop formatter"; the dialog explains that the PS button is enough to close the app.
- The ShadowMountPlus message "a game is still mounted" is shown in plain words instead of an English error.

## [2.6.61] – 2026-10-30

### Added

- Web interface on the PS5: L2/R2 scroll the page, the harder you press, the faster.

### Changed

- Games and PKG: the first drive with games is preselected; empty lists say what to do; copy, move and delete appear only once something is ticked.
- Formatting turns red only once a drive is selected; an SSD preselected for games is never taken over.
- Save data: shorter texts and dates in the format of the language.
- Tools: all buttons in one row, below them the reason when they are disabled.
- The window keeps its size in all modes and tabs.

### Fixed

- The fPKG/PKG explanation disappeared as soon as packages were loaded.

## [2.6.60] – 2026-10-30

### Added

- Game details show whether ShadowMountPlus has mounted the game, plus patch and DLCs – with unmount/mount and removal of patch or DLCs.
- Duplicate games: "Deactivate older version …" or "Deactivate surplus copy …" (`.off` extension instead of deleting).
- After ejecting, the drive shows "can be unplugged"; a stick plugged back in appears by itself.

### Changed

- Web interface: more space between the checkboxes and the edge, calmer list when activating/deactivating.

## [2.6.59] – 2026-10-29

### Added

- "Activate / Deactivate …": images (`.ffpfs`, `.ffpfsc`, `.ffpkg`, `.exfat`) get or lose the `.off` extension, so ShadowMountPlus no longer mounts them. Deactivated games are shown as such.

### Changed

- Deactivated copies do not count as duplicate games; verify, checksums, copy, delete and clean-up take them into account.

## [2.6.58] – 2026-10-29

### Fixed

- PS5 USB Formatter: crash while loading game pictures without a connection to the PS5 (this is why 2.6.57 lacked the Linux builds).

## [2.6.57] – 2026-10-29

### Changed

- Game pictures: large pictures in UFS2 images are shown too; games without a picture are not requested again on every refresh.
- Eject: the note "needs ShadowMountPlus 1.7" appears only when that is certain; a ShadowMountPlus started later is detected.

### Fixed

- PS5 USB Formatter: settings were lost with a broken or locked settings file; after switching to another PS5 its details did not apply.

### Security

- "Compare contents" is robust against manipulated images: detects directory and FAT loops, can be cancelled and limits memory and scope.

## [2.6.56] – 2026-10-29

### Added

- "Eject" safely unregisters a drive via ShadowMountPlus 1.7.
- Game pictures in the game list (ShadowMountPlus preview or `icon0.png`, also from images).
- ShadowMountPlus version in the footer; functions that need 1.7 are disabled without it.
- Speed history per drive as a small chart, clear drops marked red.
- Diagnostic and error reports with firmware, ShadowMountPlus version and kernel extensions.

### Changed

- The ShadowMountPlus log (`debug.log`) is read only when its API does not answer.

## [2.6.55] – 2026-10-29

### Added

- Note about possible wear when a stick reads clearly slower than before.
- "Compare contents" also compares an image (exFAT, UFS2) with a game folder.

### Changed

- Built with PS5 Payload SDK 0.45 (firmware 14.00/14.10).
- The Linux builds still run from glibc 2.35 on.

### Fixed

- Game folders with `eboot2.bin` (ShadowMountPlus 1.7) counted as "incomplete".

## [2.6.54] – 2026-10-29

### Changed

- The project licence is included with the Windows programs and AppImages.

### Fixed

- Settings of "On the PC – free formatting" were lost since 2.6.52.
- Help: keyboard shortcuts and mode names corrected.
- "Compare contents" rejects the same game twice; the Windows queue finds its target after a mode switch; CSV history in local time.

## [2.6.53] – 2026-10-29

### Fixed

- Web interface: search field, checkmarks, sizes and dialog buttons look right in the PS5 browser.
- PC programs: scroll bars only when needed; "Searching …" instead of "no USB drive found" while searching.

## [2.6.52] – 2026-10-29

### Fixed

- Two windows of the same program no longer overwrite each other's settings.
- Identical sticks without a serial number are not silently selected after reloading.
- Linux: btrfs partitions are detected and unmounted.
- Windows: an image that lies on the target stick via `subst` or a local share is detected.

## [2.6.51] – 2026-10-29

### Fixed

- Clean-up wrongly reported files as orphaned while loading or in very full folders.
- Copy, verify and clean-up failed with more than about 56 entries.
- A cancelled save data upload left a file open.
- A crafted image file with a nonsensical size crashed the formatter.
- Queue: the target is found by serial number; the web queue got stuck on "running".
- Windows app: the copy dialog froze with a slow PS5; error when closing during a request.
- Linux: the helper checks again right before writing that the same drive is still plugged in.
- PS5 search on Linux scans all network adapters.

## [2.6.50] – 2026-10-29

### Added

- "Compare contents" checks two copies of the same game byte by byte.
- Folder games without `eboot.bin` or title picture are marked "incomplete".
- Target selection shows the last measured read speed.
- Save the history as CSV.

### Changed

- Windows app: the copy queue survives a restart.

## [2.6.49] – 2026-10-28

### Added

- Web interface: probable copies (same version and size) are detected.

### Changed

- Web interface: the copy queue survives reloading the page.
- Target selection shows "not enough space" and the FAT32 limit right next to the drive.

## [2.6.48] – 2026-10-28

### Added

- "Clean up" finds orphaned checksums, dump infos, backport folders and empty game folders.
- Checksum lists for folder games (`<Name>.manifest.sha256`).
- Queue for copy jobs.
- Web interface: diagnostic report in the footer.

### Changed

- Speed and remaining time restart with every step.

## [2.6.47] – 2026-10-28

### Added

- Partial copies of cancelled copy jobs are shown and can be discarded.
- "Create checksum" puts a `.sha256` next to images.
- Game details by click or double-click.
- "Select older version" for duplicate games.

### Fixed

- Web interface: button bars stuck out over the edge on narrow screens.

## [2.6.46] – 2026-10-27

### Added

- A cancelled copy can be resumed.
- "Verify games" checks images against their `.sha256`.
- Searchable game list in the web interface; dialogs show the free space afterwards.
- Windows app: `fehler.log` and "Copy report" (Ctrl+Shift+B).

## [2.6.45] – 2026-10-27

### Fixed

- The Windows programs were missing from release 2.6.44 (CI test); otherwise the same as 2.6.44.

## [2.6.44] – 2026-10-27

### Fixed

- Windows: further protection against crashes with an open drop-down list in dialogs.

## [2.6.43] – 2026-10-27

### Changed

- Web interface: tab "Games" capitalised in the other languages.

## [2.6.42] – 2026-10-27

### Fixed

- Windows: crash (`KeyError: 'popdown'`) with an open drive list in the copy dialog.

## [2.6.41] – 2026-10-26

### Added

- Pre-check before copying: a target that is too small or the FAT32 limit are reported right away.
- PKG files compared with the PS5: newer, already installed or older; required firmware.
- Copy speed below the progress bar; deleting PKG files can be cancelled.

## [2.6.40] – 2026-10-26

### Fixed

- Restoring save data: a request with a path without slash could crash the formatter.
- The header checkbox selects only the newest version of each save; the old PS4 key is restored on errors.
- Hidden save data of other users is not backed up.
- Formatting writes the boot sector last.
- Memory leak, thread safety and other minor bugs.

## [2.6.39] – 2026-10-26

### Changed

- Windows program: wider save data list.

## [2.6.38] – 2026-10-26

### Added

- PKG tab: game name, version, type and required firmware from the packages.
- "Copy to …" for PKG files.

### Fixed

- A partially failed deletion is reported as such.
- Related files stay as long as another copy uses them.
- Windows program: lists after a theme/language change, sizes of folder games, freezing on network requests.

## [2.6.37] – 2026-10-25

### Changed

- Tabs depend on the selection: internal storage shows games, PKG and save data; drives also show tools and formatting.

## [2.6.36] – 2026-10-25

### Changed

- With internal storage selected there is no "Format" tab.
- Related files are determined once per search (faster).

### Fixed

- Crash when resuming an image backup (access to freed memory).

## [2.6.35] – 2026-10-25

### Added

- "Copy games …" – like moving, but the originals stay.

## [2.6.34] – 2026-10-25

### Changed

- Tab "Save data" is called "Save data on the PS5".

## [2.6.33] – 2026-10-25

### Fixed

- PS4 packages were wrongly shown as fPKG in 2.6.32.

## [2.6.32] – 2026-10-25

### Added

- Distinction between fPKG and PKG for PS4 packages (withdrawn in 2.6.33).

## [2.6.31] – 2026-10-25

### Fixed

- The progress bar of the PC search stopped with sleeping disks.

## [2.6.30] – 2026-10-25

### Changed

- Backport folders are found like ShadowMountPlus does and are taken along when copying and moving.

## [2.6.29] – 2026-10-25

### Added

- Copy, move and delete games on internal storage and on `/mnt/extN` – including related files.

## [2.6.28] – 2026-10-25

### Added

- Move and delete PKG files.

### Fixed

- The progress bar stayed at 0 % while a sleeping disk spun up.

## [2.6.27] – 2026-10-24

### Added

- Windows program with tabs like the web interface (games, PKG, save data, tools, format).
- Tools on the PS5 also in the PC program: measure speed, check & repair, rename, image backup.

## [2.6.26] – 2026-10-24

### Added

- PKG tab reads the package header: PKG/fPKG for PS5 packages, title ID and update for PS4 packages.

## [2.6.25] – 2026-10-24

### Added

- New interface with tabs, formatting inside a red warning frame.
- Type badges for games (dump folder, exFAT, ffpfs, ffpfsc, ffpkg).
- PKG tab with all `.pkg` files of the drive.
- Resume image backups and discard incomplete ones.

### Changed

- Reworked game list; sizes of folder games are determined in the background.

## [2.6.24] – 2026-10-24

### Changed

- Move dialog: note when ShadowMountPlus refuses to delete an original; neutral colour.

## [2.6.23] – 2026-10-24

### Added

- "Move games …" between drives, with a SHA-256 check of every file.

## [2.6.22] – 2026-10-24

### Added

- Web interface: sort the game list by name, size or title ID; size of folder games.

## [2.6.21] – 2026-10-24

### Changed

- Check report after formatting without warning colour.

## [2.6.20] – 2026-10-24

### Added

- Check report after formatting with individual results.
- Follow-up check of the freshly written exFAT file system.

## [2.6.19] – 2026-10-24

### Added

- Save data: previous versions of the backup (the newest and two before it are kept).
- The list shows the backup state; restoring compares with the current state.

## [2.6.18] – 2026-10-24

### Fixed

- The remaining-time line stayed after the job.

## [2.6.17] – 2026-10-23

### Changed

- Continuous progress bar for authenticity check, speed test and full formatting.
- Deleting games names the game.

## [2.6.16] – 2026-10-23

### Changed

- Progress of the image backup by used bytes; amount and remaining time below the bar.

## [2.6.15] – 2026-10-23

### Changed

- Save data: the containers of a game are shown only when expanded.

## [2.6.14] – 2026-10-23

### Fixed

- Setting the modification time of save data tries several system calls.

## [2.6.13] – 2026-10-23

### Fixed

- The timestamp repair for save data works on the PS5.

## [2.6.12] – 2026-10-23

### Added

- `POST /api/saves/times` and `tools/ps5_fix_save_times.py` to align save data that was already restored.

### Fixed

- Restoring save data reset the modification time, so games could no longer load the save.

## [2.6.11] – 2026-10-23

### Added

- Restore shows where a backup is stored.

## [2.6.10] – 2026-10-23

### Changed

- Save data grouped by game.

## [2.6.9] – 2026-10-23

### Added

- Back up and restore PS4 save data.

### Changed

- The system copy `sce_bu` is no longer listed.

## [2.6.8] – 2026-10-22

### Added

- Backing up and restoring save data can be cancelled.
- PS5/PS4 badge for each save.

## [2.6.7] – 2026-10-22

### Changed

- Real progress when backing up and restoring save data.

## [2.6.6] – 2026-10-22

### Added

- Game names in the save data list, multiple selection and backup to the PS5 or a USB drive.
- Restore from found backups or an uploaded ZIP.

### Fixed

- Backing up did nothing in the PS5 browser; extra error page when downloading.

## [2.6.5] – 2026-10-22

### Added

- Back up and restore PS5 save data ("Save data" page).

### Changed

- The Windows program builds its window in under a second.

## [2.6.4] – 2026-10-21

### Added

- "Last: …" at the drive shows the last job.

### Fixed

- For irreversible questions "Cancel" is preselected.
- Keyboard shortcuts no longer work during a job or with an open dialog.
- "Verify image" considered foreign `.sha256` files damaged.

## [2.6.3] – 2026-10-21

### Added

- Job history on the PS5 and in the PC programs.
- `SHA256SUMS` on every release.

## [2.6.2] – 2026-10-21

### Added

- "What's new" with download of the matching file.
- Update note in the web interface.
- Linux as AppImage and for ARM64.

## [2.6.1] – 2026-10-20

### Added

- Manage backups on the PS5 (verify, delete).
- Search and sorting in the game list; keyboard shortcuts; tooltips on buttons.

### Changed

- Theme "Automatic" follows the system even while running.

## [2.6.0] – 2026-10-20

### Added

- New interface of the PC programs in light and dark, with sidebar and drive cards.
- "Verify image" without writing back; delete backups on the PS5.
- Note about new versions.

### Fixed

- Error when switching from "On the PC – for PS5" to "On the PC – free".

## [2.5.11] – 2026-10-19

### Added

- Image backup of write-protected sticks and SD cards.

## [2.5.10] – 2026-10-19

### Changed

- PS5 USB Formatter: game list with checkboxes; larger window.

## [2.5.9] – 2026-10-19

### Fixed

- Write errors when writing back were reported as success.
- Freezing when opening "Back up …"/"Restore …".
- "Re-register tile" could hang.

### Security

- Windows checks again right before writing that the selected drive is plugged in.
- Images with a different sector size and images on the drive itself are rejected.

## [2.5.8] – 2026-10-19

### Added

- "Re-register tile", so that the PS5 shows the right version after an update.

## [2.5.7] – 2026-10-18

### Added

- Version check against the PS5 and offer to send the new build.

### Changed

- Image backup about six times faster (SHA-NI, zstd on up to four cores).
- Web interface: theme and quit at the top.

## [2.5.6] – 2026-10-18

### Added

- Progress bars while reading the drives, during the PS5 search and after "Send ELF".

### Changed

- The modes are called "At the PS5", "On the PC – for PS5" and "On the PC – free".

## [2.5.5] – 2026-10-18

### Fixed

- Web interface: the header wraps cleanly when space is short.

## [2.5.4] – 2026-10-18

### Changed

- Web interface: language selection at the top of the header.

## [2.5.3] – 2026-10-18

### Added

- Version display in all programs and on the app tile.

### Changed

- mkntfs 2026.9.28 and mke2fs 1.47.4.

## [2.5.2] – 2026-10-17

### Added

- Image backup in the PC program for sticks at the PS5, also to and from the PC.
- Dialog "Job not started" with the reason.

## [2.5.1] – 2026-10-17

### Changed

- Image backup skips empty areas (e.g. 8 instead of 53 seconds for an almost empty stick).

### Security

- While a game is running, mounted drives are no longer unregistered.

## [2.5.0] – 2026-10-17

### Added

- Image backup: back up whole drives as `.img.zst` and restore them, on the PS5 and on the PC.

## [2.4.9] – 2026-10-17

### Changed

- Documentation only: README extended with "Send ELF", PS5 search and mix-up protection.

## [2.4.8] – 2026-10-17

### Fixed

- Partition editor: only one bootable partition; impossible file systems are rejected before anything is deleted.
- Rename only with exactly one volume on the drive.
- Authenticity check: dead sticks as "defective", fake sticks with errors beyond the limit detected.

## [2.4.7] – 2026-10-17

### Fixed

- "Send ELF" reported success although the new build did not start.
- PS5 search on Linux fast again.
- Follow-up actions after a check always apply to the checked stick.

## [2.4.6] – 2026-10-17

### Added

- "Send ELF" sends the built-in formatter to the PS5 and replaces an older build.

## [2.4.5] – 2026-10-17

### Changed

- The PS5 search also finds the PS5 behind NAT (WSL, virtual machines).

## [2.4.4] – 2026-10-17

### Added

- "Delete games …" as a button in the PS5 app.
- Serial number check against swapped identical sticks.
- Developer footer.

### Fixed

- Linux: the programs did not start when `xrandr` is installed.
- A FAT32 label with umlauts prevented the connection to the PS5.

## [2.4.3] – 2026-10-16

### Fixed

- Re-plugging at the PS5 more reliable; NTFS/ext4 on slow sticks no longer report a premature error.
- The PC programs reach the PS5 with a configured proxy too.
- The file system choice matches exactly what the helper can do.

## [2.4.2] – 2026-10-16

### Fixed

- Soldered eMMC storage is no longer offered as an SD card.
- Cancelling before the first write still formatted.
- MBR partitions above 2 TB and HFS+ on large partitions.

## [2.4.1] – 2026-10-16

### Removed

- README: note about a planned Mac version.

## [2.4.0] – 2026-10-16

### Added

- Linux version of both PC programs.

### Fixed

- Windows: ext2/3/4 after a newly created FAT16/NTFS partition sometimes failed.

## [2.3.1] – 2026-10-15

### Changed

- Shorter warning at the top; licence GPL-3.0; new README in five languages.

## [2.3.0] – 2026-10-15

### Added

- Five languages (German, English, Italian, Spanish, French).
- Manual with a "Help" button.
- "On the PC standard" and a separate program `USB-Formatierer.exe` with all file systems.
- Expert mode for multiple partitions.

## [2.2.1] – 2026-10-15

### Fixed

- Windows program: the PC mode of 2.2.0 did not work.

## [2.2.0] – 2026-10-15

### Added

- "USB on this PC" in the Windows program.
- Delete games via ShadowMountPlus.
- Fake sticks are remembered and formatted only in their real area.

### Security

- Server parallel and robust against slow connections; Content Security Policy; checksum of the Windows helper.

## [2.1.7] – 2026-10-13

### Changed

- Windows program: the progress bar grows visibly; jobs started elsewhere are shown.

## [2.1.6] – 2026-10-13

### Changed

- Progress is shown in every open browser.

## [2.1.5] – 2026-10-13

### Changed

- Drop-down fields with their own subtle arrow.

## [2.1.4] – 2026-10-13

### Changed

- Main card slightly transparent and blurred.

## [2.1.3] – 2026-10-13

### Changed

- Main card without picture and slightly transparent.

## [2.1.2] – 2026-10-13

### Changed

- Main card without background picture.

## [2.1.1] – 2026-10-13

### Changed

- Authenticity test faster (at most 128 samples), with remaining time.

## [2.1.0] – 2026-10-13

### Added

- Web interface in two columns; size slider.
- `.ffpfs`/`.ffpfsc` games with title ID and name.

## [2.0.0] – 2026-10-13

### Added

- Web interface with tabs and drive cards.
- Show games and PKG files, detect duplicate games.
- Authenticity test, speed test, full formatting, folder template, rename.
- Check and repair exFAT; GPT from 2 TB.

### Fixed

- The app launcher tears down the sandbox cleanly.

[Unreleased]: https://github.com/strongt1me/ps5-usb-formatierer/compare/v2.6.64...HEAD
[2.6.64]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.64
[2.6.63]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.63
[2.6.62]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.62
[2.6.61]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.61
[2.6.60]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.60
[2.6.59]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.59
[2.6.58]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.58
[2.6.57]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.57
[2.6.56]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.56
[2.6.55]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.55
[2.6.54]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.54
[2.6.53]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.53
[2.6.52]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.52
[2.6.51]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.51
[2.6.50]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.50
[2.6.49]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.49
[2.6.48]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.48
[2.6.47]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.47
[2.6.46]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.46
[2.6.45]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.45
[2.6.44]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.44
[2.6.43]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.43
[2.6.42]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.42
[2.6.41]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.41
[2.6.40]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.40
[2.6.39]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.39
[2.6.38]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.38
[2.6.37]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.37
[2.6.36]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.36
[2.6.35]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.35
[2.6.34]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.34
[2.6.33]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.33
[2.6.32]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.32
[2.6.31]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.31
[2.6.30]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.30
[2.6.29]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.29
[2.6.28]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.28
[2.6.27]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.27
[2.6.26]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.26
[2.6.25]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.25
[2.6.24]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.24
[2.6.23]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.23
[2.6.22]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.22
[2.6.21]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.21
[2.6.20]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.20
[2.6.19]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.19
[2.6.18]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.18
[2.6.17]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.17
[2.6.16]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.16
[2.6.15]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.15
[2.6.14]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.14
[2.6.13]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.13
[2.6.12]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.12
[2.6.11]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.11
[2.6.10]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.10
[2.6.9]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.9
[2.6.8]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.8
[2.6.7]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.7
[2.6.6]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.6
[2.6.5]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.5
[2.6.4]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.4
[2.6.3]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.3
[2.6.2]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.2
[2.6.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.1
[2.6.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.0
[2.5.11]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.11
[2.5.10]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.10
[2.5.9]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.9
[2.5.8]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.8
[2.5.7]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.7
[2.5.6]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.6
[2.5.5]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.5
[2.5.4]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.4
[2.5.3]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.3
[2.5.2]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.2
[2.5.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.1
[2.5.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.0
[2.4.9]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.9
[2.4.8]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.8
[2.4.7]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.7
[2.4.6]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.6
[2.4.5]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.5
[2.4.4]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.4
[2.4.3]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.3
[2.4.2]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.2
[2.4.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.1
[2.4.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.0
[2.3.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.3.1
[2.3.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.3.0
[2.2.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.2.1
[2.2.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.2.0
[2.1.7]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.7
[2.1.6]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.6
[2.1.5]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.5
[2.1.4]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.4
[2.1.3]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.3
[2.1.2]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.2
[2.1.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.1
[2.1.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.0
[2.0.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.0.0
