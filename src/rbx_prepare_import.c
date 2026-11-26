/* installer/rbx_import.c built into the game for its --prepare step (RecompLauncher's setup step, see main.c):
 * the same copy and checks as Setup's importer, without its own main. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define RBX_IMPORT_NO_MAIN
#include "../installer/rbx_import.c"
#include <shellapi.h>

/* RecompLauncher's setup step (recomplauncher.json "prepare"):
 *   robotech_bc_recomp.exe --prepare [--game=<disc image, drive or folder>]
 * copies the game files into game_files beside the exe and checks each one, as Setup does (installer/rbx_import.c),
 * with no window. Without --game it only checks that a finished import is there. Prints "progress <0..1> <text>"
 * lines and ends with a plain sentence on failure; exit codes as rbx_import. Returns -1 without --prepare. */
int rbx_prepare_main(void)
{
    int argc, i, r = -1;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const WCHAR *game = NULL;
    WCHAR dest[MAX_PATH], *slash;
    DWORD n;
    if (!argv) return -1;
    for (i = 1; i < argc && wcscmp(argv[i], L"--prepare"); i++) {}
    if (i == argc) { LocalFree(argv); return -1; }
    for (i = 1; i < argc; i++) {
        if (!wcsncmp(argv[i], L"--game=", 7)) game = argv[i] + 7;
        else if (!wcscmp(argv[i], L"--game") && i + 1 < argc) game = argv[++i];
    }
    n = GetModuleFileNameW(NULL, dest, MAX_PATH);
    slash = n && n < MAX_PATH - 12 ? wcsrchr(dest, L'\\') : NULL;
    if (!slash) {
        printf("The game's folder path is too long. Move the game to a shorter folder.\n");
        r = 6;
    } else {
        wcscpy_s(slash, MAX_PATH - (size_t)(slash - dest), L"\\game_files");
        if (game && *game) {
            r = rbx_import_run(game, dest, 1);
        } else if (rbx_import_installed(dest)) {
            printf("progress 1 Already set up\n");
            r = 0;
        } else {
            printf("The game files are not set up yet. Pick your Robotech: Battlecry disc image again.\n");
            r = 2;
        }
    }
    fflush(stdout);
    LocalFree(argv);
    return r;
}
