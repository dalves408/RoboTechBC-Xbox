#pragma once

#include <windows.h>
#include <stdint.h>

int rbx_partition_from_path(const wchar_t *path);
BOOL rbx_open_save_directory(HANDLE *handle, DWORD access, DWORD share);
uint64_t rbx_partition_size(unsigned partition);
void rbx_partition_seed(unsigned char page[4096]);

HANDLE WINAPI rbx_disk_open(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL WINAPI rbx_disk_read(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI rbx_disk_write(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI rbx_disk_close(HANDLE);
BOOL WINAPI rbx_disk_seek(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
BOOL WINAPI rbx_disk_info(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
BOOL WINAPI rbx_disk_size(HANDLE, PLARGE_INTEGER);
BOOL WINAPI rbx_disk_flush(HANDLE);
BOOL WINAPI rbx_disk_duplicate(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
DWORD WINAPI rbx_disk_attributes(LPCWSTR);
BOOL WINAPI rbx_disk_mkdir(LPCWSTR, LPSECURITY_ATTRIBUTES);
BOOL WINAPI rbx_disk_delete(LPCWSTR);
