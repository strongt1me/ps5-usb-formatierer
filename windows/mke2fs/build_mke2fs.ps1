# Baut windows\mke2fs.exe (mke2fs aus e2fsprogs 1.47.1, unverändert) für ext2/ext3/ext4 am PC.
# Benötigt: python -m pip install ziglang. Die Quellen (e2fsprogs-1.47.1.tar.xz, kernel.org) werden bei Bedarf geladen,
# per SHA-256 geprüft und nach windows\mke2fs\build entpackt. Statt configure: config.h und die erzeugten Dateien in gen\
# (Fehlertabellen, CRC-Tabelle, Standardprofil, Typ-Header mit Windows-Größen).
param([string]$Tarball = "")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$work = Join-Path $here "build"
$name = "e2fsprogs-1.47.1"
$sha = "5a33dc047fd47284bca4bb10c13cfe7896377ae3d01cb81a05d406025d99e0d1"
$url = "https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.1/$name.tar.xz"
New-Item -ItemType Directory -Force $work | Out-Null

$tar = if ($Tarball) { $Tarball } else { Join-Path $work "$name.tar.xz" }
if (-not (Test-Path $tar)) {
    Write-Host "Lade $url"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $tar
}
if ((Get-FileHash -Algorithm SHA256 $tar).Hash.ToLower() -ne $sha) { throw "${tar}: falsche Prüfsumme" }
$S = Join-Path $work $name
if (-not (Test-Path (Join-Path $S "version.h"))) {
    # nur, was gebraucht wird (das Archiv enthält symbolische Links, die Windows ohne Sonderrechte nicht anlegt)
    tar -xf $tar -C $work "$name/lib" "$name/misc" "$name/include" "$name/version.h" "$name/NOTICE"
    if ($LASTEXITCODE -ne 0) { throw "Entpacken fehlgeschlagen" }
}

$zig = Join-Path (python -c "import ziglang, os; print(os.path.dirname(ziglang.__file__))") "zig.exe"
$G = Join-Path $here "gen"
$O = Join-Path $work "obj"
New-Item -ItemType Directory -Force $O | Out-Null
$cflags = @("-target", "x86_64-windows-gnu", "-O2", "-std=gnu11", "-DHAVE_CONFIG_H", "-D_FILE_OFFSET_BITS=64",
            "-D__USE_MINGW_ANSI_STDIO=1", "-w", "-I$here", "-I$G", "-I$G\ext2fs", "-I$G\support", "-I$G\blkid",
            "-I$G\uuid", "-I$S\lib", "-I$S\include\mingw", "-I$S\include")

# wie die Makefiles von e2fsprogs (lib/*/Makefile.in), ohne Teile, die mke2fs nicht braucht oder Windows nicht kann
$libs = [ordered]@{
    "et"      = @("lib\et", "error_message et_name init_et com_err com_right")
    "e2p"     = @("lib\e2p", "feature hashstr iod ls ljs mntopts parse_num pe pf ps uuid ostype percent crypto_mode encoding errcode")
    "uuid"    = @("lib\uuid", "clear compare copy gen_uuid isnull pack parse unpack unparse uuid_time")
    "support" = @("lib\support", "cstring mkquota plausible profile parse_qtype print_fs_flags profile_helpers quotaio quotaio_v2 quotaio_tree dict devname")
    "blkid"   = @("lib\blkid", "cache dev devname devno getsize llseek probe read resolve save tag version")
    "ext2fs"  = @("lib\ext2fs", "alloc alloc_sb alloc_stats alloc_tables atexit badblocks bb_inode bitmaps bitops blkmap64_ba blkmap64_rb blknum block bmap check_desc closefs crc16 crc32c csum dblist dblist_dir dirblock dirhash dir_iterate expanddir ext_attr extent fallocate fileio finddev flushb freefs gen_bitmap gen_bitmap64 get_num_dirs get_pathname getenv getsize getsectsize hashmap i_block icount ind_block initialize inline inline_data inode io_manager ismounted link llseek lookup mkdir mkjournal mmp namei native newdir nls_utf8 openfs orphan progress punch qcow2 read_bb read_bb_file res_gdt rw_bitmaps sha512 swapfs symlink undo_io windows_io sparse_io unlink valid_blk version rbtree")
}
$extra = @{ "support" = @("$G\support\prof_err.c"); "ext2fs" = @("$G\ext2fs\ext2_err.c") }

function Compile($c, $objdir, $incdir) {
    $obj = Join-Path $objdir ([IO.Path]::GetFileNameWithoutExtension($c) + ".o")
    if (-not (Test-Path $obj) -or (Get-Item $c).LastWriteTime -gt (Get-Item $obj).LastWriteTime) {
        $out = & $zig cc @cflags "-I$incdir" -c $c -o $obj 2>&1
        if ($LASTEXITCODE -ne 0) { $out | Select-Object -First 10 | Write-Host; throw "Übersetzen fehlgeschlagen: $c" }
    }
    return $obj
}

$archives = @()
foreach ($lib in $libs.Keys) {
    $dir, $files = $libs[$lib]
    $od = Join-Path $O $lib
    New-Item -ItemType Directory -Force $od | Out-Null
    $srcs = @($files -split " " | ForEach-Object { Join-Path $S "$dir\$_.c" }) + @($extra[$lib] | Where-Object { $_ })
    $objs = @($srcs | ForEach-Object { Compile $_ $od (Join-Path $S $dir) })
    $a = Join-Path $O "lib$lib.a"
    Remove-Item $a -ErrorAction SilentlyContinue
    & $zig ar rcs $a @objs
    if ($LASTEXITCODE -ne 0) { throw "Archiv $a fehlgeschlagen" }
    $archives += $a
}
$md = Join-Path $O "misc"
New-Item -ItemType Directory -Force $md | Out-Null
$mobjs = @("$S\misc\mke2fs.c", "$S\misc\util.c", "$S\misc\mk_hugefiles.c", "$S\misc\create_inode.c", "$G\default_profile.c",
           "$here\win_stubs.c") | ForEach-Object { Compile $_ $md "$S\misc" }
# Reihenfolge wie im Makefile: Benutzer vor den Bibliotheken, die sie brauchen
$order = "support", "blkid", "uuid", "ext2fs", "e2p", "et" | ForEach-Object { Join-Path $O "lib$_.a" }
$exe = Join-Path (Split-Path $here) "mke2fs.exe"
& $zig cc -target x86_64-windows-gnu -s -o $exe @mobjs @order
if ($LASTEXITCODE -ne 0) { throw "Linken fehlgeschlagen" }
Remove-Item ([IO.Path]::ChangeExtension($exe, ".pdb")) -ErrorAction SilentlyContinue
Write-Host "Fertig: $exe ($((Get-FileHash -Algorithm SHA256 $exe).Hash.ToLower()))"
