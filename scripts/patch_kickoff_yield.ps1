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

if ($changed) {
    [IO.File]::WriteAllText($path, $t)
    Write-Output "patched $path"
} else {
    Write-Output "already patched: $path"
}
