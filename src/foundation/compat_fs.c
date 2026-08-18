/*
 * compat_fs.c — Portable file system operations.
 *
 * POSIX: direct wrappers around opendir/readdir/closedir, popen/pclose, mkdir, unlink.
 * Windows: FindFirstFile/FindNextFile, _popen/_pclose, _mkdir, _unlink.
 */
#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_fs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

/* ── Windows implementation ────────────────────────────────── */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <direct.h> /* _wmkdir */
#include <errno.h>  /* errno for spawn-failure logging */
#include <fcntl.h>  /* _O_RDONLY */
#include <io.h>     /* _wunlink, _open_osfhandle, _close */
#include <stdint.h> /* intptr_t */
#include "foundation/log.h"
#include "foundation/win_utf8.h"

struct cbm_dir {
    HANDLE find_handle;
    WIN32_FIND_DATAW find_data;
    wchar_t wide_pattern[CBM_PATH_MAX];
    cbm_dirent_t entry;
    bool first;
    bool done;
};

cbm_dir_t *cbm_opendir(const char *path) {
    if (!path) {
        return NULL;
    }
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return NULL;
    }

    size_t wlen = wcslen(wpath);
    if (wlen == 0 || wlen + 2 >= CBM_PATH_MAX) {
        free(wpath);
        return NULL;
    }

    cbm_dir_t *d = (cbm_dir_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_dir_t));
    if (!d) {
        free(wpath);
        return NULL;
    }

    wmemcpy(d->wide_pattern, wpath, wlen + 1);
    wchar_t *p = d->wide_pattern + wlen - SKIP_ONE;
    if (*p != L'\\' && *p != L'/') {
        ++p;
        *p++ = L'\\';
    } else {
        ++p;
    }
    *p++ = L'*';
    *p = L'\0';
    free(wpath);

    d->find_handle = FindFirstFileW(d->wide_pattern, &d->find_data);
    if (d->find_handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = true;
    d->done = false;
    return d;
}

cbm_dirent_t *cbm_readdir(cbm_dir_t *d) {
    if (!d || d->done) {
        return NULL;
    }
    if (!d->first) {
        if (!FindNextFileW(d->find_handle, &d->find_data)) {
            d->done = true;
            return NULL;
        }
    }
    d->first = false;

    while (d->find_data.cFileName[0] == L'.' &&
           (d->find_data.cFileName[1] == L'\0' ||
            (d->find_data.cFileName[1] == L'.' && d->find_data.cFileName[2] == L'\0'))) {
        if (!FindNextFileW(d->find_handle, &d->find_data)) {
            d->done = true;
            return NULL;
        }
    }

    char *u8 = cbm_wide_to_utf8(d->find_data.cFileName);
    if (!u8) {
        d->done = true;
        return NULL;
    }
    size_t nlen = strlen(u8);
    if (nlen >= CBM_DIRENT_NAME_MAX) {
        nlen = CBM_DIRENT_NAME_MAX - SKIP_ONE;
    }
    memcpy(d->entry.name, u8, nlen);
    d->entry.name[nlen] = '\0';
    free(u8);
    d->entry.is_dir = (d->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    d->entry.d_type = 0;
    return &d->entry;
}

void cbm_closedir(cbm_dir_t *d) {
    if (d) {
        if (d->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(d->find_handle);
        }
        free(d);
    }
}

/* Windows _popen replacement that inherits ONLY the child's stdout pipe.
 *
 * The CRT's _popen uses CreateProcess(bInheritHandles=TRUE), which leaks EVERY
 * inheritable handle we hold into the child — listening/client sockets, the
 * Winsock/AFD helper handles created by WSAStartup, the MCP stdio pipe, etc.
 * When the child is git-for-Windows (MSYS2/Cygwin runtime), its startup walks
 * every inherited handle and calls NtQueryObject on each to classify it; on an
 * inherited socket/AFD handle NtQueryObject deadlocks. Since our UI server runs
 * requests on a single thread, that wedges the whole server (list_projects,
 * which shells out to git per project, never returns → the web UI hangs).
 *
 * The fix: spawn via CreateProcessW with STARTUPINFOEXW + an explicit
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST containing only the stdout write-end and a
 * NUL handle for stdin/stderr. Nothing else crosses into git, so there is no
 * foreign handle to deadlock on. POSIX popen() already sets O_CLOEXEC on its
 * pipe, so the POSIX path is unchanged.
 *
 * There is deliberately NO fallback to _popen when the isolated spawn fails:
 * falling back would silently re-arm the deadlock. cbm_popen logs a structured
 * warning and returns NULL instead (every call site handles NULL). */

enum { CBM_POPEN_MAX = 16 };
static struct {
    FILE *fp;
    HANDLE proc;
} g_popen_tab[CBM_POPEN_MAX];
static CRITICAL_SECTION g_popen_lock;
static INIT_ONCE g_popen_once = INIT_ONCE_STATIC_INIT;

/* Test hook (declared in compat_fs_internal.h): 1 when the most recent
 * cbm_popen(..., "r") stream came from the isolated spawn. Test-only
 * observable; not synchronized across threads. */
static volatile LONG g_popen_last_isolated = 0;

int cbm_popen_last_was_isolated(void) {
    return (int)g_popen_last_isolated;
}

static BOOL CALLBACK cbm_popen_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_popen_lock);
    return TRUE;
}

