/* rbx_import: copies the game's files out of the player's own copy of the disc (after the Halo 2 recomp's
 * halo2_import). Setup runs it from its temp folder; it is not installed.
 *
 *   rbx_import <source> <dest_dir>
 *
 * <source> is an Xbox disc image (.iso: a full Redump image or a plain XISO), a drive letter, or a folder holding
 * the disc's files (the one with default.xbe). The disc's default.xbe must be the release this build was made from
 * (DISC_XBE_SHA256 in manifest.h); anything else stops before a file is written.
 *
 * Every file in manifest.h is written to <dest_dir> and checked against its pinned size and SHA-256 as it is copied;
 * nothing else is. Progress goes to stdout as "PROGRESS done total", errors as "ERROR <sentence>".
 * A file already in <dest_dir> with the right size and hash is kept (a repair reads, it does not copy again).
 * <dest_dir>\.rbx-imported is removed first and written last, only when every file is in place and checked: Setup
 * takes it as the sign of a finished install, so a run cut short never looks finished.
 *
 * Exit codes: 0 ok, 2 source not found, 3 not an Xbox disc image, 4 not the supported disc, 5 files missing or
 * damaged, 6 cannot write. */
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "manifest.h"

#define SECTOR 2048ull
static const unsigned long long XDVDFS_OFFSETS[] = { 0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull };

typedef struct { unsigned long long sector, size; int found; } DiscFile;

static unsigned long long g_done, g_total;
static int g_have[MANIFEST_COUNT];
static WCHAR g_dest[MAX_PATH];
static HANDLE g_img = INVALID_HANDLE_VALUE;
static unsigned long long g_base, g_img_len;
static DiscFile g_disc[MANIFEST_COUNT];
static unsigned long long g_xbe_sector, g_xbe_size;
static int g_xbe_found;
static int g_launcher;   /* RecompLauncher's setup step: "progress <0..1> <text>" lines and plain error sentences */

#define ERR(msg) printf("%s" msg, g_launcher ? "" : "ERROR ")

static void progress(void)
{
    if (g_launcher)
        printf("progress %.4f Copying and checking the game files\n", g_total ? (double)g_done / (double)g_total : 1.0);
    else
        printf("PROGRESS %llu %llu\n", g_done, g_total);
    fflush(stdout);
}

static int manifest_find(const char *rel)
{
    int i;
    for (i = 0; i < MANIFEST_COUNT; i++)
        if (_stricmp(MANIFEST[i].path, rel) == 0) return i;
    return -1;
}

static void rel_w(const char *rel, WCHAR *out)
{
    WCHAR *p;
    MultiByteToWideChar(CP_ACP, 0, rel, -1, out, MAX_PATH);
    for (p = out; *p; p++) if (*p == L'/') *p = L'\\';
}

static void path_in(const WCHAR *dir, const char *rel, WCHAR *out)
{
    WCHAR r[MAX_PATH];
    rel_w(rel, r);
    swprintf_s(out, MAX_PATH, L"%s\\%s", dir, r);
}

static int make_dirs_for(const WCHAR *file)
{
    WCHAR dir[MAX_PATH], *sl;
    wcscpy_s(dir, MAX_PATH, file);
    if (!(sl = wcsrchr(dir, L'\\'))) return 1;
    *sl = 0;
    return SHCreateDirectoryExW(NULL, dir, NULL) == ERROR_SUCCESS || GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES;
}

/* ---- SHA-256 ------------------------------------------------------------------------------------------------- */
typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; } Sha;

static int sha_begin(Sha *s)
{
    if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    if (BCryptCreateHash(s->alg, &s->h, NULL, 0, NULL, 0, 0)) { BCryptCloseAlgorithmProvider(s->alg, 0); return 0; }
    return 1;
}

static void sha_end(Sha *s, unsigned char out[32])
{
    BCryptFinishHash(s->h, out, 32, 0);
    BCryptDestroyHash(s->h);
    BCryptCloseAlgorithmProvider(s->alg, 0);
}

/* Read `size` bytes from `in` (at its current position), hash them and, when `dst` is given, write them there.
 * 1: the bytes match `want`; 0: they don't (or the read failed: a short dump); -1: the write failed. A wrong or
 * failed copy is deleted. `count` adds the bytes to the progress. */
