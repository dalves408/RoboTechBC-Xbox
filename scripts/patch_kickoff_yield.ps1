# Apply RBX-17 KickOff PFB/GET yield hooks to lifted gen (gitignored).
# Safe to re-run after regenerating src/recomp/gen.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$path = Join-Path $root 'src\recomp\gen\recomp_0022.c'
if (-not (Test-Path $path)) { throw "missing $path" }
$t = [IO.File]::ReadAllText($path)
$changed = $false

if ($t -notmatch 'rbx_d3d_pfb_spin_yield') {
    $insert = @"
/* RBX-17 KickOff BusyLoop helpers (defined in recomp_manual.c). */
void rbx_d3d_pfb_spin_yield(void);
void rbx_d3d_kick_get_yield(void);

"@
    if ($t.Contains("#include <math.h>`r`n")) {
        $t = $t.Replace("#include <math.h>`r`n", "#include <math.h>`r`n`r`n$insert")
    } else {
        $t = $t.Replace("#include <math.h>`n", "#include <math.h>`n`n$insert")
    }
    $changed = $true
}

function Apply-Replace([ref]$text, [string]$old, [string]$new, [ref]$flag) {
    if ($text.Value.Contains($old)) {
        $text.Value = $text.Value.Replace($old, $new)
        $flag.Value = $true
    }
}

