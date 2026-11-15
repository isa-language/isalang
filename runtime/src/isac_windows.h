#ifndef ISAC_WINDOWS_INTERNAL_H
#define ISAC_WINDOWS_INTERNAL_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#include <io.h>
#include <wchar.h>

static int win_error(DWORD error) {
  switch (error) {
  case ERROR_SUCCESS:
    return 0;
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_ENVVAR_NOT_FOUND:
    return ENOENT;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
  case ERROR_LOCK_VIOLATION:
    return EACCES;
  case ERROR_INVALID_PARAMETER:
  case ERROR_INVALID_NAME:
  case ERROR_BAD_PATHNAME:
    return EINVAL;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    return EEXIST;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    return ENOMEM;
  case ERROR_NO_UNICODE_TRANSLATION:
    return EILSEQ;
  case ERROR_FILENAME_EXCED_RANGE:
    return ENAMETOOLONG;
  case ERROR_DISK_FULL:
    return ENOSPC;
  case ERROR_DIR_NOT_EMPTY:
    return ENOTEMPTY;
  case ERROR_DIRECTORY:
    return ENOTDIR;
  default:
    return EIO;
  }
}
static wchar_t *win_wide(const char *text) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (!n || (size_t)n > SIZE_MAX / sizeof(wchar_t)) {
    errno = n ? EOVERFLOW : win_error(GetLastError());
    return NULL;
  }
  wchar_t *value = isac_rt_alloc((size_t)n * sizeof(wchar_t));
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, value, n)) {
    errno = win_error(GetLastError());
    isac_rt_free(value);
    return NULL;
  }
  return value;
}
static char *win_utf8(const wchar_t *text, size_t *bytes) {
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
  if (!n) {
    errno = win_error(GetLastError());
    return NULL;
  }
  char *value = isac_rt_alloc((size_t)n);
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, value, n, NULL, NULL)) {
    errno = win_error(GetLastError());
    isac_rt_free(value);
    return NULL;
  }
  if (bytes)
    *bytes = (size_t)n - 1;
  return value;
}
static FILE *win_fopen(const char *name, const char *mode) {
  wchar_t *wide = win_wide(name);
  if (!wide)
    return NULL;
  FILE *file = _wfopen(wide, strcmp(mode, "rb") == 0 ? L"rb" : L"wb");
  int error = errno;
  isac_rt_free(wide);
  errno = error;
  return file;
}
static void win_quote(wchar_t *out, size_t *position, const wchar_t *arg) {
  out[(*position)++] = L'"';
  for (;;) {
    size_t slashes = 0;
    while (*arg == L'\\') {
      ++slashes;
      ++arg;
    }
    size_t encoded = (*arg == L'"' || !*arg) ? slashes * 2 : slashes;
    if (*arg == L'"')
      ++encoded;
    while (encoded--)
      out[(*position)++] = L'\\';
    if (!*arg)
      break;
    out[(*position)++] = *arg++;
  }
  out[(*position)++] = L'"';
}
static int64_t win_run(const char *name, const char *arg, int has_argument) {
  wchar_t *program = win_wide(name), *argument = NULL;
  int error = program ? 0 : errno;
  if (!error && has_argument) {
    argument = win_wide(arg);
    if (!argument)
      error = errno;
  }
  if (error) {
    isac_rt_free(program);
    isac_rt_free(argument);
    return -error;
  }
  size_t a = wcslen(program), b = argument ? wcslen(argument) : 0;
  if (a > 32767 || b > 32767) {
    error = E2BIG;
    goto done;
  }

  size_t capacity = 2 * (a + b) + 8;
  wchar_t *line = isac_rt_alloc(capacity * sizeof(wchar_t));
  size_t position = 0;
  win_quote(line, &position, program);
  if (has_argument) {
    line[position++] = L' ';
    win_quote(line, &position, argument);
  }
  line[position] = 0;
  if (position > 32766) {
    isac_rt_free(line);
    error = E2BIG;
    goto done;
  }
  DWORD count = SearchPathW(NULL, program, L".exe", 0, NULL, NULL);
  wchar_t *resolved = NULL;
  while (count) {
    if ((size_t)count > SIZE_MAX / sizeof(wchar_t)) {
      error = EOVERFLOW;
      break;
    }
    resolved = isac_rt_alloc((size_t)count * sizeof(wchar_t));
    DWORD actual = SearchPathW(NULL, program, L".exe", count, resolved, NULL);
    if (actual && actual < count)
      break;
    isac_rt_free(resolved);
    resolved = NULL;
    if (!actual) {
      error = win_error(GetLastError());
      break;
    }
    count = actual;
  }
  if (!resolved) {
    if (!error)
      error = win_error(GetLastError());
    isac_rt_free(line);
    goto done;
  }
  STARTUPINFOW startup = {0};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION child = {0};
  BOOL created = CreateProcessW(resolved, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child);
  if (!created)
    error = win_error(GetLastError());
  isac_rt_free(resolved);
  isac_rt_free(line);
  if (created) {
    CloseHandle(child.hThread);
    DWORD exit_code = 0;
    if (WaitForSingleObject(child.hProcess, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(child.hProcess, &exit_code))
      error = win_error(GetLastError());
    CloseHandle(child.hProcess);
    isac_rt_free(program);
    isac_rt_free(argument);
    return error ? -(int64_t)error : (int64_t)exit_code;
  }
done:
  isac_rt_free(program);
  isac_rt_free(argument);
  return -(int64_t)(error ? error : EIO);
}
#endif
