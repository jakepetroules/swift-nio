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
#include <fcntl.h>
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

// MARK: - openat / *at family

// Recover the full path behind a directory descriptor. Writes up to `capacity`
// wchars (including the NUL) into `out`; returns the length (excluding NUL) or
// -1 with errno set.
static int CNIOWindows_pathForFd(int dirfd, wchar_t *out, DWORD capacity) {
  HANDLE handle = (HANDLE)_get_osfhandle(dirfd);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  // FILE_NAME_NORMALIZED returns a "\\?\"-prefixed path, which is fine to feed
  // straight back into the wide Win32 file APIs.
  DWORD length = GetFinalPathNameByHandleW(handle, out, capacity, FILE_NAME_NORMALIZED);
  if (length == 0) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  if (length >= capacity) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return (int)length;
}

// Join `dirfd`'s path with the relative child `name` into `out`. Returns 0 or
// -1 with errno set.
static int CNIOWindows_joinAt(int dirfd, const wchar_t *name, wchar_t *out, size_t capacity) {
  int baseLength = CNIOWindows_pathForFd(dirfd, out, (DWORD)capacity);
  if (baseLength < 0) {
    return -1;
  }
  size_t nameLength = wcslen(name);
  // base + '\\' + name + NUL
  if ((size_t)baseLength + 1 + nameLength + 1 > capacity) {
    errno = ENAMETOOLONG;
    return -1;
  }
  out[baseLength] = L'\\';
  memcpy(out + baseLength + 1, name, (nameLength + 1) * sizeof(wchar_t));
  return 0;
}

