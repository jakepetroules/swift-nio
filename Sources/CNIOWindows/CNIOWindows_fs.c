//===----------------------------------------------------------------------===//
//
// This source file is part of the SwiftNIO open source project
//
// Copyright (c) 2026 Apple Inc. and the SwiftNIO project authors
// Licensed under Apache License v2.0
//
// See LICENSE.txt for license information
// See CONTRIBUTORS.txt for the list of SwiftNIO project authors
//
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

// Filesystem and CRT syscall shims backing the POSIX-shaped stubs used by the
// NIOFileSystem family on Windows.

#if defined(_WIN32)

// getenv / strerror are flagged deprecated by the Windows CRT in favour of the
// _s variants; we intentionally expose the POSIX-shaped calls here.
#define _CRT_SECURE_NO_WARNINGS 1

#include "CNIOWindows.h"

#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

// POSIX file-type mode bits. Windows <sys/stat.h> only defines a subset (and
// not S_IFLNK), so define the ones we set ourselves.
#define CNIO_S_IFMT  0170000
#define CNIO_S_IFDIR 0040000
#define CNIO_S_IFREG 0100000
#define CNIO_S_IFLNK 0120000
#include <sys/stat.h>

// MARK: - Shared helpers

static int CNIOWindows_fsErrno(DWORD error) {
  switch (error) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:      return ENOENT;
  case ERROR_ACCESS_DENIED:       return EACCES;
  case ERROR_FILE_EXISTS:
  case ERROR_ALREADY_EXISTS:      return EEXIST;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:         return ENOMEM;
  case ERROR_INVALID_PARAMETER:   return EINVAL;
  case ERROR_DIR_NOT_EMPTY:       return ENOTEMPTY;
  case ERROR_PRIVILEGE_NOT_HELD:  return EACCES;
  case ERROR_SHARING_VIOLATION:   return EACCES;
  default:                        return EIO;
  }
}

// Windows FILETIME (100 ns ticks since 1601-01-01) <-> Unix epoch.
#define CNIO_FILETIME_UNIX_EPOCH_OFFSET 116444736000000000LL

static void CNIOWindows_fileTimeToUnix(FILETIME ft, int64_t *sec, int64_t *nsec) {
  int64_t ticks = ((int64_t)ft.dwHighDateTime << 32) | (int64_t)ft.dwLowDateTime;
  ticks -= CNIO_FILETIME_UNIX_EPOCH_OFFSET;
  *sec = ticks / 10000000LL;
  *nsec = (ticks % 10000000LL) * 100;
}

static FILETIME CNIOWindows_unixToFileTime(int64_t sec, int64_t nsec) {
  int64_t ticks = sec * 10000000LL + nsec / 100 + CNIO_FILETIME_UNIX_EPOCH_OFFSET;
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFF);
  ft.dwHighDateTime = (DWORD)((ticks >> 32) & 0xFFFFFFFF);
  return ft;
}

// MARK: - Scalar CRT / Win32 wrappers