/* Resolve the shell explicitly — %COMSPEC%, else <system dir>\cmd.exe — so it
 * can be passed as lpApplicationName and CreateProcess never walks the search
 * path (no cmd.exe planting from a hostile CWD). Heap string; caller frees. */
static wchar_t *cbm_resolve_comspec(void) {
    wchar_t buf[MAX_PATH];
    const wchar_t suffix[] = L"\\cmd.exe";
    DWORD n = GetEnvironmentVariableW(L"COMSPEC", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        UINT sn = GetSystemDirectoryW(buf, MAX_PATH);
        if (sn == 0 || (size_t)sn + wcslen(suffix) >= MAX_PATH) {
            return NULL;
        }
        wmemcpy(buf + sn, suffix, wcslen(suffix) + 1);
    }
    return _wcsdup(buf);
}

/* On failure returns NULL with *stage naming the failing step and *gle the
 * GetLastError value captured at that step (0 when errno is the signal). */
static FILE *cbm_popen_isolated(const char *cmd, const char **stage, DWORD *gle) {
    *stage = "";
    *gle = 0;
    InitOnceExecuteOnce(&g_popen_once, cbm_popen_init, NULL, NULL);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        *stage = "pipe";
        *gle = GetLastError();
        return NULL;
    }
    /* The parent read-end must never cross into the child. */
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    /* NUL for the child's stdin/stderr so it never touches our real stdin
     * pipe. If NUL cannot be opened, fail: STARTF_USESTDHANDLES slots must
     * never carry INVALID_HANDLE_VALUE. */
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) {
        *stage = "nul";
        *gle = GetLastError();
        CloseHandle(rd);
        CloseHandle(wr);
        return NULL;
    }

    HANDLE inherit[2];
    inherit[0] = wr;
    inherit[1] = nul;

    SIZE_T attr_sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_sz);
    LPPROC_THREAD_ATTRIBUTE_LIST attr = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_sz);
    BOOL attr_init = attr && InitializeProcThreadAttributeList(attr, 1, 0, &attr_sz);
    BOOL prepared =
        attr_init && UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                               sizeof(inherit), NULL, NULL);
    DWORD attr_gle = prepared ? 0 : GetLastError();

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attr;

    /* Run through cmd.exe /c so command quoting and `2>NUL` behave as under
     * _popen. The command line is heap-composed (no fixed-size truncation)
     * and widened via UTF-8 so non-ASCII repo paths survive intact. */
    wchar_t *app = cbm_resolve_comspec();
    wchar_t *wcmdline = NULL;
    if (app) {
        size_t u8len = strlen(cmd) + sizeof("cmd.exe /c ");
        char *u8 = (char *)malloc(u8len);
        if (u8) {
            snprintf(u8, u8len, "cmd.exe /c %s", cmd);
            wcmdline = cbm_utf8_to_wide(u8);
            free(u8);
        }
    }

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL created = FALSE;
    if (!prepared) {
        *stage = "attr";
        *gle = attr_gle;
    } else if (!app || !wcmdline) {
        *stage = "cmdline";
        *gle = ERROR_NOT_ENOUGH_MEMORY;
    } else {
        created = CreateProcessW(app, wcmdline, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                                 NULL, NULL, &si.StartupInfo, &pi);
        if (!created) {
            *stage = "spawn";
            *gle = GetLastError();
        }
    }

    free(app);
    free(wcmdline);
    if (attr) {
        if (attr_init) {
            DeleteProcThreadAttributeList(attr);
        }
        free(attr);
    }
    CloseHandle(wr); /* the child owns the write-end now */
    CloseHandle(nul);
    if (!created) {
        CloseHandle(rd);
        return NULL;
    }
    CloseHandle(pi.hThread);

    int fd = _open_osfhandle((intptr_t)rd, _O_RDONLY);
    if (fd == -1) {
        *stage = "osfhandle";
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        return NULL;
    }
    FILE *fp = _fdopen(fd, "r"); /* takes ownership of fd/rd */
    if (!fp) {
        *stage = "fdopen";
        _close(fd);
        CloseHandle(pi.hProcess);
        return NULL;
    }

    EnterCriticalSection(&g_popen_lock);
    for (int i = 0; i < CBM_POPEN_MAX; i++) {
        if (!g_popen_tab[i].fp) {
            g_popen_tab[i].fp = fp;
            g_popen_tab[i].proc = pi.hProcess;
            LeaveCriticalSection(&g_popen_lock);
            return fp;
        }
    }
    LeaveCriticalSection(&g_popen_lock);
    /* Table full (shouldn't happen): don't leak the process handle. */
    *stage = "table";
    CloseHandle(pi.hProcess);
    fclose(fp);
    return NULL;
}