static int copy_checked(HANDLE in, unsigned long long size, const unsigned char want[32], const WCHAR *dst, int count)
{
    static unsigned char buf[1 << 20];
    unsigned char got[32];
    unsigned long long left = size;
    HANDLE out = INVALID_HANDLE_VALUE;
    Sha s;
    int ok = 1;
    if (dst) {
        if (!make_dirs_for(dst)) return -1;
        out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) return -1;
    }
    if (!sha_begin(&s)) { if (dst) { CloseHandle(out); DeleteFileW(dst); } return 0; }
    while (left) {
        DWORD n = left > sizeof buf ? (DWORD)sizeof buf : (DWORD)left, got_n = 0, wr = 0;
        if (!ReadFile(in, buf, n, &got_n, NULL) || got_n != n) { ok = 0; break; }
        BCryptHashData(s.h, buf, n, 0);
        if (dst && (!WriteFile(out, buf, n, &wr, NULL) || wr != n)) { ok = -1; break; }
        left -= n;
        if (count) {
            g_done += n;
            if ((g_done >> 23) != ((g_done - n) >> 23)) progress();     /* every 8 MB */
        }
    }
    sha_end(&s, got);
    if (dst) CloseHandle(out);
    if (ok == 1 && memcmp(got, want, 32)) ok = 0;
    if (ok != 1 && dst) DeleteFileW(dst);
    return ok;
}

/* The file is already in the destination with the pinned size and hash. */
static int dest_has(int i)
{
    WCHAR path[MAX_PATH];
    HANDLE f;
    LARGE_INTEGER sz;
    int ok = 0;
    path_in(g_dest, MANIFEST[i].path, path);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (GetFileSizeEx(f, &sz) && (unsigned long long)sz.QuadPart == MANIFEST[i].size)
        ok = copy_checked(f, MANIFEST[i].size, MANIFEST[i].sha256, NULL, 1) == 1;
    CloseHandle(f);
    return ok;
}

/* ---- disc image ---------------------------------------------------------------------------------------------- */
static int img_read(unsigned long long off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    if (off + n > g_img_len || off + n < off) return 0;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(g_img, li, NULL, FILE_BEGIN) && ReadFile(g_img, buf, n, &got, NULL) && got == n;
}

static int find_xdvdfs(unsigned long long *root_sector, unsigned long long *root_size)
{
    unsigned char v[SECTOR];
    size_t i;
    for (i = 0; i < sizeof XDVDFS_OFFSETS / sizeof XDVDFS_OFFSETS[0]; i++) {
        if (img_read(XDVDFS_OFFSETS[i] + 32 * SECTOR, v, sizeof v) && !memcmp(v, "MICROSOFT*XBOX*MEDIA", 20) &&
            !memcmp(v + SECTOR - 20, "MICROSOFT*XBOX*MEDIA", 20)) {
            unsigned int s, z;
            memcpy(&s, v + 20, 4); memcpy(&z, v + 24, 4);
            g_base = XDVDFS_OFFSETS[i];
            *root_sector = s; *root_size = z;
            return 1;
        }
    }
    return 0;
}

/* One directory table: a binary tree of entries (child offsets in dwords; 0xFFFF pads to the next sector). Records
 * where each manifest file and default.xbe are. Every extent must lie inside the image. */
static int walk(unsigned long long sector, unsigned long long size, const char *prefix, int depth)
{
    unsigned char *d;
    unsigned int *stack, sp = 0, cap;
    unsigned char *seen;
    int ok = 1;
    if (!size) return 1;
    if (depth > 16 || size > (64u << 20)) return 0;
    d = (unsigned char *)malloc((size_t)size);
    seen = (unsigned char *)calloc((size_t)size / 4 + 1, 1);
    cap = (unsigned int)(size / 4 + 2);
    stack = (unsigned int *)malloc(cap * sizeof *stack);
    if (!d || !seen || !stack || !img_read(g_base + sector * SECTOR, d, (DWORD)size)) { ok = 0; goto out; }
    stack[sp++] = 0;
    while (ok && sp) {
        unsigned int pos = stack[--sp], start, count;
        unsigned short left, right;
        unsigned char attr, nl;
        char name[MAX_PATH];
        int i;
        if (pos + 14 > size || seen[pos / 4]) { ok = 0; break; }
        seen[pos / 4] = 1;
        if (d[pos] == 0xFF && d[pos + 1] == 0xFF) {
            if (pos == 0) continue;                               /* empty directory */
            pos += (unsigned int)(SECTOR - pos % SECTOR);          /* padding: next sector */
            if (pos < size) stack[sp++] = pos;
            continue;
        }
        memcpy(&left, d + pos, 2); memcpy(&right, d + pos + 2, 2);
        memcpy(&start, d + pos + 4, 4); memcpy(&count, d + pos + 8, 4);
        attr = d[pos + 12]; nl = d[pos + 13];
        if (!nl || pos + 14u + nl > size || strlen(prefix) + nl + 2 > sizeof name) { ok = 0; break; }
        sprintf_s(name, sizeof name, "%s%.*s", prefix, (int)nl, (const char *)d + pos + 14);
        for (i = (int)strlen(prefix); name[i]; i++)
            if ((unsigned char)name[i] < 0x20 || strchr("\\:*?\"<>|", name[i])) { ok = 0; break; }
        if (!ok) break;
        if (attr & 0x10) {
            char sub[MAX_PATH];
            sprintf_s(sub, sizeof sub, "%s/", name);
            if (!walk(start, count, sub, depth + 1)) { ok = 0; break; }
        } else {
            int k;
            if (g_base + (unsigned long long)start * SECTOR + count > g_img_len) { ok = 0; break; }
            if (!_stricmp(name, "default.xbe")) { g_xbe_found = 1; g_xbe_sector = start; g_xbe_size = count; }
            if ((k = manifest_find(name)) >= 0) { g_disc[k].found = 1; g_disc[k].sector = start; g_disc[k].size = count; }
        }
        if (sp + 2 > cap) { ok = 0; break; }
        if (right) stack[sp++] = (unsigned int)right * 4;
        if (left) stack[sp++] = (unsigned int)left * 4;
    }
out:
    free(d); free(seen); free(stack);
    return ok;
}

