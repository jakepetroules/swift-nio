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

// Extended-attribute support for the NIOFileSystem family on Windows, built on
// NTFS Extended Attributes (EAs) via the ntdll native calls NtQueryEaFile /
// NtSetEaFile. Those calls and their structures are only declared in the DDK
// header <ntifs.h>, which is not part of the Windows SDK shipped with the Swift
// toolchain, so we declare the minimal subset we need here and resolve the two
// entry points dynamically from ntdll (which is always loaded).

#if defined(_WIN32)

#include "CNIOWindows.h"

#include <windows.h>
#include <errno.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

// MARK: - Minimal ntdll declarations

typedef LONG NTSTATUS;

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#define CNIO_STATUS_NO_MORE_EAS         ((NTSTATUS)0x80000012L)
#define CNIO_STATUS_NO_EAS_ON_FILE      ((NTSTATUS)0xC0000052L)
#define CNIO_STATUS_NONEXISTENT_EA_ENTRY ((NTSTATUS)0xC0000051L)
#define CNIO_STATUS_BUFFER_OVERFLOW     ((NTSTATUS)0x80000005L)
#define CNIO_STATUS_BUFFER_TOO_SMALL    ((NTSTATUS)0xC0000023L)
#define CNIO_STATUS_ACCESS_DENIED       ((NTSTATUS)0xC0000022L)
#define CNIO_STATUS_INVALID_PARAMETER   ((NTSTATUS)0xC000000DL)
#define CNIO_STATUS_EAS_NOT_SUPPORTED   ((NTSTATUS)0xC000004FL)
#define CNIO_STATUS_EA_TOO_LARGE        ((NTSTATUS)0xC0000050L)

typedef struct _CNIO_IO_STATUS_BLOCK {
  union {
    NTSTATUS Status;
    PVOID Pointer;
  } DUMMYUNIONNAME;
  ULONG_PTR Information;
} CNIO_IO_STATUS_BLOCK, *PCNIO_IO_STATUS_BLOCK;

// Written by NtQueryEaFile; read by NtSetEaFile.
typedef struct _CNIO_FILE_FULL_EA_INFORMATION {
  ULONG NextEntryOffset;
  UCHAR Flags;
  UCHAR EaNameLength;    // length of EaName, not counting the trailing NUL
  USHORT EaValueLength;  // length of the value, which follows EaName's NUL
  CHAR EaName[1];
} CNIO_FILE_FULL_EA_INFORMATION, *PCNIO_FILE_FULL_EA_INFORMATION;

// Passed to NtQueryEaFile to request a single named attribute.
typedef struct _CNIO_FILE_GET_EA_INFORMATION {
  ULONG NextEntryOffset;
  UCHAR EaNameLength;
  CHAR EaName[1];
} CNIO_FILE_GET_EA_INFORMATION, *PCNIO_FILE_GET_EA_INFORMATION;

typedef NTSTATUS(NTAPI *CNIO_NtQueryEaFile_t)(HANDLE FileHandle,
                                              PCNIO_IO_STATUS_BLOCK IoStatusBlock,
                                              PVOID Buffer, ULONG Length,
                                              BOOLEAN ReturnSingleEntry,
                                              PVOID EaList, ULONG EaListLength,
                                              PULONG EaIndex, BOOLEAN RestartScan);

typedef NTSTATUS(NTAPI *CNIO_NtSetEaFile_t)(HANDLE FileHandle,
                                            PCNIO_IO_STATUS_BLOCK IoStatusBlock,
                                            PVOID Buffer, ULONG Length);

static CNIO_NtQueryEaFile_t CNIOWindows_NtQueryEaFile(void) {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == NULL) {
    return NULL;
  }
  return (CNIO_NtQueryEaFile_t)(void *)GetProcAddress(ntdll, "NtQueryEaFile");
}

static CNIO_NtSetEaFile_t CNIOWindows_NtSetEaFile(void) {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == NULL) {
    return NULL;
  }
  return (CNIO_NtSetEaFile_t)(void *)GetProcAddress(ntdll, "NtSetEaFile");
}

// MARK: - Helpers

// The NTFS total EA size limit is 64 KiB, so a single buffer of this size is
// always large enough to hold either one attribute's value or the full list.
#define CNIO_EA_BUFFER_SIZE (0x10000 + 1024)

static int CNIOWindows_ntStatusToErrno(NTSTATUS status) {
  switch (status) {
  case CNIO_STATUS_NO_EAS_ON_FILE:
  case CNIO_STATUS_NONEXISTENT_EA_ENTRY: return ENODATA;
  case CNIO_STATUS_BUFFER_OVERFLOW:
  case CNIO_STATUS_BUFFER_TOO_SMALL:     return ERANGE;
  case CNIO_STATUS_ACCESS_DENIED:        return EACCES;
  case CNIO_STATUS_INVALID_PARAMETER:    return EINVAL;
  case CNIO_STATUS_EAS_NOT_SUPPORTED:    return ENOTSUP;
  case CNIO_STATUS_EA_TOO_LARGE:         return E2BIG;
  default:                               return EIO;
  }
}