FILE *cbm_popen(const char *cmd, const char *mode) {
    /* Our git shell-outs are all read-mode; they MUST use the isolated
     * spawn. On failure, log and fail the call — never fall back to
     * _popen, whose full handle inheritance re-arms the UI hang (#798). */
    if (mode && mode[0] == 'r' && mode[1] == '\0') {
        const char *stage = "";
        DWORD gle = 0;
        FILE *fp = cbm_popen_isolated(cmd, &stage, &gle);
        g_popen_last_isolated = (fp != NULL);
        if (!fp) {
            char glebuf[CBM_SZ_16];
            char errnobuf[CBM_SZ_16];
            snprintf(glebuf, sizeof(glebuf), "%lu", (unsigned long)gle);
            snprintf(errnobuf, sizeof(errnobuf), "%d", errno);
            cbm_log_warn("compat.popen_isolated_failed", "stage", stage, "gle", glebuf, "errno",
                         errnobuf);
        }
        return fp;
    }
    g_popen_last_isolated = 0;
    return _popen(cmd, mode);
}

int cbm_pclose(FILE *f) {
    InitOnceExecuteOnce(&g_popen_once, cbm_popen_init, NULL, NULL);

    HANDLE proc = NULL;
    EnterCriticalSection(&g_popen_lock);
    for (int i = 0; i < CBM_POPEN_MAX; i++) {
        if (g_popen_tab[i].fp == f) {
            proc = g_popen_tab[i].proc;
            g_popen_tab[i].fp = NULL;
            g_popen_tab[i].proc = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_popen_lock);

    if (!proc) {
        return _pclose(f); /* opened via _popen (non-read mode) */
    }
    fclose(f);
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    BOOL got = GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    return got ? (int)code : -1;
}

FILE *cbm_fopen(const char *path, const char *mode) {
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return NULL;
    }
    wchar_t *wmode = cbm_utf8_to_wide(mode);
    if (!wmode) {
        free(wpath);
        return NULL;
    }
    FILE *f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
}

/* Stamp the exact current user as owner and apply an owner-only protected
 * DACL on a directory cbm just created. Under the Administrators-default-owner
 * policy (server/runner images), a plain _wmkdir yields an Administrators-owned
 * directory that the launcher/activation exact-owner validators reject. Only
 * freshly created directories are stamped; pre-existing ones keep their owner. */
static void cbm_windows_stamp_dir_owner(const wchar_t *path) {
    HANDLE token = NULL;
    TOKEN_USER *user = NULL;
    PACL acl = NULL;
    DWORD needed = 0U;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
        !GetTokenInformation(token, TokenUser, NULL, 0U, &needed) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && (user = malloc(needed)) != NULL &&
        GetTokenInformation(token, TokenUser, user, needed, &needed) && user->User.Sid &&
        IsValidSid(user->User.Sid)) {
        EXPLICIT_ACCESSW access;
        memset(&access, 0, sizeof(access));
        access.grfAccessPermissions = GENERIC_ALL;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = (LPWSTR)user->User.Sid;
        HANDLE directory =
            CreateFileW(path, WRITE_OWNER | WRITE_DAC | READ_CONTROL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (directory != INVALID_HANDLE_VALUE) {
            if (SetEntriesInAclW(1U, &access, NULL, &acl) == ERROR_SUCCESS) {
                (void)SetSecurityInfo(directory, SE_FILE_OBJECT,
                                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                          PROTECTED_DACL_SECURITY_INFORMATION,
                                      user->User.Sid, NULL, acl, NULL);
            }
            (void)CloseHandle(directory);
        }
    }
    if (acl) {
        (void)LocalFree(acl);
    }
    free(user);
    if (token) {
        (void)CloseHandle(token);
    }
}

static bool cbm_windows_mkdir_component(wchar_t *path) {
    bool created = _wmkdir(path) == 0;
    if (!created && errno != EEXIST) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return false;
    }
    if (created) {
        cbm_windows_stamp_dir_owner(path);
    }
    return true;
}

