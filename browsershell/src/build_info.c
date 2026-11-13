/* build_info.c - project URL, engine version/hash and a privacy-free launch
 * summary, shared by the settings About section and the crash report. */
#include "build_info.h"

#include <windows.h>
#include <winver.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define BUILD_INFO_TEXT 2048

static char s_version[64];
static int  s_versionDone;
static char s_hash[65];
static int  s_hashDone;
static char s_args[BUILD_INFO_TEXT];
static int  s_argsDone;
static char s_env[BUILD_INFO_TEXT];
static int  s_envDone;

static void append(char* out, size_t cap, size_t* len, const char* fmt, ...)
{
    va_list args;
    int n;

    if (*len + 1 >= cap)
        return;

    va_start(args, fmt);
    n = _vsnprintf(out + *len, cap - *len - 1, fmt, args);
    va_end(args);

    if (n < 0)
        return;
    if ((size_t)n >= cap - *len - 1)
        n = (int)(cap - *len - 2);

    *len += (size_t)n;
    out[*len] = 0;
}

static int engineModulePath(wchar_t* out, DWORD cap)
{
    HMODULE module = GetModuleHandleA("mb132_x64.dll");
    wchar_t exe[MAX_PATH];
    wchar_t* slash;

    if (module && GetModuleFileNameW(module, out, cap))
        return 1;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return 0;

    slash = wcsrchr(exe, L'\\');
    if (slash)
        slash[1] = 0;
    else
        exe[0] = 0;

    _snwprintf(out, cap, L"%s%s", exe, L"mb132_x64.dll");
    out[cap - 1] = 0;
    return 1;
}

static void fileVersion(const wchar_t* path, char* out, size_t cap)
{
    static unsigned char block[8 * 1024];
    DWORD dummy = 0;
    DWORD size;
    VS_FIXEDFILEINFO* fixed = NULL;
    UINT fixedLen = 0;

    out[0] = 0;

    size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size || size > sizeof(block))
        return;

    if (GetFileVersionInfoW(path, 0, size, block) &&
        VerQueryValueW(block, L"\\", (LPVOID*)&fixed, &fixedLen) && fixed &&
        fixedLen >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xFEEF04BD) {
        _snprintf(out, cap, "%u.%u.%u.%u",
                  (unsigned)HIWORD(fixed->dwFileVersionMS),
                  (unsigned)LOWORD(fixed->dwFileVersionMS),
                  (unsigned)HIWORD(fixed->dwFileVersionLS),
                  (unsigned)LOWORD(fixed->dwFileVersionLS));
        out[cap - 1] = 0;
    }
}

const char* buildInfoProjectUrl(void)
{
    return "https://github.com/linsmod/miniBrowserShell";
}

const char* buildInfoShellBuild(void)
{
    return __DATE__ " " __TIME__;
}

const char* buildInfoEngineVersion(void)
{
    wchar_t path[MAX_PATH];

    if (!s_versionDone) {
        s_versionDone = 1;
        strcpy(s_version, "-");
        if (engineModulePath(path, MAX_PATH))
            fileVersion(path, s_version, sizeof(s_version));
        if (!s_version[0])
            strcpy(s_version, "-");
    }

    return s_version;
}

static int sha256File(const wchar_t* path, char* out)
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned char buf[64 * 1024];
    unsigned char digest[32];
    DWORD digestLen = sizeof(digest);
    DWORD read = 0;
    int i;
    int ok = 0;

    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return 0;

    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash))
        goto done;

    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        goto done;

    while (ReadFile(file, buf, sizeof(buf), &read, NULL) && read > 0) {
        if (!CryptHashData(hash, buf, read, 0))
            goto done;
    }

    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLen, 0))
        goto done;

    for (i = 0; i < 32; ++i)
        _snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = 0;
    ok = 1;

done:
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (hash)
        CryptDestroyHash(hash);
    if (prov)
        CryptReleaseContext(prov, 0);
    return ok;
}

const char* buildInfoEngineSha256(void)
{
    wchar_t path[MAX_PATH];

    if (!s_hashDone) {
        s_hashDone = 1;
        strcpy(s_hash, "-");
        if (engineModulePath(path, MAX_PATH) && sha256File(path, s_hash))
            ;
        else
            strcpy(s_hash, "-");
    }

    return s_hash;
}

/* ------------------------------------------------------------------------- */
/* launch summary                                                            */
/* ------------------------------------------------------------------------- */

/* Flags whose value is a number / short token and safe to print. */
static int valueIsSafe(const char* name)
{
    static const char* const safe[] = {
        "--zoom", "--device-scale", "--resize", "--timeout", "--wait", "--settle",
        "--reload", "--reload-count", "--mod", "--ui-dpi", "--ui-theme",
        "--ui-size", "--shot-rect", "--click", "--right-click", "--drag",
        "--shift-click", "--editor", "--nav-after", "--hide-after",
        "--show-after", "--interrupt-after", "--port", "--reload-every"
    };
    size_t i;

    for (i = 0; i < sizeof(safe) / sizeof(safe[0]); ++i) {
        if (!_stricmp(name, safe[i]))
            return 1;
    }

    return 0;
}

/* Flags whose value is a path, URL or content and must never be printed. */
static int valueIsPrivate(const char* name)
{
    static const char* const hidden[] = {
        "--userdata", "--proxy", "--eval", "--eval-file", "--pre-eval",
        "--inject", "--shot", "--keys", "--ime", "--crash-page", "--nav-url",
        "--find", "--before-nav-eval", "--test-url"
    };
    size_t i;

    for (i = 0; i < sizeof(hidden) / sizeof(hidden[0]); ++i) {
        if (!_stricmp(name, hidden[i]))
            return 1;
    }

    return 0;
}