static HANDLE CNIOWindows_handleForFd(int fd) {
  HANDLE handle = (HANDLE)_get_osfhandle(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return INVALID_HANDLE_VALUE;
  }
  return handle;
}

// Transcode a wide (UTF-16) attribute name to a narrow, NUL-terminated buffer.
// Returns the length (excluding the NUL) or -1 with errno set. NTFS caps EA
// names at 254 bytes.
static int CNIOWindows_narrowEaName(const wchar_t *name, char *out, int outCapacity) {
  int length = WideCharToMultiByte(CP_UTF8, 0, name, -1, out, outCapacity, NULL, NULL);
  if (length <= 0) {
    errno = EILSEQ;
    return -1;
  }
  // `length` includes the terminating NUL.
  int nameLength = length - 1;
  if (nameLength > 254) {
    errno = ERANGE;
    return -1;
  }
  return nameLength;
}

// MARK: - fgetxattr

intptr_t CNIOWindows_fgetxattr(int fd, const wchar_t *name, void *value, intptr_t size) {
  HANDLE handle = CNIOWindows_handleForFd(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    return -1;
  }

  CNIO_NtQueryEaFile_t queryEaFile = CNIOWindows_NtQueryEaFile();
  if (queryEaFile == NULL) {
    errno = ENOSYS;
    return -1;
  }

  char narrowName[256];
  int nameLength = CNIOWindows_narrowEaName(name, narrowName, sizeof(narrowName));
  if (nameLength < 0) {
    return -1;
  }

  // Build the FILE_GET_EA_INFORMATION list requesting just this attribute.
  size_t getInfoSize = offsetof(CNIO_FILE_GET_EA_INFORMATION, EaName) + (size_t)nameLength + 1;
  PCNIO_FILE_GET_EA_INFORMATION getInfo = (PCNIO_FILE_GET_EA_INFORMATION)malloc(getInfoSize);
  if (getInfo == NULL) {
    errno = ENOMEM;
    return -1;
  }
  getInfo->NextEntryOffset = 0;
  getInfo->EaNameLength = (UCHAR)nameLength;
  memcpy(getInfo->EaName, narrowName, (size_t)nameLength + 1);

  void *outBuffer = malloc(CNIO_EA_BUFFER_SIZE);
  if (outBuffer == NULL) {
    free(getInfo);
    errno = ENOMEM;
    return -1;
  }

  CNIO_IO_STATUS_BLOCK iosb;
  memset(&iosb, 0, sizeof(iosb));
  NTSTATUS status = queryEaFile(handle, &iosb, outBuffer, CNIO_EA_BUFFER_SIZE,
                                TRUE, getInfo, (ULONG)getInfoSize, NULL, TRUE);
  free(getInfo);

  if (!NT_SUCCESS(status)) {
    free(outBuffer);
    errno = CNIOWindows_ntStatusToErrno(status);
    return -1;
  }

  PCNIO_FILE_FULL_EA_INFORMATION fullInfo = (PCNIO_FILE_FULL_EA_INFORMATION)outBuffer;
  // A named query for an attribute that doesn't exist returns an entry whose
  // value length is zero; there's no way to distinguish that from an empty
  // value via this API, so treat a zero-length result as "no such attribute".
  if (fullInfo->EaValueLength == 0) {
    free(outBuffer);
    errno = ENODATA;
    return -1;
  }

  USHORT valueLength = fullInfo->EaValueLength;
  if (value == NULL || size == 0) {
    // Size query.
    free(outBuffer);
    return (intptr_t)valueLength;
  }
  if ((intptr_t)valueLength > size) {
    free(outBuffer);
    errno = ERANGE;
    return -1;
  }

  const char *valuePtr = fullInfo->EaName + (size_t)fullInfo->EaNameLength + 1;
  memcpy(value, valuePtr, valueLength);
  free(outBuffer);
  return (intptr_t)valueLength;
}

// MARK: - fsetxattr / fremovexattr