bool cbm_mkdir_p(const char *path, int mode) {
    (void)mode;
    if (!path || path[0] == '\0') {
        return false;
    }
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return false;
    }

    size_t wlen = wcslen(wpath);
    wchar_t *tmp = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    if (!tmp) {
        free(wpath);
        return false;
    }
    wmemcpy(tmp, wpath, wlen + 1);
    size_t start = wlen > 0U && (tmp[0] == L'/' || tmp[0] == L'\\') ? 1U : 0U;
    if (wlen >= 3U && tmp[1] == L':' && (tmp[2] == L'/' || tmp[2] == L'\\')) {
        start = 3U;
    } else if (wlen >= 2U && (tmp[0] == L'/' || tmp[0] == L'\\') &&
               (tmp[1] == L'/' || tmp[1] == L'\\')) {
        size_t separators = 0U;
        start = 2U;
        while (start < wlen && separators < 2U) {
            if (tmp[start] == L'/' || tmp[start] == L'\\') {
                separators++;
            }
            start++;
        }
    }
    bool ok = true;
    for (wchar_t *p = tmp + start; ok && *p; p++) {
        if (*p == L'/' || *p == L'\\') {
            if (p == tmp || p[-1] == L'/' || p[-1] == L'\\') {
                continue;
            }
            wchar_t separator = *p;
            *p = L'\0';
            ok = cbm_windows_mkdir_component(tmp);
            *p = separator;
        }
    }
    if (ok) {
        ok = cbm_windows_mkdir_component(tmp);
    }
    free(tmp);
    free(wpath);
    return ok;
}

int cbm_unlink(const char *path) {
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    int ret = _wunlink(wpath);
    free(wpath);
    return ret;
}

int cbm_rmdir(const char *path) {
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    int ret = _wrmdir(wpath);
    free(wpath);
    return ret;
}

/* Build a properly-quoted Windows command line from an argv array.
 * Returns a heap-allocated wide string, or NULL on allocation failure.
 * Quoting follows the MSVC CRT convention: arguments containing spaces,
 * tabs, or double-quotes are wrapped in double-quotes, with backslashes
 * before a closing quote doubled and the quote itself escaped. Argument
 * bytes are treated as UTF-8 and converted to wide via cbm_utf8_to_wide,
 * so non-ASCII arguments (e.g. a non-ASCII %USERPROFILE%) survive intact.
 * Declared in compat_fs_internal.h so the test suite can drive it. */
wchar_t *cbm_build_cmdline(const char *const *argv) {
    /* First pass: compute required buffer size. */
    size_t total = 1; /* NUL terminator */
    for (int i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        bool needs_quote = (arg[0] == '\0');
        for (const char *p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                needs_quote = true;
            }
        }
        if (i > 0) {
            total++; /* space separator */
        }
        if (needs_quote) {
            total += 2; /* opening and closing quote */
            size_t backslashes = 0;
            for (const char *p = arg; *p; p++) {
                if (*p == '\\') {
                    backslashes++;
                } else if (*p == '"') {
                    total += backslashes + 1; /* double backslashes + escape backslash */
                    backslashes = 0;
                } else {
                    backslashes = 0;
                }
                total++;
            }
            /* Trailing backslashes before closing quote must be doubled. */
            total += backslashes;
        } else {
            total += strlen(arg);
        }
    }

    /* Build the quoted command line in UTF-8 first, then widen it as a
     * whole via cbm_utf8_to_wide. Every character the quoting logic acts
     * on (space, tab, '"', '\\') is ASCII and, by UTF-8's design, never
     * appears inside a multibyte sequence, so operating on raw bytes here
     * is safe and keeps multibyte argument bytes intact for conversion. */
    char *buf = (char *)malloc(total);
    if (!buf) {
        return NULL;
    }

    /* Second pass: write the command line bytes. */
    char *w = buf;
    for (int i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        bool needs_quote = (arg[0] == '\0');
        for (const char *p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                needs_quote = true;
                break;
            }
        }
        if (i > 0) {
            *w++ = ' ';
        }
        if (needs_quote) {
            *w++ = '"';
            size_t backslashes = 0;
            for (const char *p = arg; *p; p++) {
                if (*p == '\\') {
                    backslashes++;
                    *w++ = '\\';
                } else if (*p == '"') {
                    /* Double the preceding backslashes, then escape the quote. */
                    for (size_t b = 0; b < backslashes; b++) {
                        *w++ = '\\';
                    }
                    *w++ = '\\';
                    *w++ = '"';
                    backslashes = 0;
                } else {
                    backslashes = 0;
                    *w++ = *p;
                }
            }
            /* Double trailing backslashes before the closing quote. */
            for (size_t b = 0; b < backslashes; b++) {
                *w++ = '\\';
            }
            *w++ = '"';
        } else {
            for (const char *p = arg; *p; p++) {
                *w++ = *p;
            }
        }
    }
    *w = '\0';

    wchar_t *out = cbm_utf8_to_wide(buf);
    free(buf);
    return out;
}