int CNIOWindows_openat(int dirfd, const wchar_t *name, int oflag, int windowsFlags, uint32_t mode) {
  wchar_t path[32768];  // max long-path length
  if (CNIOWindows_joinAt(dirfd, name, path, sizeof(path) / sizeof(wchar_t)) != 0) {
    return -1;
  }

  // Access mode from the low CRT bits.
  DWORD access = 0;
  switch (oflag & (_O_RDONLY | _O_WRONLY | _O_RDWR)) {
  case _O_WRONLY: access = GENERIC_WRITE; break;
  case _O_RDWR:   access = GENERIC_READ | GENERIC_WRITE; break;
  default:        access = GENERIC_READ; break;
  }
  if (oflag & _O_APPEND) {
    access |= FILE_APPEND_DATA;
  }

  // Creation disposition from the CRT create/excl/trunc bits.
  DWORD disposition;
  BOOL create = (oflag & _O_CREAT) != 0;
  BOOL excl = (oflag & _O_EXCL) != 0;
  BOOL trunc = (oflag & _O_TRUNC) != 0;
  if (create && excl) {
    disposition = CREATE_NEW;
  } else if (create && trunc) {
    disposition = CREATE_ALWAYS;
  } else if (create) {
    disposition = OPEN_ALWAYS;
  } else if (trunc) {
    disposition = TRUNCATE_EXISTING;
  } else {
    disposition = OPEN_EXISTING;
  }

  DWORD attributes = FILE_ATTRIBUTE_NORMAL;
  // FILE_FLAG_BACKUP_SEMANTICS is required to obtain a handle to a directory.
  if (windowsFlags & CNIO_O_DIRECTORY) {
    attributes |= FILE_FLAG_BACKUP_SEMANTICS;
  }
  // noFollow: open the reparse point itself rather than its target.
  if (windowsFlags & CNIO_O_NOFOLLOW) {
    attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
  }
  // A read-only created file gets the read-only attribute when mode lacks write.
  if ((disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
      && (mode & 0200) == 0) {
    attributes = (attributes & ~(DWORD)FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_READONLY;
  }

  SECURITY_ATTRIBUTES security;
  security.nLength = sizeof(security);
  security.lpSecurityDescriptor = NULL;
  // closeOnExec maps to a non-inheritable handle.
  security.bInheritHandle = (windowsFlags & CNIO_O_CLOEXEC) ? FALSE : TRUE;

  HANDLE handle = CreateFileW(path, access,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              &security, disposition, attributes, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }

  // Wrap the Win32 HANDLE in a CRT file descriptor so the rest of NIOFS (which
  // works in terms of fds) can use it.
  int crtFlags = _O_BINARY;
  if ((oflag & (_O_RDONLY | _O_WRONLY | _O_RDWR)) == _O_RDONLY) {
    crtFlags |= _O_RDONLY;
  }
  if (oflag & _O_APPEND) {
    crtFlags |= _O_APPEND;
  }
  int fd = _open_osfhandle((intptr_t)handle, crtFlags);
  if (fd == -1) {
    CloseHandle(handle);
    errno = EMFILE;
    return -1;
  }
  return fd;
}

int CNIOWindows_unlinkat(int dirfd, const wchar_t *name, int removeDir) {
  wchar_t path[32768];
  if (CNIOWindows_joinAt(dirfd, name, path, sizeof(path) / sizeof(wchar_t)) != 0) {
    return -1;
  }
  BOOL ok = removeDir ? RemoveDirectoryW(path) : DeleteFileW(path);
  if (!ok) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  return 0;
}

int CNIOWindows_symlink(const wchar_t *target, const wchar_t *linkPath) {
  // Choose the link type from the target: if it resolves to an existing
  // directory, create a directory symlink. SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
  // lets this succeed under Developer Mode without elevation.
  DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  DWORD targetAttrs = GetFileAttributesW(target);
  if (targetAttrs != INVALID_FILE_ATTRIBUTES && (targetAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
    flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
  }
  if (!CreateSymbolicLinkW(linkPath, target, flags)) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }
  return 0;
}

int CNIOWindows_symlinkat(const wchar_t *target, int dirfd, const wchar_t *linkPath) {
  wchar_t joined[32768];
  if (CNIOWindows_joinAt(dirfd, linkPath, joined, sizeof(joined) / sizeof(wchar_t)) != 0) {
    return -1;
  }
  return CNIOWindows_symlink(target, joined);
}

intptr_t CNIOWindows_readlink(const wchar_t *path, wchar_t *buffer, intptr_t size) {
  // Open the reparse point itself and let GetFinalPathNameByHandleW would follow
  // it, so instead read the raw reparse data and extract the print name.
  HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }

  // REPARSE_DATA_BUFFER can be up to MAXIMUM_REPARSE_DATA_BUFFER_SIZE bytes.
  BYTE reparseData[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
  DWORD returned = 0;
  BOOL ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0,
                            reparseData, sizeof(reparseData), &returned, NULL);
  CloseHandle(handle);
  if (!ok) {
    errno = CNIOWindows_fsErrno(GetLastError());
    return -1;
  }

  // Interpret the buffer via the CNIOWindows_REPARSE_DATA_BUFFER declared in the
  // header. Only symbolic-link reparse points carry a symlink target.
  CNIOWindows_PREPARSE_DATA_BUFFER reparse = (CNIOWindows_PREPARSE_DATA_BUFFER)reparseData;
  const WCHAR *nameBase;
  USHORT nameOffset;
  USHORT nameLength;
  if (reparse->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
    nameBase = reparse->SymbolicLinkReparseBuffer.PathBuffer;
    nameOffset = reparse->SymbolicLinkReparseBuffer.PrintNameOffset;
    nameLength = reparse->SymbolicLinkReparseBuffer.PrintNameLength;
  } else if (reparse->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
    nameBase = reparse->MountPointReparseBuffer.PathBuffer;
    nameOffset = reparse->MountPointReparseBuffer.PrintNameOffset;
    nameLength = reparse->MountPointReparseBuffer.PrintNameLength;
  } else {
    errno = EINVAL;
    return -1;
  }

  intptr_t wcharCount = nameLength / (intptr_t)sizeof(WCHAR);
  if (wcharCount > size) {
    wcharCount = size;  // truncate, matching readlink(2)
  }
  memcpy(buffer, (const BYTE *)nameBase + nameOffset, (size_t)wcharCount * sizeof(WCHAR));
  return wcharCount;
}

#endif  // defined(_WIN32)