int CNIOWindows_fsync(int fd) {
  HANDLE handle = (HANDLE)_get_osfhandle(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  if (!FlushFileBuffers(handle)) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  return 0;
}

char *CNIOWindows_getenv(const char *name) {
  return getenv(name);
}

wchar_t *CNIOWindows_getcwd(wchar_t *buffer, int size) {
  return _wgetcwd(buffer, size);
}

char *CNIOWindows_strerror(int code) {
  return strerror(code);
}

int CNIOWindows_mkdir(const wchar_t *path) {
  if (!CreateDirectoryW(path, NULL)) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  return 0;
}

int CNIOWindows_unlink(const wchar_t *path) {
  if (!DeleteFileW(path)) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  return 0;
}

int CNIOWindows_remove(const wchar_t *path) {
  if (DeleteFileW(path)) {
    return 0;
  }
  DWORD error = GetLastError();
  // DeleteFileW fails on directories with ERROR_ACCESS_DENIED; try to remove it
  // as a directory before reporting the error.
  if (error == ERROR_ACCESS_DENIED && RemoveDirectoryW(path)) {
    return 0;
  }
  errno = CNIOWindows_fsErrno(error);
  return -1;
}

int CNIOWindows_link(const wchar_t *existing, const wchar_t *newLink) {
  if (!CreateHardLinkW(newLink, existing, NULL)) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  return 0;
}

// MARK: - Fiber Local Storage (pthread key emulation)

int CNIOWindows_fls_key_create(uint32_t *key, void (*destructor)(void *)) {
  DWORD index = FlsAlloc(destructor);
  if (index == FLS_OUT_OF_INDEXES) {
    errno = EAGAIN;
    return -1;
  }
  *key = (uint32_t)index;
  return 0;
}

int CNIOWindows_fls_set(uint32_t key, const void *value) {
  if (!FlsSetValue((DWORD)key, (PVOID)value)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

void *CNIOWindows_fls_get(uint32_t key) {
  return FlsGetValue((DWORD)key);
}

// MARK: - stat family

static void CNIOWindows_fillStatFromHandle(HANDLE handle,
                                           const BY_HANDLE_FILE_INFORMATION *info,
                                           CNIOWindows_stat_t *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = info->dwVolumeSerialNumber;
  out->st_ino = ((uint64_t)info->nFileIndexHigh << 32) | (uint64_t)info->nFileIndexLow;
  out->st_nlink = info->nNumberOfLinks;
  out->st_size = ((uint64_t)info->nFileSizeHigh << 32) | (uint64_t)info->nFileSizeLow;

  // Map attributes onto a POSIX-ish mode. Windows has no per-file permission
  // bits comparable to POSIX; report read+execute always, add write unless the
  // read-only attribute is set, and mark directories vs regular vs symlink.
  uint32_t mode;
  if (info->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    mode = CNIO_S_IFLNK;
  } else if (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    mode = CNIO_S_IFDIR;
  } else {
    mode = CNIO_S_IFREG;
  }
  if (info->dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
    mode |= 0555;
  } else {
    mode |= 0777;
  }
  out->st_mode = mode;

  CNIOWindows_fileTimeToUnix(info->ftLastAccessTime, &out->st_atim_sec, &out->st_atim_nsec);
  CNIOWindows_fileTimeToUnix(info->ftLastWriteTime, &out->st_mtim_sec, &out->st_mtim_nsec);
  CNIOWindows_fileTimeToUnix(info->ftCreationTime, &out->st_ctim_sec, &out->st_ctim_nsec);
}

int CNIOWindows_fstat(int fd, CNIOWindows_stat_t *out) {
  HANDLE handle = (HANDLE)_get_osfhandle(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(handle, &info)) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  CNIOWindows_fillStatFromHandle(handle, &info, out);
  return 0;
}

int CNIOWindows_stat(const wchar_t *path, int followSymlinks, CNIOWindows_stat_t *out) {
  DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;  // required to open directories
  if (!followSymlinks) {
    flags |= FILE_FLAG_OPEN_REPARSE_POINT;
  }
  HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, flags, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(handle, &info)) {
    errno = CNIOWindows_fsErrno(GetLastError());
    CloseHandle(handle);
    return -1;
  }
  CNIOWindows_fillStatFromHandle(handle, &info, out);
  CloseHandle(handle);
  return 0;
}

int CNIOWindows_fchmod(int fd, uint32_t mode) {
  HANDLE handle = (HANDLE)_get_osfhandle(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  FILE_BASIC_INFO basic;
  if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  // Only the owner-write bit is representable via FILE_ATTRIBUTE_READONLY.
  if (mode & 0200) {
    basic.FileAttributes &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
  } else {
    basic.FileAttributes |= FILE_ATTRIBUTE_READONLY;
  }
  if (basic.FileAttributes == 0) {
    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
  }
  if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic))) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  return 0;
}

int CNIOWindows_futimens(int fd, int64_t atimeSec, int64_t atimeNsec,
                         int64_t mtimeSec, int64_t mtimeNsec) {
  HANDLE handle = (HANDLE)_get_osfhandle(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  // Sentinel seconds values from the Swift layer, mirroring UTIME_OMIT/UTIME_NOW:
  //   -1 -> leave the timestamp unchanged (pass NULL to SetFileTime)
  //   -2 -> set to the current time
  // Any other (non-negative) value is a literal Unix timestamp.
  FILETIME now;
  GetSystemTimeAsFileTime(&now);

  FILETIME atime, mtime;
  FILETIME *atimePtr = NULL;
  FILETIME *mtimePtr = NULL;
  if (atimeSec == -2) {
    atime = now;
    atimePtr = &atime;
  } else if (atimeSec >= 0) {
    atime = CNIOWindows_unixToFileTime(atimeSec, atimeNsec);
    atimePtr = &atime;
  }
  if (mtimeSec == -2) {
    mtime = now;
    mtimePtr = &mtime;
  } else if (mtimeSec >= 0) {
    mtime = CNIOWindows_unixToFileTime(mtimeSec, mtimeNsec);
    mtimePtr = &mtime;
  }
  if (!SetFileTime(handle, NULL, atimePtr, mtimePtr)) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  return 0;
}

#endif  // defined(_WIN32)