int cbm_exec_no_shell(const char *const *argv) {
    if (!argv || !argv[0]) {
        return CBM_NOT_FOUND;
    }

    wchar_t *cmdline = cbm_build_cmdline(argv);
    if (!cmdline) {
        return CBM_NOT_FOUND;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        free(cmdline);
        return CBM_NOT_FOUND;
    }
    free(cmdline);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = (DWORD)CBM_NOT_FOUND;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

int cbm_spawn_capture(const char *exe, const char *const *argv, char **out, size_t *out_len) {
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!exe || !argv || !argv[0] || !out) {
        return CBM_NOT_FOUND;
    }

    /* CreatePipe: stdout pipe with inheritable write end. */
    HANDLE pipe_r, pipe_w;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&pipe_r, &pipe_w, &sa, 0)) {
        return CBM_NOT_FOUND;
    }
    if (!SetHandleInformation(pipe_r, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(pipe_r);
        CloseHandle(pipe_w);
        return CBM_NOT_FOUND;
    }

    /* Resolve exe to a full path via SearchPathW so CreateProcessW's
     * lpApplicationName receives an absolute path (no CWD planting risk).
     * If SearchPathW fails (exe not on PATH), fall back to lpApplicationName=NULL
     * so lpCommandLine's first token does the PATH search instead. */
    wchar_t wexe[CBM_PATH_MAX];
    wchar_t wexe_resolved[CBM_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, CBM_PATH_MAX) <= 0) {
        CloseHandle(pipe_r);
        CloseHandle(pipe_w);
        return CBM_NOT_FOUND;
    }
    DWORD found = SearchPathW(NULL, wexe, L".exe", CBM_PATH_MAX, wexe_resolved, NULL);
    wchar_t *lpApp = (found > 0 && found < CBM_PATH_MAX) ? wexe_resolved : NULL;

    wchar_t cmdline[CBM_SZ_8K];
    int pos = 0;
    for (int i = 0; argv[i]; i++) {
        if (pos >= CBM_SZ_8K - 1)
            goto fail;
        if (i > 0)
            cmdline[pos++] = L' ';
        if (pos >= CBM_SZ_8K - 1)
            goto fail;
        cmdline[pos++] = L'"';
        for (const char *p = argv[i]; *p; p++) {
            wchar_t wc_buf[2];
            int n = MultiByteToWideChar(CP_UTF8, 0, p, 1, wc_buf, 2);
            if (n <= 0)
                continue;
            wchar_t wc = wc_buf[0];
            if (wc == L'"' || wc == L'\\') {
                if (pos >= CBM_SZ_8K - 2)
                    goto fail;
                cmdline[pos++] = L'\\';
            }
            if (pos >= CBM_SZ_8K - 1)
                goto fail;
            cmdline[pos++] = wc;
        }
        if (pos >= CBM_SZ_8K - 1)
            goto fail;
        cmdline[pos++] = L'"';
    }
    cmdline[pos] = L'\0';

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {0};
    BOOL ok =
        CreateProcessW(lpApp, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(pipe_w);
    if (!ok) {
        CloseHandle(pipe_r);
        return CBM_NOT_FOUND;
    }

    char buf[CBM_SZ_4K];
    size_t total = 0;
    char *result = NULL;
    for (;;) {
        DWORD n = 0;
        BOOL rok = ReadFile(pipe_r, buf, sizeof(buf), &n, NULL);
        if (n == 0)
            break;
        char *np = (char *)realloc(result, total + n + 1);
        if (!np) {
            free(result);
            result = NULL;
            total = 0;
            break;
        }
        result = np;
        memcpy(result + total, buf, n);
        total += n;
        result[total] = '\0';
        if (!rok)
            break;
    }
    CloseHandle(pipe_r);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    *out = result; /* NULL when child produced no output; caller initialized *out=NULL above */
    if (out_len)
        *out_len = total;
    return (int)exit_code;

fail:
    CloseHandle(pipe_r);
    CloseHandle(pipe_w);
    return CBM_NOT_FOUND;
}

#else /* POSIX */

/* ── POSIX implementation ────────────────────────────────── */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct cbm_dir {
    DIR *dir;
    cbm_dirent_t entry;
};

cbm_dir_t *cbm_opendir(const char *path) {
    if (!path) {
        return NULL;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return NULL;
    }
    cbm_dir_t *d = (cbm_dir_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_dir_t));
    if (!d) {
        closedir(dir);
        return NULL;
    }
    d->dir = dir;
    return d;
}

cbm_dirent_t *cbm_readdir(cbm_dir_t *d) {
    if (!d || !d->dir) {
        return NULL;
    }
    struct dirent *de;
    while ((de = readdir(d->dir)) != NULL) {
        /* Skip "." and ".." */
        if (de->d_name[0] == '.' &&
            (de->d_name[SKIP_ONE] == '\0' ||
             (de->d_name[SKIP_ONE] == '.' && de->d_name[PAIR_LEN] == '\0'))) {
            continue;
        }
        size_t nlen = strlen(de->d_name);
        if (nlen >= CBM_DIRENT_NAME_MAX) {
            nlen = CBM_DIRENT_NAME_MAX - SKIP_ONE;
        }
        memcpy(d->entry.name, de->d_name, nlen);
        d->entry.name[nlen] = '\0';
        d->entry.is_dir = (de->d_type == DT_DIR);
        d->entry.d_type = de->d_type;
        return &d->entry;
    }
    return NULL;
}

void cbm_closedir(cbm_dir_t *d) {
    if (d) {
        if (d->dir) {
            closedir(d->dir);
        }
        free(d);
    }
}