$oldShort = @"
loc_00155710: ;
    _fa = (uint32_t)(MEM32(eax + 0x100410)) & 0xFFFFFFFFu; _fb = (uint32_t)(0x10000) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test MEM32(eax + 0x100410), 0x10000 (32-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_00155710; /* jne: not equal / not zero */
"@
$newShort = @"
loc_00155710: ;
    _fa = (uint32_t)(MEM32(eax + 0x100410)) & 0xFFFFFFFFu; _fb = (uint32_t)(0x10000) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test MEM32(eax + 0x100410), 0x10000 (32-bit) */
    /* RBX-17: same PFB yield as KickOff (0x1556F0 short kick). */
    if (TEST_NZ(_fa, _fb)) { rbx_d3d_pfb_spin_yield(); goto loc_00155710; }
"@
Apply-Replace ([ref]$t) $oldShort $newShort ([ref]$changed)

$oldPfb = @"
loc_00155774: ;
    _fa = (uint32_t)(MEM32(eax + 0x100410)) & 0xFFFFFFFFu; _fb = (uint32_t)(0x10000) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test MEM32(eax + 0x100410), 0x10000 (32-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_00155774; /* jne: not equal / not zero */
"@
$newPfb = @"
loc_00155774: ;
    _fa = (uint32_t)(MEM32(eax + 0x100410)) & 0xFFFFFFFFu; _fb = (uint32_t)(0x10000) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test MEM32(eax + 0x100410), 0x10000 (32-bit) */
    /* RBX-17: yield + ack so flip_held/sleeping ack cannot BusyLoop the guest. */
    if (TEST_NZ(_fa, _fb)) { rbx_d3d_pfb_spin_yield(); goto loc_00155774; }
"@
Apply-Replace ([ref]$t) $oldPfb $newPfb ([ref]$changed)

if ($t -notmatch 'rbx_d3d_kick_get_yield\(\); goto loc_001557A0') {
    $oldGet = @"
loc_001557A5: ;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_001557A0; /* jne: not equal / not zero */
"@
    $newGet = @"
loc_001557A5: ;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    /* RBX-17: if still busy after the yielding busy-check, force GET catch-up. */
    if (CMP_NE(_fa, _fb)) { rbx_d3d_kick_get_yield(); goto loc_001557A0; }
"@
    Apply-Replace ([ref]$t) $oldGet $newGet ([ref]$changed)
}

# BlockOnFence (0x1558D0) is wrapped by sub_001558D0 in recomp_manual.c, which
# re-checks the fence after D3D's event wait (spurious wakes overwrote the ring).
if ($t -notmatch 'void sub_001558D0_orig\(void\)') {
    $re = [regex]'void sub_001558D0\(void\)(\r?\n)\{'
    if ($re.IsMatch($t)) {
        $t = $re.Replace($t, '/* RBX: renamed; wrapped by sub_001558D0 in src/recomp_manual.c (BlockOnFence recheck). */$1void sub_001558D0_orig(void)$1{', 1)
        $changed = $true
    }
}

if ($changed) {
    [IO.File]::WriteAllText($path, $t)
    Write-Output "patched $path"
} else {
    Write-Output "already patched: $path"
}

# 0x1166C (shared epilogue of 0x11370/0x11424) is implemented in
# recomp_manual.c; the lifter's unresolved stub for it never restored edi,
# which froze the mission's fixed-step loop.
$stubs = Join-Path $root 'src\recomp\gen\recomp_stubs_unresolved.c'
if (Test-Path $stubs) {
    $st = [IO.File]::ReadAllText($stubs)
    $old = 'void sub_0001166C(void) {'
    if ($st.Contains($old)) {
        $st = $st.Replace($old, 'void sub_0001166C_stub(void) { /* RBX: real one in src/recomp_manual.c */')
        [IO.File]::WriteAllText($stubs, $st)
        Write-Output "patched $stubs"
    } else {
        Write-Output "already patched: $stubs"
    }
}

# The camera's frustum builder (0x12B670) is wrapped by sub_0012B670 in
# recomp_manual.c, which widens its x extents for widescreen (side pop-in).
$gen = Join-Path $root 'src\recomp\gen'
$hit = $false
foreach ($f in Get-ChildItem -Path $gen -Filter 'recomp_*.c') {
    $ft = [IO.File]::ReadAllText($f.FullName)
    if ($ft -match 'void sub_0012B670_orig\(void\)') { $hit = $true; Write-Output "already patched: $($f.FullName) (0x12B670)"; break }
    $re = [regex]'void sub_0012B670\(void\)(\s*)\{'
    if ($re.IsMatch($ft)) {
        $ft = $re.Replace($ft, '/* RBX: renamed; wrapped by sub_0012B670 in src/recomp_manual.c (widescreen culling). */' + "`n" + 'void sub_0012B670_orig(void)$1{', 1)
        [IO.File]::WriteAllText($f.FullName, $ft)
        Write-Output "patched $($f.FullName) (0x12B670)"
        $hit = $true
        break
    }
}
if (-not $hit) { throw "sub_0012B670 definition not found in $gen" }

# isCheat (0x2F350) is wrapped by sub_0002F350 in recomp_manual.c, which
# reports the level-select cheat as on in -DRBX_DEBUG_UNLOCK_ALL_LEVELS=ON
# builds (a plain pass-through otherwise). The lifter's file split can move,
# so look for the definition in every gen file.
$gen = Join-Path $root 'src\recomp\gen'
$cheatRe = [regex]'void sub_0002F350\(void\)(\r?\n)\{'
$cheatDone = $false
foreach ($f in Get-ChildItem $gen -Filter 'recomp_*.c') {
    $ct = [IO.File]::ReadAllText($f.FullName)
    if ($ct.Contains('void sub_0002F350_orig(void)')) { $cheatDone = $true; Write-Output "already patched: $($f.FullName) (isCheat)"; break }
    if ($cheatRe.IsMatch($ct)) {
        $ct = $cheatRe.Replace($ct, '/* RBX: renamed; wrapped by sub_0002F350 in src/recomp_manual.c (debug level unlock). */$1void sub_0002F350_orig(void)$1{', 1)
        [IO.File]::WriteAllText($f.FullName, $ct)
        $cheatDone = $true
        Write-Output "patched $($f.FullName) (isCheat)"
        break
    }
}
if (-not $cheatDone) { throw 'sub_0002F350 (isCheat) not found in src\recomp\gen' }

# The pause menu's loop (0x242B0) and its update/draw (0x24060) are wrapped
# in recomp_manual.c, which holds each pass of that loop to 30 a second.
foreach ($va in @('000242B0', '00024060')) {
    $re = [regex]('void sub_' + $va + '\(void\)(\s*)\{')
    $done = $false
    foreach ($f in Get-ChildItem $gen -Filter 'recomp_*.c') {
        $ft = [IO.File]::ReadAllText($f.FullName)
        if ($ft.Contains('void sub_' + $va + '_orig(void)')) { $done = $true; Write-Output "already patched: $($f.FullName) (0x$va)"; break }
        if ($re.IsMatch($ft)) {
            $ft = $re.Replace($ft, '/* RBX: renamed; wrapped in src/recomp_manual.c (pause menu pace). */' + "`n" + 'void sub_' + $va + '_orig(void)$1{', 1)
            [IO.File]::WriteAllText($f.FullName, $ft)
            Write-Output "patched $($f.FullName) (0x$va)"
            $done = $true
            break
        }
    }
    if (-not $done) { throw "sub_$va definition not found in $gen" }
}