static int looksLikeUrl(const char* s)
{
    return s && (strstr(s, "://") || !_strnicmp(s, "file:", 5) ||
                 !_strnicmp(s, "about:", 6) || !_strnicmp(s, "minishell:", 10));
}

static int isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char* buildLaunchArgs(void)
{
    const char* p = GetCommandLineA();
    size_t len = 0;
    int positional = 0;

    if (s_argsDone)
        return s_args;

    s_argsDone = 1;
    s_args[0] = 0;

    if (!p)
        return s_args;

    /* Skip argv[0]. */
    if (*p == '"') {
        ++p;
        while (*p && *p != '"')
            ++p;
        if (*p)
            ++p;
    } else {
        while (*p && !isSpace(*p))
            ++p;
    }

    while (*p) {
        char token[512];
        char name[128];
        char* eq;
        size_t n = 0;

        while (*p && isSpace(*p))
            ++p;
        if (!*p)
            break;

        if (*p == '"') {
            ++p;
            while (*p && *p != '"' && n + 1 < sizeof(token))
                token[n++] = *p++;
            if (*p == '"')
                ++p;
        } else {
            while (*p && !isSpace(*p) && n + 1 < sizeof(token))
                token[n++] = *p++;
        }
        token[n] = 0;

        if (token[0] == '-') {
            strncpy(name, token, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            eq = strchr(name, '=');

            if (eq) {
                *eq++ = 0;
                if (valueIsSafe(name))
                    append(s_args, sizeof(s_args), &len, "%s %s ", name, eq);
                else
                    append(s_args, sizeof(s_args), &len, "%s=(set) ", name);
                continue;
            }

            append(s_args, sizeof(s_args), &len, "%s ", name);

            /* Consume the value the next token carries, if there is one. */
            if (valueIsPrivate(name) || valueIsSafe(name)) {
                const char* save = p;
                char value[512];
                size_t vn = 0;

                while (*p && isSpace(*p))
                    ++p;
                if (*p == '"') {
                    ++p;
                    while (*p && *p != '"' && vn + 1 < sizeof(value))
                        value[vn++] = *p++;
                    if (*p == '"')
                        ++p;
                } else {
                    while (*p && !isSpace(*p) && vn + 1 < sizeof(value))
                        value[vn++] = *p++;
                }
                value[vn] = 0;

                if (!value[0] || value[0] == '-') {
                    p = save;   /* no value: do not swallow the next flag */
                } else if (valueIsSafe(name) && !looksLikeUrl(value)) {
                    append(s_args, sizeof(s_args), &len, "%s ", value);
                } else {
                    append(s_args, sizeof(s_args), &len, "(set) ");
                }
            }
            continue;
        }

        /* Positional argument (the startup URL) or an unknown value. */
        if (!positional) {
            append(s_args, sizeof(s_args), &len, looksLikeUrl(token) ? "(url omitted) " : "(value omitted) ");
            positional = 1;
        }
    }

    /* Trim the trailing space. */
    while (len > 0 && s_args[len - 1] == ' ')
        s_args[--len] = 0;

    if (!s_args[0])
        strcpy(s_args, "(default)");

    return s_args;
}

const char* buildInfoLaunchArgs(void)
{
    return buildLaunchArgs();
}

static int envValueIsSafe(const char* name, const char* value)
{
    size_t i;
    size_t len;

    if (!value || !value[0])
        return 1;
    if (strstr(value, "://") || strchr(value, '\\') || strchr(value, '/'))
        return 0;

    /* Anything that names a location is private even without separators. */
    if (strstr(name, "PROXY") || strstr(name, "USERDATA") || strstr(name, "PATH") ||
        strstr(name, "DIR") || strstr(name, "FILE") || strstr(name, "URL"))
        return 0;

    len = strlen(value);
    if (len > 48)
        return 0;

    for (i = 0; i < len; ++i) {
        char c = value[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ',' ||
              c == ':' || c == '+' || c == '-'))
            return 0;
    }

    return 1;
}

static const char* buildLaunchEnv(void)
{
    const char* block;
    const char* p;
    size_t len = 0;

    if (s_envDone)
        return s_env;

    s_envDone = 1;
    s_env[0] = 0;

    block = GetEnvironmentStringsA();
    if (!block)
        return s_env;

    for (p = block; *p; p += strlen(p) + 1) {
        const char* eq;
        char name[128];
        const char* value;
        size_t nameLen;

        if (p[0] == '=')
            continue;   /* hidden drive-current-directory entries */

        if (_strnicmp(p, "AGB_", 4) != 0 && _strnicmp(p, "MBV_", 4) != 0)
            continue;

        eq = strchr(p, '=');
        if (!eq)
            continue;

        nameLen = (size_t)(eq - p);
        if (nameLen >= sizeof(name))
            nameLen = sizeof(name) - 1;
        memcpy(name, p, nameLen);
        name[nameLen] = 0;
        value = eq + 1;

        if (envValueIsSafe(name, value))
            append(s_env, sizeof(s_env), &len, "%s=%s ", name, value);
        else
            append(s_env, sizeof(s_env), &len, "%s=(set) ", name);
    }

    FreeEnvironmentStringsA((LPCH)block);

    while (len > 0 && s_env[len - 1] == ' ')
        s_env[--len] = 0;

    if (!s_env[0])
        strcpy(s_env, "(none)");

    return s_env;
}

const char* buildInfoLaunchEnv(void)
{
    return buildLaunchEnv();
}