FILE *cbm_popen(const char *cmd, const char *mode) {
    return popen(cmd, mode);
}

int cbm_pclose(FILE *f) {
    return pclose(f);
}

FILE *cbm_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}

static int cbm_open_directory_component(int parent, const char *component, int flags) {
    int descriptor = openat(parent, component, flags);
#if defined(O_NOFOLLOW) && defined(AT_SYMLINK_NOFOLLOW)
    if (descriptor < 0) {
        struct stat state;
        if (fstatat(parent, component, &state, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(state.st_mode) && state.st_uid == 0U) {
            descriptor = openat(parent, component, flags & ~O_NOFOLLOW);
        }
    }
#endif
    return descriptor;
}

bool cbm_mkdir_p(const char *path, int mode) {
    if (!path || path[0] == '\0') {
        return false;
    }
    char *tmp = strdup(path);
    if (!tmp) {
        return false;
    }

    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int directory = open(path[0] == '/' ? "/" : ".", flags);
    if (directory < 0) {
        free(tmp);
        return false;
    }

    bool ok = true;
    char *cursor = tmp;
    while (*cursor == '/') {
        cursor++;
    }
    while (ok && *cursor) {
        char *separator = strchr(cursor, '/');
        if (separator) {
            *separator = '\0';
        }
        if (cursor[0] != '\0' && strcmp(cursor, ".") != 0) {
            int next = cbm_open_directory_component(directory, cursor, flags);
            if (next < 0 && errno == ENOENT) {
                if (mkdirat(directory, cursor, (mode_t)mode) != 0 && errno != EEXIST) {
                    ok = false;
                } else {
                    next = cbm_open_directory_component(directory, cursor, flags);
                }
            }
            if (ok && next < 0) {
                ok = false;
            }
            if (ok) {
                (void)close(directory);
                directory = next;
            }
        }
        if (!separator) {
            break;
        }
        *separator = '/';
        cursor = separator + 1;
        while (*cursor == '/') {
            cursor++;
        }
    }
    (void)close(directory);
    free(tmp);
    return ok;
}

int cbm_unlink(const char *path) {
    return unlink(path);
}

int cbm_rmdir(const char *path) {
    return rmdir(path);
}

int cbm_exec_no_shell(const char *const *argv) {
    if (!argv || !argv[0]) {
        return CBM_NOT_FOUND;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return CBM_NOT_FOUND;
    }
    if (pid == 0) {
        /* Child: exec directly — no shell interpretation */
        /* 127 = standard "command not found" exit code (POSIX convention) */
        enum { EXEC_NOT_FOUND = 127 };
        execvp(argv[0], (char *const *)argv);
        _exit(EXEC_NOT_FOUND);
    }
    /* Parent: wait for child */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return CBM_NOT_FOUND;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return CBM_NOT_FOUND; /* killed by signal */
}

int cbm_spawn_capture(const char *exe, const char *const *argv, char **out, size_t *out_len) {
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!exe || !argv || !argv[0] || !out) {
        return CBM_NOT_FOUND;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return CBM_NOT_FOUND;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return CBM_NOT_FOUND;
    }
    if (pid == 0) {
        /* Child: wire stdout to pipe write end, then execvp directly. */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);
        execvp(exe, (char *const *)argv);
        _exit(127); /* exec failed */
    }

    /* Parent */
    close(pipefd[1]);

    char buf[CBM_SZ_4K];
    size_t total = 0;
    char *result = NULL;
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0)
            break;
        char *np = (char *)realloc(result, total + (size_t)n + 1);
        if (!np) {
            free(result);
            result = NULL;
            total = 0;
            break;
        }
        result = np;
        memcpy(result + total, buf, (size_t)n);
        total += (size_t)n;
        result[total] = '\0';
    }
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        free(result);
        return CBM_NOT_FOUND;
    }

    *out = result; /* NULL when child produced no output; caller initialized *out=NULL above */
    if (out_len)
        *out_len = total;
    if (WIFEXITED(status)) {
        int ec = WEXITSTATUS(status);
        /* 127 is the POSIX convention for execvp failure (command not found).
         * Map it to CBM_NOT_FOUND so callers can distinguish "exe not found"
         * from "exe ran and exited with code N". */
        return ec == 127 ? CBM_NOT_FOUND : ec;
    }
    free(result);
    return CBM_NOT_FOUND; /* killed by signal */
}

#endif /* _WIN32 */

/* Canonicalize an EXISTING path (collapse `..`, resolve links/junctions):
 * realpath on POSIX; a final path queried from an opened handle on Windows.
 * The previous Windows callers used the ANSI CRT (_access/_fullpath) on UTF-8
 * input — locale-dependent by construction: on a CJK system codepage (e.g.
 * Big5) the UTF-8 bytes of a CJK path re-decode into different characters and
 * canonicalization corrupts the path (#973).  GetFullPathNameW alone is also
 * only lexical and would let an allowed-root check follow a junction outside
 * the root.  Returns 0 when the path does not exist or cannot be resolved. */