// Shared setter: `value`/`size` describe the value to store; a zero-length
// value deletes the attribute on NTFS, which is how removal is implemented.
static int CNIOWindows_setEa(int fd, const wchar_t *name, const void *value, intptr_t size) {
  HANDLE handle = CNIOWindows_handleForFd(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    return -1;
  }

  CNIO_NtSetEaFile_t setEaFile = CNIOWindows_NtSetEaFile();
  if (setEaFile == NULL) {
    errno = ENOSYS;
    return -1;
  }

  if (size < 0 || size > 0xFFFF) {
    errno = EINVAL;
    return -1;
  }

  char narrowName[256];
  int nameLength = CNIOWindows_narrowEaName(name, narrowName, sizeof(narrowName));
  if (nameLength < 0) {
    return -1;
  }

  size_t bufferSize = offsetof(CNIO_FILE_FULL_EA_INFORMATION, EaName)
                      + (size_t)nameLength + 1 + (size_t)size;
  PCNIO_FILE_FULL_EA_INFORMATION fullInfo =
      (PCNIO_FILE_FULL_EA_INFORMATION)calloc(1, bufferSize);
  if (fullInfo == NULL) {
    errno = ENOMEM;
    return -1;
  }

  fullInfo->NextEntryOffset = 0;
  fullInfo->Flags = 0;
  fullInfo->EaNameLength = (UCHAR)nameLength;
  fullInfo->EaValueLength = (USHORT)size;
  memcpy(fullInfo->EaName, narrowName, (size_t)nameLength + 1);
  if (size > 0 && value != NULL) {
    memcpy(fullInfo->EaName + (size_t)nameLength + 1, value, (size_t)size);
  }

  CNIO_IO_STATUS_BLOCK iosb;
  memset(&iosb, 0, sizeof(iosb));
  NTSTATUS status = setEaFile(handle, &iosb, fullInfo, (ULONG)bufferSize);
  free(fullInfo);

  if (!NT_SUCCESS(status)) {
    errno = CNIOWindows_ntStatusToErrno(status);
    return -1;
  }
  return 0;
}

int CNIOWindows_fsetxattr(int fd, const wchar_t *name, const void *value, intptr_t size) {
  return CNIOWindows_setEa(fd, name, value, size);
}

int CNIOWindows_fremovexattr(int fd, const wchar_t *name) {
  // Setting a zero-length value deletes the attribute on NTFS. First confirm it
  // exists so removing an absent attribute reports ENODATA like Linux does.
  intptr_t existing = CNIOWindows_fgetxattr(fd, name, NULL, 0);
  if (existing < 0) {
    return -1;  // errno already set (ENODATA if absent).
  }
  return CNIOWindows_setEa(fd, name, NULL, 0);
}

// MARK: - flistxattr

intptr_t CNIOWindows_flistxattr(int fd, char *namebuf, intptr_t size) {
  HANDLE handle = CNIOWindows_handleForFd(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    return -1;
  }

  CNIO_NtQueryEaFile_t queryEaFile = CNIOWindows_NtQueryEaFile();
  if (queryEaFile == NULL) {
    errno = ENOSYS;
    return -1;
  }

  void *outBuffer = malloc(CNIO_EA_BUFFER_SIZE);
  if (outBuffer == NULL) {
    errno = ENOMEM;
    return -1;
  }

  CNIO_IO_STATUS_BLOCK iosb;
  memset(&iosb, 0, sizeof(iosb));
  NTSTATUS status = queryEaFile(handle, &iosb, outBuffer, CNIO_EA_BUFFER_SIZE,
                                FALSE, NULL, 0, NULL, TRUE);

  if (status == CNIO_STATUS_NO_EAS_ON_FILE) {
    // No attributes: an empty list, not an error.
    free(outBuffer);
    return 0;
  }
  if (!NT_SUCCESS(status)) {
    free(outBuffer);
    errno = CNIOWindows_ntStatusToErrno(status);
    return -1;
  }

  // First pass: compute the total size of the NUL-terminated names.
  intptr_t totalLength = 0;
  const char *cursor = (const char *)outBuffer;
  for (;;) {
    PCNIO_FILE_FULL_EA_INFORMATION entry = (PCNIO_FILE_FULL_EA_INFORMATION)cursor;
    totalLength += (intptr_t)entry->EaNameLength + 1;  // name + NUL
    if (entry->NextEntryOffset == 0) {
      break;
    }
    cursor += entry->NextEntryOffset;
  }

  if (namebuf == NULL || size == 0) {
    // Size query.
    free(outBuffer);
    return totalLength;
  }
  if (totalLength > size) {
    free(outBuffer);
    errno = ERANGE;
    return -1;
  }

  // Second pass: copy each NUL-terminated name into the caller's buffer.
  char *dst = namebuf;
  cursor = (const char *)outBuffer;
  for (;;) {
    PCNIO_FILE_FULL_EA_INFORMATION entry = (PCNIO_FILE_FULL_EA_INFORMATION)cursor;
    memcpy(dst, entry->EaName, entry->EaNameLength);
    dst += entry->EaNameLength;
    *dst++ = '\0';
    if (entry->NextEntryOffset == 0) {
      break;
    }
    cursor += entry->NextEntryOffset;
  }

  free(outBuffer);
  return totalLength;
}

#endif  // defined(_WIN32)