static int seek_img(unsigned long long off)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(g_img, li, NULL, FILE_BEGIN);
}

static int import_image(const WCHAR *image)
{
    unsigned long long rs, rsz;
    LARGE_INTEGER len;
    int i, missing = 0;
    g_img = CreateFileW(image, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (g_img == INVALID_HANDLE_VALUE) { ERR("The disc image could not be opened.\n"); return 2; }
    GetFileSizeEx(g_img, &len);
    g_img_len = (unsigned long long)len.QuadPart;
    if (!find_xdvdfs(&rs, &rsz)) { ERR("This is not an Xbox disc image.\n"); return 3; }
    if (!walk(rs, rsz, "", 0)) { ERR("The disc image is damaged or incomplete.\n"); return 5; }
    if (!g_xbe_found || g_xbe_size != DISC_XBE_SIZE || !seek_img(g_base + g_xbe_sector * SECTOR) ||
        copy_checked(g_img, g_xbe_size, DISC_XBE_SHA256, NULL, 0) != 1) {
        ERR("This is not the supported " GAME_NAME " disc (" GAME_RELEASE "): its default.xbe differs.\n");
        return 4;
    }
    progress();
    for (i = 0; i < MANIFEST_COUNT; i++) {
        WCHAR dst[MAX_PATH];
        int r;
        if (dest_has(i)) { g_have[i] = 1; continue; }
        if (!g_disc[i].found || g_disc[i].size != MANIFEST[i].size) {
            printf("MISSING %s\n", MANIFEST[i].path); missing++; g_done += MANIFEST[i].size; continue;
        }
        path_in(g_dest, MANIFEST[i].path, dst);
        if (!seek_img(g_base + g_disc[i].sector * SECTOR)) { missing++; continue; }
        r = copy_checked(g_img, MANIFEST[i].size, MANIFEST[i].sha256, dst, 1);
        if (r < 0) { ERR("Could not write to the install folder.\n"); return 6; }
        if (r) g_have[i] = 1; else { printf("DAMAGED %s\n", MANIFEST[i].path); missing++; }
    }
    progress();
    if (missing) { printf("%s%d game files are missing or damaged (bad or incomplete dump).\n", g_launcher ? "" : "ERROR ", missing); return 5; }
    return 0;
}

/* ---- folder or drive ----------------------------------------------------------------------------------------- */
/* The folder holding default.xbe: the source itself or one level below it (a drive's or a zip's top folder). */
static int find_root(const WCHAR *dir, WCHAR *root)
{
    WCHAR t[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(t, MAX_PATH, L"%s\\default.xbe", dir);
    if (GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) { wcscpy_s(root, MAX_PATH, dir); return 1; }
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
            swprintf_s(t, MAX_PATH, L"%s\\%s\\default.xbe", dir, fd.cFileName);
            if (GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) {
                swprintf_s(root, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                FindClose(h);
                return 1;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

static int import_folder(const WCHAR *source)
{
    WCHAR root[MAX_PATH], xbe[MAX_PATH];
    HANDLE f;
    LARGE_INTEGER sz;
    int i, missing = 0, ok = 0;
    if (!find_root(source, root)) { ERR("default.xbe was not found in that folder.\n"); return 4; }
    swprintf_s(xbe, MAX_PATH, L"%s\\default.xbe", root);
    f = CreateFileW(xbe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(f, &sz) && (unsigned long long)sz.QuadPart == DISC_XBE_SIZE)
            ok = copy_checked(f, DISC_XBE_SIZE, DISC_XBE_SHA256, NULL, 0) == 1;
        CloseHandle(f);
    }
    if (!ok) {
        ERR("This is not the supported " GAME_NAME " disc (" GAME_RELEASE "): its default.xbe differs.\n");
        return 4;
    }
    progress();
    for (i = 0; i < MANIFEST_COUNT; i++) {
        WCHAR src[MAX_PATH], dst[MAX_PATH];
        int r = 0;
        if (dest_has(i)) { g_have[i] = 1; continue; }
        path_in(root, MANIFEST[i].path, src);
        path_in(g_dest, MANIFEST[i].path, dst);
        f = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f == INVALID_HANDLE_VALUE) { printf("MISSING %s\n", MANIFEST[i].path); missing++; g_done += MANIFEST[i].size; continue; }
        if (GetFileSizeEx(f, &sz) && (unsigned long long)sz.QuadPart == MANIFEST[i].size)
            r = copy_checked(f, MANIFEST[i].size, MANIFEST[i].sha256, dst, 1);
        else
            g_done += MANIFEST[i].size;
        CloseHandle(f);
        if (r < 0) { ERR("Could not write to the install folder.\n"); return 6; }
        if (r) g_have[i] = 1; else { printf("DAMAGED %s\n", MANIFEST[i].path); missing++; }
    }
    progress();
    if (missing) { printf("%s%d game files are missing or damaged (bad or incomplete dump).\n", g_launcher ? "" : "ERROR ", missing); return 5; }
    return 0;
}

/* Copies and checks every manifest file from source into dest. launcher: print RecompLauncher's progress lines. */
int rbx_import_run(const WCHAR *source, const WCHAR *dest, int launcher)
{
    WCHAR stamp[MAX_PATH];
    DWORD attr;
    size_t n;
    int r;
    g_launcher = launcher;
    g_done = 0; g_total = MANIFEST_BYTES;
    wcscpy_s(g_dest, MAX_PATH, dest);
    n = wcslen(g_dest);
    while (n > 3 && (g_dest[n - 1] == L'\\' || g_dest[n - 1] == L'/')) g_dest[--n] = 0;
    attr = GetFileAttributesW(source);
    if (attr == INVALID_FILE_ATTRIBUTES) { ERR("The disc image or folder was not found.\n"); return 2; }
    if (SHCreateDirectoryExW(NULL, g_dest, NULL) != ERROR_SUCCESS && GetFileAttributesW(g_dest) == INVALID_FILE_ATTRIBUTES) {
        ERR("Could not write to the install folder.\n");
        return 6;
    }
    swprintf_s(stamp, MAX_PATH, L"%s\\.rbx-imported", g_dest);
    DeleteFileW(stamp);
    r = (attr & FILE_ATTRIBUTE_DIRECTORY) ? import_folder(source) : import_image(source);
    if (g_img != INVALID_HANDLE_VALUE) { CloseHandle(g_img); g_img = INVALID_HANDLE_VALUE; }
    if (!r) {
        HANDLE f = CreateFileW(stamp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        static const char text[] = "rbx_import: every manifest file copied and checked\r\n";
        DWORD wr;
        if (f == INVALID_HANDLE_VALUE || !WriteFile(f, text, sizeof text - 1, &wr, NULL)) {
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            ERR("Could not write to the install folder.\n");
            return 6;
        }
        CloseHandle(f);
        if (launcher) printf("progress 1 Game files ready\n");
        else printf("OK %d files\n", MANIFEST_COUNT);
    }
    return r;
}

/* A finished import is in dest: the stamp is there and every manifest file has its pinned size. Quick: no hashing. */
int rbx_import_installed(const WCHAR *dest)
{
    WCHAR path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    int i;
    swprintf_s(path, MAX_PATH, L"%s\\.rbx-imported", dest);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;
    for (i = 0; i < MANIFEST_COUNT; i++) {
        path_in(dest, MANIFEST[i].path, path);
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) ||
            (((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) != MANIFEST[i].size)
            return 0;
    }
    return 1;
}

#ifndef RBX_IMPORT_NO_MAIN
int wmain(int argc, wchar_t **argv)
{
    if (argc != 3) {
        printf("usage: rbx_import <disc image, drive or folder> <dest folder>\n");
        return 2;
    }
    return rbx_import_run(argv[1], argv[2], 0);
}
#endif