int cbm_canonical_path(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz == 0) {
        return 0;
    }
#ifdef _WIN32
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return 0;
    }
    HANDLE handle = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD needed =
        GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0 || needed == MAXDWORD || (size_t)needed > SIZE_MAX / sizeof(wchar_t) - 1) {
        (void)CloseHandle(handle);
        return 0;
    }
    size_t capacity = (size_t)needed + 1;
    wchar_t *wfull = calloc(capacity, sizeof(*wfull));
    if (!wfull) {
        (void)CloseHandle(handle);
        return 0;
    }
    DWORD n = GetFinalPathNameByHandleW(handle, wfull, (DWORD)capacity,
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    (void)CloseHandle(handle);
    if (n == 0 || (size_t)n >= capacity) {
        free(wfull);
        return 0;
    }

    /* Preserve the conventional DOS/UNC form returned by the old API while
     * retaining the handle-based resolution. */
    if (wcsncmp(wfull, L"\\\\?\\UNC\\", 8) == 0) {
        size_t tail_length = wcslen(wfull + 8);
        wmemmove(wfull + 2, wfull + 8, tail_length + 1);
        wfull[0] = L'\\';
        wfull[1] = L'\\';
    } else if (wcsncmp(wfull, L"\\\\?\\", 4) == 0) {
        size_t tail_length = wcslen(wfull + 4);
        wmemmove(wfull, wfull + 4, tail_length + 1);
    }
    char *utf8 = cbm_wide_to_utf8(wfull);
    free(wfull);
    if (!utf8) {
        return 0;
    }
    size_t len = strlen(utf8);
    if (len >= out_sz) {
        free(utf8);
        return 0;
    }
    memcpy(out, utf8, len + 1);
    free(utf8);
    return 1;
#else
    /* Callers pass >= 4K buffers (>= PATH_MAX on our platforms). */
    return realpath(path, out) != NULL;
#endif
}

/* rename() with overwrite semantics on every platform: POSIX rename already
 * replaces atomically; Windows rename fails with EEXIST when the target
 * exists, so use write-through MoveFileExW(MOVEFILE_REPLACE_EXISTING) there
 * (wide paths — raw MoveFileExA would re-mangle non-ASCII cache paths). */
int cbm_rename_replace(const char *src, const char *dst) {
#ifdef _WIN32
    wchar_t *wsrc = cbm_path_to_wide(src);
    wchar_t *wdst = cbm_path_to_wide(dst);
    int ret = CBM_NOT_FOUND;
    if (wsrc && wdst) {
        ret = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                  ? 0
                  : CBM_NOT_FOUND;
    }
    free(wsrc);
    free(wdst);
    return ret;
#else
    return rename(src, dst);
#endif
}

/* Remove a SQLite database's -wal/-shm/-journal sidecars (both platforms). Any code
 * path that installs a FRESH database file at a path where a previous
 * generation lived must call this first: SQLite decides whether to replay a
 * WAL purely from the sidecar's own header/checksums, so a leftover WAL
 * from a crashed session is recovered ON TOP of the freshly installed file
 * at the next open, splicing old-generation pages into it (#897). */
int cbm_remove_db_sidecars(const char *db_path) {
    if (!db_path || !db_path[0]) {
        return CBM_NOT_FOUND;
    }
    enum { SIDECAR_PATH_MAX = 4096 };
    char side[SIDECAR_PATH_MAX];
    /* Validate the longest suffix before unlinking anything. Otherwise a
     * near-limit path can remove -wal/-shm, silently skip a truncated
     * -journal, and report success after partially mutating the generation. */
    if (strlen(db_path) > sizeof(side) - sizeof("-journal")) {
        return CBM_NOT_FOUND;
    }
    int result = 0;
    int n = snprintf(side, sizeof(side), "%s-wal", db_path);
    if (n <= 0 || (size_t)n >= sizeof(side)) {
        return CBM_NOT_FOUND;
    }
    errno = 0;
    int unlink_rc = cbm_unlink(side);
    int unlink_error = errno;
    if (unlink_rc != 0 && unlink_error != ENOENT) {
        result = CBM_NOT_FOUND;
    }
    n = snprintf(side, sizeof(side), "%s-shm", db_path);
    if (n <= 0 || (size_t)n >= sizeof(side)) {
        return CBM_NOT_FOUND;
    }
    errno = 0;
    unlink_rc = cbm_unlink(side);
    unlink_error = errno;
    if (unlink_rc != 0 && unlink_error != ENOENT) {
        result = CBM_NOT_FOUND;
    }
    n = snprintf(side, sizeof(side), "%s-journal", db_path);
    if (n <= 0 || (size_t)n >= sizeof(side)) {
        return CBM_NOT_FOUND;
    }
    errno = 0;
    unlink_rc = cbm_unlink(side);
    unlink_error = errno;
    if (unlink_rc != 0 && unlink_error != ENOENT) {
        result = CBM_NOT_FOUND;
    }
    return result;
}
/* ── Cross-platform helpers (validated output extraction) ───────
 *
 * Pure functions: no spawn, no IPC. Unit-testable in isolation. */

