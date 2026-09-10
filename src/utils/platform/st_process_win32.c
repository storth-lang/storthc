#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

#include "../st_process.h"

static void st_append_quoted_arg(wchar_t *dst, size_t *pos, const wchar_t *arg) {
    size_t n = wcslen(arg);
    b8 needs_quotes = (n == 0) || wcspbrk(arg, L" \t\"") != NULL;

    if (!needs_quotes) {
        wmemcpy(dst + *pos, arg, n);
        *pos += n;
        return;
    }

    dst[(*pos)++] = L'"';
    for (size_t i = 0; i < n; i++) {
        size_t backslashes = 0;
        while (i < n && arg[i] == L'\\') {
            backslashes++;
            i++;
        }
        if (i == n) {
            for (size_t k = 0; k < backslashes * 2; k++)
                dst[(*pos)++] = L'\\';
            break;
        }
        if (arg[i] == L'"') {
            for (size_t k = 0; k < backslashes * 2 + 1; k++)
                dst[(*pos)++] = L'\\';
            dst[(*pos)++] = L'"';
        } else {
            for (size_t k = 0; k < backslashes; k++)
                dst[(*pos)++] = L'\\';
            dst[(*pos)++] = arg[i];
        }
    }
    dst[(*pos)++] = L'"';
}

static wchar_t *st_build_cmdline(ST_arena_t *arena, const char *const *args) {
    if (!args || !args[0])
        return NULL;

    size_t total = 1;
    int max_wlen = 0;
    for (int i = 0; args[i] != NULL; i++) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, args[i], -1, NULL, 0);
        if (wlen <= 0)
            return NULL;
        if (wlen > max_wlen)
            max_wlen = wlen;
        total += (size_t)wlen * 2 + 3;
    }

    wchar_t *cmdline = ST_arena_push(arena, (u64)(total * sizeof(wchar_t)));
    wchar_t *warg = ST_arena_push(arena, (u64)((size_t)max_wlen * sizeof(wchar_t)));

    size_t pos = 0;
    for (int i = 0; args[i] != NULL; i++) {
        MultiByteToWideChar(CP_UTF8, 0, args[i], -1, warg, max_wlen);
        if (i > 0)
            cmdline[pos++] = L' ';
        st_append_quoted_arg(cmdline, &pos, warg);
    }
    cmdline[pos] = L'\0';
    return cmdline;
}

b8 ST_run_process(ST_proc_t *proc) {
    fflush(NULL);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL inherit = FALSE;
    if (proc->opt.out) {
        fflush(proc->opt.out);
        HANDLE h = (HANDLE)_get_osfhandle(_fileno(proc->opt.out));
        if (h != INVALID_HANDLE_VALUE) {
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = h;
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            inherit = TRUE;
        }
    }

    wchar_t *cmdline = st_build_cmdline(proc->arena, proc->opt.args);
    if (!cmdline)
        return 0;

    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, inherit, 0, NULL, NULL, &si, &pi);

    if (!ok) {
        DWORD err = GetLastError();
        char msg[256];
        DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                 err, 0, msg, sizeof(msg), NULL);
        while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n'))
            msg[--n] = 0;
        fprintf(stderr, "storthc: could not start '%s': %s (error %lu)\n", proc->opt.args[0],
                n ? msg : "unknown error", (unsigned long)err);
        proc->id = INVALID_HANDLE_VALUE;
        proc->exited_normally = 0;
        proc->exit_code = -1;
        return 0;
    }

    CloseHandle(pi.hThread);
    proc->id = pi.hProcess;

    if (proc->opt.async)
        return 1;
    return ST_wait_process(proc);
}

b8 ST_wait_process(ST_proc_t *proc) {
    if (proc->id == NULL || proc->id == INVALID_HANDLE_VALUE)
        return 1;

    HANDLE h = (HANDLE)proc->id;
    proc->id = INVALID_HANDLE_VALUE;

    if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) {
        CloseHandle(h);
        proc->exited_normally = 0;
        return 0;
    }

    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);

    proc->exit_code = (i32)code;
    proc->exited_normally = 1;
    return code == 0;
}

b8 ST_path_is_absolute(const char *path) {
    if (!path || !path[0])
        return 0;
    b8 has_drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                   path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    b8 is_unc = (path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/');
    return has_drive || is_unc;
}

char ST_path_separator(void) {
    return '\\';
}
