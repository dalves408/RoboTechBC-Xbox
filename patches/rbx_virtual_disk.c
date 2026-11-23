#include "rbx_virtual_disk.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_BYTES 4096

typedef struct DiskPage {
    struct DiskPage *next;
    uint64_t index;
    unsigned char data[PAGE_BYTES];
} DiskPage;

typedef struct DiskOpen {
    unsigned partition;
    unsigned refs;
    DWORD access;
    uint64_t position;
} DiskOpen;

typedef struct DiskHandle {
    struct DiskHandle *next;
    HANDLE handle;
    DiskOpen *open;
} DiskHandle;

static SRWLOCK disk_lock = SRWLOCK_INIT;
static DiskHandle *handles;
static DiskPage *pages[6];

static DiskHandle **find_handle(HANDLE handle)
{
    DiskHandle **slot = &handles;
    while (*slot && (*slot)->handle != handle)
        slot = &(*slot)->next;
    return slot;
}

static void remove_handle(DiskHandle **slot)
{
    DiskHandle *entry = *slot;
    *slot = entry->next;
    if (--entry->open->refs == 0)
        free(entry->open);
    free(entry);
}

static BOOL fail(DWORD error)
{
    SetLastError(error);
    return FALSE;
}

HANDLE WINAPI rbx_disk_open(LPCWSTR path, DWORD access, DWORD share,
                           LPSECURITY_ATTRIBUTES security, DWORD disposition,
                           DWORD flags, HANDLE template_file)
{
    int partition = rbx_partition_from_path(path);
    if (partition < 0)
        return CreateFileW(path, access, share, security, disposition, flags, template_file);
    if (partition >= 6) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE handle;
    if (flags & FILE_FLAG_BACKUP_SEMANTICS) {
        rbx_open_save_directory(&handle, access, share);
        return handle;
    }

    DiskOpen *open = calloc(1, sizeof(*open));
    DiskHandle *entry = malloc(sizeof(*entry));
    handle = INVALID_HANDLE_VALUE;
    if (open && entry)
        handle = CreateEventW(security, TRUE, TRUE, NULL);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        DWORD error = open && entry ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        free(open);
        free(entry);
        SetLastError(error);
        return INVALID_HANDLE_VALUE;
    }

    open->partition = (unsigned)partition;
    open->refs = 1;
    open->access = access;
    entry->handle = handle;
    entry->open = open;
    AcquireSRWLockExclusive(&disk_lock);
    entry->next = handles;
    handles = entry;
    ReleaseSRWLockExclusive(&disk_lock);
    return handle;
}

static BOOL transfer(DiskOpen *open, void *read_buffer, const void *write_buffer,
                     DWORD count, LPDWORD completed, LPOVERLAPPED offset, BOOL write)
{
    uint64_t at = offset ? ((uint64_t)offset->OffsetHigh << 32) | offset->Offset
                         : open->position;
    uint64_t size = rbx_partition_size(open->partition);
    DWORD allowed = write ? GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA
                          : GENERIC_READ | GENERIC_ALL | FILE_READ_DATA;
    *completed = 0;
    if (!(open->access & allowed))
        return fail(ERROR_ACCESS_DENIED);
    if (!count)
        return TRUE;
    if (write && (at >= size || count > size - at))
        return fail(ERROR_DISK_FULL);
    if (at >= size)
        return fail(ERROR_HANDLE_EOF);
    if (count > size - at)
        count = (DWORD)(size - at);

    while (*completed < count) {
        uint64_t index = at / PAGE_BYTES;
        unsigned start = (unsigned)(at % PAGE_BYTES);
        DWORD bytes = PAGE_BYTES - start;
        if (bytes > count - *completed)
            bytes = count - *completed;
        DiskPage *page = pages[open->partition];
        while (page && page->index != index)
            page = page->next;
        if (!page && (write || (open->partition == 0 && index == 0))) {
            page = calloc(1, sizeof(*page));
            if (!page)
                return fail(ERROR_NOT_ENOUGH_MEMORY);
            page->index = index;
            if (open->partition == 0 && index == 0)
                rbx_partition_seed(page->data);
            page->next = pages[open->partition];
            pages[open->partition] = page;
        }
        if (write)
            memcpy(page->data + start, (const unsigned char *)write_buffer + *completed, bytes);
        else if (page)
            memcpy((unsigned char *)read_buffer + *completed, page->data + start, bytes);
        else
            memset((unsigned char *)read_buffer + *completed, 0, bytes);
        *completed += bytes;
        at += bytes;
        if (!offset)
            open->position = at;
    }
    return TRUE;
}

BOOL WINAPI rbx_disk_read(HANDLE handle, LPVOID buffer, DWORD count,
                          LPDWORD completed, LPOVERLAPPED offset)
{
    AcquireSRWLockExclusive(&disk_lock);
    DiskHandle *entry = *find_handle(handle);
    BOOL result = entry && transfer(entry->open, buffer, NULL, count, completed, offset, FALSE);
    ReleaseSRWLockExclusive(&disk_lock);
    return entry ? result : ReadFile(handle, buffer, count, completed, offset);
}