#include <ctype.h>
#include "foundation/log.h"

/* ── Common validators (used by git_capture, artifact.c, etc.) ───
 *
 * Pure functions: take one trimmed stdout line, decide whether it could be
 * the real output of a known subcommand. Reject chcp banners, GCM hints,
 * hook output, and anything else that doesn't match the expected schema. */

bool cbm_validator_sha40_hex(const char *line, void *ctx) {
    (void)ctx;
    if (!line || strlen(line) != 40) {
        return false;
    }
    for (int i = 0; i < 40; i++) {
        if (!isxdigit((unsigned char)line[i])) {
            return false;
        }
    }
    return true;
}

bool cbm_validator_git_path(const char *line, void *ctx) {
    (void)ctx;
    if (!line || !line[0]) {
        return false;
    }
    /* Accept `.git` (worktree-relative), /abs/path, C:\...\, \\server\share. */
    if (strcmp(line, ".git") == 0) {
        return true;
    }
#ifdef _WIN32
    if (isalpha((unsigned char)line[0]) && line[1] == ':' && (line[2] == '\\' || line[2] == '/')) {
        return true;
    }
    if (line[0] == '\\' || line[0] == '/') {
        return true;
    }
#else
    if (line[0] == '/') {
        return true;
    }
#endif
    return false;
}

bool cbm_validator_branch_name(const char *line, void *ctx) {
    (void)ctx;
    if (!line || !line[0]) {
        return false;
    }
    /* Branch names never contain spaces, tabs, or git ref meta-characters. */
    for (const char *p = line; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == ':' || *p == '?' || *p == '*' || *p == '[' ||
            *p == '\\' || *p == '~' || *p == '^') {
            return false;
        }
    }
    return true;
}

int cbm_find_validated_line(const char *text, cbm_line_validator_t validator, void *ctx,
                            char **out) {
    if (out)
        *out = NULL;
    if (!text || !validator || !out) {
        return CBM_NOT_FOUND;
    }

    const char *line_start = text;
    while (1) {
        const char *line_end = strchr(line_start, '\n');
        size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        /* Copy into a scratch buffer with trailing \r/\n trimmed. */
        char tmp[CBM_SZ_4K];
        size_t copy_len = len;
        if (copy_len >= sizeof(tmp)) {
            copy_len = sizeof(tmp) - 1;
        }
        memcpy(tmp, line_start, copy_len);
        tmp[copy_len] = '\0';
        size_t tlen = strlen(tmp);
        while (tlen > 0 && (tmp[tlen - 1] == '\r' || tmp[tlen - 1] == '\n')) {
            tmp[--tlen] = '\0';
        }

        if (validator(tmp, ctx)) {
            char *dup = (char *)malloc(strlen(tmp) + 1);
            if (!dup)
                return CBM_NOT_FOUND;
            strcpy(dup, tmp);
            *out = dup;
            return 0;
        }

        if (!line_end)
            break;
        line_start = line_end + 1;
    }

    return CBM_NOT_FOUND;
}

int cbm_spawn_capture_validated(const char *exe, const char *const *argv, char **out,
                                cbm_line_validator_t validator, void *ctx) {
    if (out)
        *out = NULL;
    if (!exe || !argv || !out) {
        return CBM_NOT_FOUND;
    }

    char *raw = NULL;
    size_t raw_len = 0;
    int rc = cbm_spawn_capture(exe, argv, &raw, &raw_len);
    if (rc != 0) {
        free(raw);
        return CBM_NOT_FOUND;
    }
    if (!raw) {
        return CBM_NOT_FOUND;
    }

    /* No validator → return raw output as-is. */
    if (!validator) {
        *out = raw;
        return 0;
    }

    char *match = NULL;
    int vrc = cbm_find_validated_line(raw, validator, ctx, &match);
    if (vrc != 0) {
        /* Fail loud: no line passed validation. Surface the first line +
         * total line count so the user can diagnose the pollution source
         * (chcp banner, GCM hint, git hook output, etc.). Never fuzzy-match. */
        char first_line[CBM_SZ_256];
        size_t i = 0;
        for (; i < raw_len && i < sizeof(first_line) - 1 && raw[i] != '\n'; i++) {
            first_line[i] = raw[i];
        }
        first_line[i] = '\0';

        size_t lines = 1;
        for (size_t j = 0; j < raw_len; j++) {
            if (raw[j] == '\n')
                lines++;
        }
        cbm_log_warn("spawn.validation_failed", "exe", exe, "first_line", first_line, "total_lines",
                     (int)lines);
        free(raw);
        return CBM_NOT_FOUND;
    }

    free(raw);
    *out = match;
    return 0;
}