BOOL WINAPI rbx_disk_write(HANDLE handle, LPCVOID buffer, DWORD count,
                           LPDWORD completed, LPOVERLAPPED offset)
{
    AcquireSRWLockExclusive(&disk_lock);
    DiskHandle *entry = *find_handle(handle);
    BOOL result = entry && transfer(entry->open, NULL, buffer, count, completed, offset, TRUE);
    ReleaseSRWLockExclusive(&disk_lock);
    return entry ? result : WriteFile(handle, buffer, count, completed, offset);
}

BOOL WINAPI rbx_disk_close(HANDLE handle)
{
    AcquireSRWLockExclusive(&disk_lock);
    DiskHandle **slot = find_handle(handle);
    if (*slot)
        remove_handle(slot);
    BOOL result = CloseHandle(handle);
    ReleaseSRWLockExclusive(&disk_lock);
    return result;
}

BOOL WINAPI rbx_disk_duplicate(HANDLE source_process, HANDLE source, HANDLE target_process,
                               LPHANDLE target, DWORD access, BOOL inherit, DWORD options)
{
    AcquireSRWLockExclusive(&disk_lock);
    DiskHandle **slot = find_handle(source);
    if (!*slot) {
        ReleaseSRWLockExclusive(&disk_lock);
        return DuplicateHandle(source_process, source, target_process, target, access, inherit, options);
    }

    DiskHandle *entry = malloc(sizeof(*entry));
    if (!entry) {
        ReleaseSRWLockExclusive(&disk_lock);
        return fail(ERROR_NOT_ENOUGH_MEMORY);
    }
    BOOL result = DuplicateHandle(source_process, source, target_process, target, access,
                                  inherit, options & ~DUPLICATE_CLOSE_SOURCE);
    if (result) {
        entry->handle = *target;
        entry->open = (*slot)->open;
        ++entry->open->refs;
        if (options & DUPLICATE_CLOSE_SOURCE) {
            remove_handle(slot);
            CloseHandle(source);
        }
        entry->next = handles;
        handles = entry;
    } else {
        free(entry);
    }
    ReleaseSRWLockExclusive(&disk_lock);
    return result;
}

BOOL WINAPI rbx_disk_seek(HANDLE handle, LARGE_INTEGER delta, PLARGE_INTEGER position, DWORD origin)
{
    AcquireSRWLockExclusive(&disk_lock);
    DiskHandle *entry = *find_handle(handle);
    if (!entry) {
        ReleaseSRWLockExclusive(&disk_lock);
        return SetFilePointerEx(handle, delta, position, origin);
    }
    DiskOpen *open = entry->open;
    LONGLONG base = 0;
    if (origin == FILE_CURRENT)
        base = (LONGLONG)open->position;
    else if (origin == FILE_END)
        base = (LONGLONG)rbx_partition_size(open->partition);
    BOOL valid = origin <= FILE_END && delta.QuadPart >= -base &&
                 delta.QuadPart <= LLONG_MAX - base;
    if (valid) {
        open->position = base + delta.QuadPart;
        if (position)
            position->QuadPart = (LONGLONG)open->position;
    }
    ReleaseSRWLockExclusive(&disk_lock);
    return valid || fail(ERROR_NEGATIVE_SEEK);
}

BOOL WINAPI rbx_disk_size(HANDLE handle, PLARGE_INTEGER size)
{
    AcquireSRWLockShared(&disk_lock);
    DiskHandle *entry = *find_handle(handle);
    if (entry)
        size->QuadPart = (LONGLONG)rbx_partition_size(entry->open->partition);
    ReleaseSRWLockShared(&disk_lock);
    return entry || GetFileSizeEx(handle, size);
}

BOOL WINAPI rbx_disk_info(HANDLE handle, LPBY_HANDLE_FILE_INFORMATION info)
{
    AcquireSRWLockShared(&disk_lock);
    DiskHandle *entry = *find_handle(handle);
    if (entry) {
        LARGE_INTEGER size;
        size.QuadPart = (LONGLONG)rbx_partition_size(entry->open->partition);
        memset(info, 0, sizeof(*info));
        info->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        info->nNumberOfLinks = 1;
        info->nFileSizeHigh = size.HighPart;
        info->nFileSizeLow = size.LowPart;
    }
    ReleaseSRWLockShared(&disk_lock);
    return entry || GetFileInformationByHandle(handle, info);
}

BOOL WINAPI rbx_disk_flush(HANDLE handle)
{
    AcquireSRWLockShared(&disk_lock);
    BOOL virtual_handle = *find_handle(handle) != NULL;
    ReleaseSRWLockShared(&disk_lock);
    return virtual_handle || FlushFileBuffers(handle);
}

DWORD WINAPI rbx_disk_attributes(LPCWSTR path)
{
    if (rbx_partition_from_path(path) < 0)
        return GetFileAttributesW(path);
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
}

BOOL WINAPI rbx_disk_mkdir(LPCWSTR path, LPSECURITY_ATTRIBUTES security)
{
    if (rbx_partition_from_path(path) >= 0)
        return fail(ERROR_ALREADY_EXISTS);
    return CreateDirectoryW(path, security);
}

BOOL WINAPI rbx_disk_delete(LPCWSTR path)
{
    if (rbx_partition_from_path(path) >= 0)
        return fail(ERROR_ACCESS_DENIED);
    return DeleteFileW(path);
}
