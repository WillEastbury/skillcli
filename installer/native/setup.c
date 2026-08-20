#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "marketplace.h"

#define SKILL_ZERO 101
#define SKILL_ONE 102
#define SKILL_TWO 103

typedef struct {
    char *body;
    size_t length;
    DWORD status;
} HttpResponse;

typedef struct {
    wchar_t specifications[64][512];
    size_t count;
} Sources;

void http_response_free(HttpResponse *response) {
    free(response->body);
    response->body = NULL;
    response->length = 0;
    response->status = 0;
}

int http_get(const wchar_t *url, HttpResponse *response) {
    URL_COMPONENTS parts = {0};
    wchar_t host[256] = {0};
    wchar_t path[4096] = {0};
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    int success = 0;

    http_response_free(response);
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = _countof(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(url, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) goto done;
    session = WinHttpOpen(L"skillcli/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto done;
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connection) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) goto done;
    DWORD status_size = sizeof(response->status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &response->status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) goto done;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) goto done;
        if (!available) break;
        char *expanded = realloc(response->body, response->length + available + 1);
        if (!expanded) goto done;
        response->body = expanded;
        DWORD read = 0;
        if (!WinHttpReadData(request, response->body + response->length, available, &read)) goto done;
        response->length += read;
        response->body[response->length] = '\0';
    }
    success = response->status >= 200 && response->status < 300;
done:
    if (!success) http_response_free(response);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return success;
}

/* The catalogue only needs JSON string fields, arrays, objects, and literals. */
static const char *json_skip_whitespace(const char *value) {
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') ++value;
    return value;
}

static const char *json_skip_value(const char *value) {
    value = json_skip_whitespace(value);
    if (*value == '"') {
        ++value;
        while (*value && (*value != '"' || value[-1] == '\\')) ++value;
        return *value ? value + 1 : NULL;
    }
    if (*value == '{' || *value == '[') {
        char open = *value++;
        char close = open == '{' ? '}' : ']';
        int depth = 1;
        while (*value && depth) {
            if (*value == '"') {
                value = json_skip_value(value);
                if (!value) return NULL;
                continue;
            }
            if (*value == open) ++depth;
            if (*value == close) --depth;
            ++value;
        }
        return depth ? NULL : value;
    }
    while (*value && !strchr(",]}", *value)) ++value;
    return value;
}

int json_object_string(const char *json, const char *key, char *output, size_t capacity) {
    const char *value = json_skip_whitespace(json);
    if (*value++ != '{') return 0;
    for (;;) {
        value = json_skip_whitespace(value);
        if (*value == '}') return 0;
        if (*value++ != '"') return 0;
        const char *name = value;
        while (*value && (*value != '"' || value[-1] == '\\')) ++value;
        if (!*value) return 0;
        size_t name_length = (size_t)(value - name);
        ++value;
        value = json_skip_whitespace(value);
        if (*value++ != ':') return 0;
        value = json_skip_whitespace(value);
        if (strlen(key) == name_length && strncmp(name, key, name_length) == 0) {
            if (*value++ != '"') return 0;
            size_t length = 0;
            while (*value && (*value != '"' || value[-1] == '\\')) {
                if (length + 1 >= capacity) return 0;
                output[length++] = *value++;
            }
            if (!*value) return 0;
            output[length] = '\0';
            return 1;
        }
        value = json_skip_value(value);
        if (!value) return 0;
        value = json_skip_whitespace(value);
        if (*value++ == '}') return 0;
        if (value[-1] != ',') return 0;
    }
}

static int append_path(wchar_t *buffer, size_t capacity, const wchar_t *part) {
    size_t length = wcslen(buffer);
    size_t part_length = wcslen(part);
    if (length + part_length + 2 > capacity) return 0;
    if (length && buffer[length - 1] != L'\\') buffer[length++] = L'\\';
    wcscpy_s(buffer + length, capacity - length, part);
    return 1;
}

static int is_elevated(void) {
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation = {0};
    DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                  sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

static int relaunch_elevated(const wchar_t *parameters) {
    wchar_t executable[MAX_PATH * 4] = {0};
    SHELLEXECUTEINFOW launch = {0};
    DWORD exit_code = 1;
    if (!GetModuleFileNameW(NULL, executable, _countof(executable))) return 1;
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS;
    launch.lpVerb = L"runas";
    launch.lpFile = executable;
    launch.lpParameters = parameters;
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) return 1;
    WaitForSingleObject(launch.hProcess, INFINITE);
    GetExitCodeProcess(launch.hProcess, &exit_code);
    CloseHandle(launch.hProcess);
    return (int)exit_code;
}

static int user_path_contains(const wchar_t *path, const wchar_t *directory) {
    wchar_t copy[32767] = {0};
    wcscpy_s(copy, _countof(copy), path);
    for (wchar_t *item = copy; item;) {
        wchar_t *next = wcschr(item, L';');
        if (next) *next++ = L'\0';
        while (*item == L' ') ++item;
        size_t length = wcslen(item);
        while (length && (item[length - 1] == L' ' || item[length - 1] == L'\\')) {
            item[--length] = L'\0';
        }
        if (_wcsicmp(item, directory) == 0) return 1;
        item = next;
    }
    return 0;
}

static int add_to_user_path(const wchar_t *directory) {
    HKEY key = NULL;
    wchar_t current[32767] = {0};
    DWORD type = REG_EXPAND_SZ;
    DWORD size = sizeof(current);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return 0;
    }
    LONG status = RegQueryValueExW(key, L"Path", NULL, &type, (BYTE *)current, &size);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return 0;
    }
    if (!user_path_contains(current, directory)) {
        wchar_t updated[32767] = {0};
        wcscpy_s(updated, _countof(updated), current);
        if (current[0]) wcscat_s(updated, _countof(updated), L";");
        wcscat_s(updated, _countof(updated), directory);
        if (RegSetValueExW(key, L"Path", 0, type, (const BYTE *)updated,
                            (DWORD)((wcslen(updated) + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            RegCloseKey(key);
            return 0;
        }
    }
    RegCloseKey(key);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    return 1;
}

static int remove_from_user_path(const wchar_t *directory, int *removed) {
    HKEY key = NULL;
    wchar_t current[32767] = {0};
    wchar_t updated[32767] = {0};
    DWORD type = REG_EXPAND_SZ;
    DWORD size = sizeof(current);
    LONG status;
    *removed = 0;
    status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0,
                           KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return 1;
    if (status != ERROR_SUCCESS) return 0;
    status = RegQueryValueExW(key, L"Path", NULL, &type, (BYTE *)current, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return 1;
    }
    if (status != ERROR_SUCCESS) {
        RegCloseKey(key);
        return 0;
    }
    for (wchar_t *item = current; item;) {
        wchar_t *next = wcschr(item, L';');
        wchar_t *trimmed = item;
        size_t length;
        if (next) *next++ = L'\0';
        while (*trimmed == L' ') ++trimmed;
        length = wcslen(trimmed);
        while (length && (trimmed[length - 1] == L' ' || trimmed[length - 1] == L'\\')) {
            trimmed[--length] = L'\0';
        }
        if (_wcsicmp(trimmed, directory) == 0) {
            *removed = 1;
        } else if (trimmed[0]) {
            if (updated[0]) wcscat_s(updated, _countof(updated), L";");
            wcscat_s(updated, _countof(updated), trimmed);
        }
        item = next;
    }
    if (*removed &&
        RegSetValueExW(key, L"Path", 0, type, (const BYTE *)updated,
                        (DWORD)((wcslen(updated) + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return 0;
    }
    RegCloseKey(key);
    if (*removed) {
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    }
    return 1;
}

static int user_home(wchar_t *path, size_t capacity) {
    return SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, path) == S_OK &&
           wcslen(path) < capacity;
}

static int user_path_has_skillcli_directory(wchar_t *directory, size_t capacity) {
    HKEY key = NULL;
    wchar_t current[32767] = {0};
    DWORD type = REG_EXPAND_SZ;
    DWORD size = sizeof(current);
    int contains = 0;
    LONG status;
    if (!user_home(directory, capacity) ||
        !append_path(directory, capacity, L"skillcli")) {
        return -1;
    }
    status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return 0;
    if (status != ERROR_SUCCESS) {
        return -1;
    }
    if (RegQueryValueExW(key, L"Path", NULL, &type, (BYTE *)current, &size) == ERROR_SUCCESS) {
        contains = user_path_contains(current, directory);
    }
    RegCloseKey(key);
    return contains;
}

static int confirm_user_path_update(const wchar_t *directory) {
    wchar_t response[16] = {0};
    wprintf(L"skillcli will add this directory to your user PATH:\n  %ls\n", directory);
    wprintf(L"This lets you run `skillcli` from any console. Continue to Windows elevation? [y/N]: ");
    return fgetws(response, _countof(response), stdin) &&
           (response[0] == L'y' || response[0] == L'Y');
}

static int ensure_directory(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
               !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    return CreateDirectoryW(path, NULL) &&
           !(GetFileAttributesW(path) & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int files_equal(const wchar_t *left_path, const wchar_t *right_path) {
    HANDLE left = CreateFileW(left_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE right = CreateFileW(right_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER left_size = {0};
    LARGE_INTEGER right_size = {0};
    unsigned char left_buffer[65536];
    unsigned char right_buffer[65536];
    int equal = 0;
    if (left == INVALID_HANDLE_VALUE || right == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(left, &left_size) || !GetFileSizeEx(right, &right_size) ||
        left_size.QuadPart != right_size.QuadPart) goto done;
    for (;;) {
        DWORD left_read = 0;
        DWORD right_read = 0;
        if (!ReadFile(left, left_buffer, sizeof(left_buffer), &left_read, NULL) ||
            !ReadFile(right, right_buffer, sizeof(right_buffer), &right_read, NULL) ||
            left_read != right_read) goto done;
        if (!left_read) {
            equal = 1;
            goto done;
        }
        if (memcmp(left_buffer, right_buffer, left_read) != 0) goto done;
    }
done:
    if (left != INVALID_HANDLE_VALUE) CloseHandle(left);
    if (right != INVALID_HANDLE_VALUE) CloseHandle(right);
    return equal;
}

/* 1 = fresh install, 2 = updated, 3 = already current, 0 = failed. */
static int install_self(wchar_t *install_directory, size_t capacity) {
    if (!user_home(install_directory, capacity) ||
        !append_path(install_directory, capacity, L"skillcli") ||
        !ensure_directory(install_directory)) return 0;

    wchar_t source[MAX_PATH * 4] = {0};
    wchar_t destination[MAX_PATH * 4] = {0};
    if (!GetModuleFileNameW(NULL, source, _countof(source))) return 0;
    wcscpy_s(destination, _countof(destination), install_directory);
    if (!append_path(destination, _countof(destination), L"skillcli.exe")) return 0;
    DWORD attributes = GetFileAttributesW(destination);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    if (_wcsicmp(source, destination) == 0) {
        return attributes != INVALID_FILE_ATTRIBUTES && add_to_user_path(install_directory) ? 3 : 0;
    }
    if (attributes != INVALID_FILE_ATTRIBUTES && files_equal(source, destination)) {
        return add_to_user_path(install_directory) ? 3 : 0;
    }
    if (!CopyFileW(source, destination, FALSE) || !files_equal(source, destination) ||
        !add_to_user_path(install_directory)) return 0;
    return attributes == INVALID_FILE_ATTRIBUTES ? 1 : 2;
}

static int write_resource(WORD resource_id, const wchar_t *destination) {
    DWORD attributes = GetFileAttributesW(destination);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(10));
    if (!resource) return 0;
    HGLOBAL loaded = LoadResource(NULL, resource);
    DWORD size = SizeofResource(NULL, resource);
    const void *data = LockResource(loaded);
    HANDLE file = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(file, data, size, &written, NULL) && written == size;
    CloseHandle(file);
    return ok;
}

static void deploy_skill(const wchar_t *root, const wchar_t *plugin_name, WORD resource_id) {
    wchar_t target[MAX_PATH * 4] = {0};
    wcscpy_s(target, _countof(target), root);
    if (!append_path(target, _countof(target), L"WillEastbury!skillcli!") ||
        wcslen(target) + wcslen(plugin_name) + 1 >= _countof(target)) return;
    wcscat_s(target, _countof(target), plugin_name);
    if (!ensure_directory(target)) return;
    if (!append_path(target, _countof(target), L"SKILL.md")) return;
    write_resource(resource_id, target);
}

static void deploy_core_skills(const wchar_t *root) {
    DWORD attributes = GetFileAttributesW(root);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    deploy_skill(root, L"skillcli-skill-zero", SKILL_ZERO);
    deploy_skill(root, L"skillcli-skill-one", SKILL_ONE);
    deploy_skill(root, L"skillcli-skill-two", SKILL_TWO);
}

static int command_exists(const wchar_t *command) {
    wchar_t resolved[MAX_PATH] = {0};
    return SearchPathW(NULL, command, L".exe", _countof(resolved), resolved, NULL) != 0;
}

static void install_copilot_via_winget(void) {
    if (command_exists(L"copilot")) return;
    wchar_t winget[MAX_PATH * 4] = {0};
    if (!SearchPathW(NULL, L"winget", L".exe", _countof(winget), winget, NULL)) {
        wprintf(L"  winget is unavailable. Install GitHub Copilot CLI manually.\n");
        return;
    }
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    wchar_t command[] = L"winget.exe install --id GitHub.Copilot --exact --source winget "
                        L"--accept-source-agreements --accept-package-agreements";
    if (CreateProcessW(winget, command, NULL, NULL, FALSE, 0, NULL,
                       L"C:\\Windows\\System32", &startup, &process)) {
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

static void open_scout_download(void) {
    ShellExecuteW(NULL, L"open", L"https://aka.ms/scout-release", NULL, NULL, SW_SHOWNORMAL);
}

typedef struct {
    const wchar_t *name;
    wchar_t folder[MAX_PATH * 4];
} Host;

static int path_has_reparse_point(const wchar_t *path) {
    wchar_t current[MAX_PATH * 4] = {0};
    const wchar_t *component;
    if (!path[0] || wcsncmp(path, L"\\\\?\\", 4) == 0) return 1;
    if (path[1] == L':') {
        if (path[2] != L'\\') return 1;
        swprintf_s(current, _countof(current), L"%lc:\\", path[0]);
        component = path + 3;
    } else {
        return 1;
    }
    while (*component) {
        const wchar_t *next = wcschr(component, L'\\');
        size_t length = next ? (size_t)(next - component) : wcslen(component);
        DWORD attributes;
        if (!length || length >= 256 || wcslen(current) + length + 2 >= _countof(current)) return 1;
        if (wcslen(current) && current[wcslen(current) - 1] != L'\\') wcscat_s(current, _countof(current), L"\\");
        wcsncat_s(current, _countof(current), component, length);
        attributes = GetFileAttributesW(current);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 1;
        if (next && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return 1;
        if (!next) break;
        component = next + 1;
    }
    return 0;
}

static int safe_existing_directory(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) && !path_has_reparse_point(path);
}

static int read_managed_text_file(const wchar_t *path, char *contents, size_t capacity) {
    HANDLE file;
    LARGE_INTEGER size = {0};
    DWORD read = 0;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || capacity < 2) return 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (ULONGLONG)size.QuadPart >= capacity) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return 0;
    }
    if (!ReadFile(file, contents, (DWORD)size.QuadPart, &read, NULL) ||
        read != (DWORD)size.QuadPart) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    contents[read] = '\0';
    return 1;
}

static int managed_text_contains(const char *contents, const char *first, const char *second) {
    return strstr(contents, first) != NULL && strstr(contents, second) != NULL;
}

static int child_path(const wchar_t *root, const wchar_t *name, wchar_t *path, size_t capacity) {
    if (wcslen(root) + wcslen(name) + 2 > capacity) return 0;
    wcscpy_s(path, capacity, root);
    return append_path(path, capacity, name);
}

static int delete_managed_file(const wchar_t *root, const wchar_t *name,
                               const char *first_marker, const char *second_marker) {
    wchar_t path[MAX_PATH * 4] = {0};
    char contents[65536] = {0};
    if (!child_path(root, name, path, _countof(path)) ||
        !read_managed_text_file(path, contents, _countof(contents)) ||
        !managed_text_contains(contents, first_marker, second_marker)) return 0;
    return DeleteFileW(path) != 0;
}

static unsigned int clean_legacy_tool_files(const wchar_t *root) {
    wchar_t cli_path[MAX_PATH * 4] = {0};
    wchar_t core_path[MAX_PATH * 4] = {0};
    char cli[65536] = {0};
    char core[65536] = {0};
    unsigned int removed = 0;
    int python_pair = 0;
    if (!safe_existing_directory(root)) {
        if (GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES) {
            wprintf(L"  Skipped legacy cleanup at %ls: reparse point or unsafe directory.\n", root);
        }
        return 0;
    }
    python_pair = child_path(root, L"skillcli.py", cli_path, _countof(cli_path)) &&
                  child_path(root, L"skillcli_core.py", core_path, _countof(core_path)) &&
                  read_managed_text_file(cli_path, cli, _countof(cli)) &&
                  read_managed_text_file(core_path, core, _countof(core)) &&
                  managed_text_contains(cli, "from skillcli_core import",
                                        "Command-line interface for governed plugin marketplaces") &&
                  managed_text_contains(core, "class Catalogues", "def install_or_update");
    if (python_pair) {
        if (DeleteFileW(cli_path)) ++removed;
        if (DeleteFileW(core_path)) ++removed;
        removed += delete_managed_file(root, L"skillcli.cmd", "python", "skillcli.py");
    }
    removed += delete_managed_file(root, L"install.ps1", "WillEastbury/skillcli", "skillcli.py");
    removed += delete_managed_file(root, L"install-skill-zero.ps1", "SKILLCLI_TOOL_DIRECTORY", "install.ps1");
    return removed;
}

static int legacy_skill_directory(const wchar_t *directory, const wchar_t *skill_id) {
    wchar_t skill[MAX_PATH * 4] = {0};
    wchar_t receipt[MAX_PATH * 4] = {0};
    WIN32_FIND_DATAW entry;
    HANDLE find;
    int have_skill = 0;
    int have_receipt = 0;
    char marker[128] = {0};
    char id[64] = {0};
    if (!safe_existing_directory(directory) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, skill_id, -1, id, _countof(id),
                             NULL, NULL) ||
        sprintf_s(marker, _countof(marker), "skill-id: \"%s\"", id) < 0 ||
        !child_path(directory, L"SKILL.md", skill, _countof(skill))) return 0;
    {
        char contents[65536] = {0};
        if (!read_managed_text_file(skill, contents, _countof(contents)) ||
            !managed_text_contains(contents, marker, "skillcli")) return 0;
        have_skill = 1;
    }
    if (!child_path(directory, L".skillcli.json", receipt, _countof(receipt))) return 0;
    {
        char contents[65536] = {0};
        DWORD attributes = GetFileAttributesW(receipt);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (!read_managed_text_file(receipt, contents, _countof(contents)) ||
                !managed_text_contains(contents, "WillEastbury/skillcli", "qualifiedId")) return 0;
            have_receipt = 1;
        }
    }
    {
        wchar_t pattern[MAX_PATH * 4] = {0};
        if (wcslen(directory) + 3 >= _countof(pattern)) return 0;
        wcscpy_s(pattern, _countof(pattern), directory);
        wcscat_s(pattern, _countof(pattern), L"\\*");
        find = FindFirstFileW(pattern, &entry);
        if (find == INVALID_HANDLE_VALUE) return 0;
        do {
            if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                (_wcsicmp(entry.cFileName, L"SKILL.md") &&
                 (!have_receipt || _wcsicmp(entry.cFileName, L".skillcli.json")))) {
                FindClose(find);
                return 0;
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
        if (GetLastError() != ERROR_NO_MORE_FILES) return 0;
    }
    return have_skill && have_receipt;
}

static int remove_legacy_skill_directory(const wchar_t *root, const wchar_t *folder,
                                         const wchar_t *skill_id) {
    wchar_t directory[MAX_PATH * 4] = {0};
    wchar_t skill[MAX_PATH * 4] = {0};
    wchar_t receipt[MAX_PATH * 4] = {0};
    if (!safe_existing_directory(root) || !child_path(root, folder, directory, _countof(directory)) ||
        !legacy_skill_directory(directory, skill_id) ||
        !child_path(directory, L"SKILL.md", skill, _countof(skill)) ||
        !DeleteFileW(skill)) return 0;
    if (!child_path(directory, L".skillcli.json", receipt, _countof(receipt))) return 0;
    if (GetFileAttributesW(receipt) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(receipt)) return 0;
    return RemoveDirectoryW(directory) != 0;
}

static int remove_legacy_core_receipt(const wchar_t *root, const wchar_t *folder,
                                      const wchar_t *skill_id, const wchar_t *plugin_name) {
    wchar_t directory[MAX_PATH * 4] = {0};
    wchar_t skill[MAX_PATH * 4] = {0};
    wchar_t receipt[MAX_PATH * 4] = {0};
    char skill_contents[65536] = {0};
    char receipt_contents[65536] = {0};
    char skill_marker[128] = {0};
    char qualified_marker[256] = {0};
    char id[64] = {0};
    char plugin[128] = {0};
    if (!safe_existing_directory(root) || !child_path(root, folder, directory, _countof(directory)) ||
        !safe_existing_directory(directory) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, skill_id, -1, id, _countof(id),
                             NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, plugin_name, -1, plugin,
                             _countof(plugin), NULL, NULL) ||
        sprintf_s(skill_marker, _countof(skill_marker), "skill-id: \"%s\"", id) < 0 ||
        sprintf_s(qualified_marker, _countof(qualified_marker),
                  "WillEastbury/skillcli/%s", plugin) < 0 ||
        !child_path(directory, L"SKILL.md", skill, _countof(skill)) ||
        !child_path(directory, L".skillcli.json", receipt, _countof(receipt)) ||
        !read_managed_text_file(skill, skill_contents, _countof(skill_contents)) ||
        !managed_text_contains(skill_contents, skill_marker, "skillcli") ||
        !read_managed_text_file(receipt, receipt_contents, _countof(receipt)) ||
        !managed_text_contains(receipt_contents, qualified_marker, "WillEastbury/skillcli") ||
        strstr(receipt_contents, "\"path\"") != NULL) return 0;
    return DeleteFileW(receipt) != 0;
}

static unsigned int clean_legacy_skill_directories(const Host *hosts) {
    static const struct {
        const wchar_t *folder;
        const wchar_t *skill_id;
    } legacy[] = {
        {L"WillEastbury!skillcli!skill-zero", L"skill-zero"},
        {L"WillEastbury!skillcli!skill-one", L"skill-one"},
        {L"WillEastbury!skillcli!skill-two", L"skill-two"}
    };
    unsigned int removed = 0;
    for (size_t host = 0; host < 3; ++host) {
        for (size_t item = 0; item < _countof(legacy); ++item) {
            if (remove_legacy_skill_directory(hosts[host].folder, legacy[item].folder,
                                               legacy[item].skill_id)) ++removed;
        }
        if (remove_legacy_core_receipt(hosts[host].folder,
                                       L"WillEastbury!skillcli!skillcli-skill-zero",
                                       L"skill-zero", L"skillcli-skill-zero")) ++removed;
        if (remove_legacy_core_receipt(hosts[host].folder,
                                       L"WillEastbury!skillcli!skillcli-skill-one",
                                       L"skill-one", L"skillcli-skill-one")) ++removed;
        if (remove_legacy_core_receipt(hosts[host].folder,
                                       L"WillEastbury!skillcli!skillcli-skill-two",
                                       L"skill-two", L"skillcli-skill-two")) ++removed;
    }
    return removed;
}

static void clean_legacy_installation(const wchar_t *install_directory, const Host *hosts) {
    wchar_t local_app_data[MAX_PATH * 4] = {0};
    unsigned int removed = clean_legacy_tool_files(install_directory);
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, _countof(local_app_data));
    if (length && length < _countof(local_app_data) &&
        append_path(local_app_data, _countof(local_app_data), L"skillcli") &&
        _wcsicmp(local_app_data, install_directory) != 0) {
        removed += clean_legacy_tool_files(local_app_data);
    }
    removed += clean_legacy_skill_directories(hosts);
    if (removed) wprintf(L"  Removed %u verified legacy skillcli artifact(s).\n", removed);
}

static int folder_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static void print_heading(const wchar_t *text) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    wprintf(L"\n== %ls ==\n", text);
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

static void print_splash(void) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    wprintf(L"\n");
    wprintf(L"  ____  _    _ _ _  ____ _     ___ \n");
    wprintf(L" / ___|| | _(_) | |/ ___| |   |_ _|\n");
    wprintf(L" \\___ \\| |/ / | | | |   | |    | | \n");
    wprintf(L"  ___) |   <| | | | |___| |___ | | \n");
    wprintf(L" |____/|_|\\_\\_|_|_|\\____|_____|___|\n");
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    wprintf(L"  Governed agent skills for your installed harnesses\n");
}

static void detect_hosts(Host *hosts) {
    wchar_t home[MAX_PATH * 4] = {0};
    user_home(home, _countof(home));
    hosts[0].name = L"GitHub Copilot CLI";
    wcscpy_s(hosts[0].folder, _countof(hosts[0].folder), home);
    append_path(hosts[0].folder, _countof(hosts[0].folder), L".copilot\\skills");
    hosts[1].name = L"Microsoft Scout";
    wcscpy_s(hosts[1].folder, _countof(hosts[1].folder), home);
    append_path(hosts[1].folder, _countof(hosts[1].folder), L".scout\\m-skills");
    hosts[2].name = L"Copilot Co-Work";
    DWORD length = GetEnvironmentVariableW(L"OneDrive", hosts[2].folder, _countof(hosts[2].folder));
    if (length && length < _countof(hosts[2].folder)) {
        append_path(hosts[2].folder, _countof(hosts[2].folder), L"Documents\\Cowork\\Skills");
    } else {
        hosts[2].folder[0] = L'\0';
    }
}

static int resource_matches_file(WORD resource_id, const wchar_t *path) {
    HRSRC resource;
    HGLOBAL loaded;
    const unsigned char *expected;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD size;
    DWORD read = 0;
    DWORD offset = 0;
    unsigned char buffer[4096];
    LARGE_INTEGER file_size = {0};
    DWORD attributes = GetFileAttributesW(path);
    int matches = 0;
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    resource = FindResourceW(NULL, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(10));
    if (!resource || !(loaded = LoadResource(NULL, resource)) ||
        !(expected = LockResource(loaded)) || !(size = SizeofResource(NULL, resource))) return 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &file_size) ||
        file_size.QuadPart != size) goto done;
    while (offset < size) {
        DWORD request = size - offset > sizeof(buffer) ? sizeof(buffer) : size - offset;
        if (!ReadFile(file, buffer, request, &read, NULL) || read != request ||
            memcmp(buffer, expected + offset, read) != 0) goto done;
        offset += read;
    }
    matches = 1;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return matches;
}

static int managed_core_directory(const wchar_t *directory, WORD resource_id) {
    WIN32_FIND_DATAW entry;
    wchar_t pattern[MAX_PATH * 4] = {0};
    wchar_t skill[MAX_PATH * 4] = {0};
    HANDLE find;
    int have_skill = 0;
    if (!safe_existing_directory(directory) ||
        !child_path(directory, L"SKILL.md", skill, _countof(skill)) ||
        wcslen(directory) + 3 >= _countof(pattern)) return 0;
    wcscpy_s(pattern, _countof(pattern), directory);
    wcscat_s(pattern, _countof(pattern), L"\\*");
    find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            _wcsicmp(entry.cFileName, L"SKILL.md")) {
            FindClose(find);
            return 0;
        }
        have_skill = 1;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return GetLastError() == ERROR_NO_MORE_FILES && have_skill &&
           resource_matches_file(resource_id, skill);
}

static int remove_managed_core_directory(const wchar_t *root, const wchar_t *folder,
                                         WORD resource_id) {
    wchar_t directory[MAX_PATH * 4] = {0};
    wchar_t skill[MAX_PATH * 4] = {0};
    if (!safe_existing_directory(root) ||
        !child_path(root, folder, directory, _countof(directory)) ||
        !managed_core_directory(directory, resource_id) ||
        !child_path(directory, L"SKILL.md", skill, _countof(skill))) return 0;
    return DeleteFileW(skill) && RemoveDirectoryW(directory);
}

static int uninstall_skillcli(int clean) {
    wchar_t install_directory[MAX_PATH * 4] = {0};
    wchar_t executable[MAX_PATH * 4] = {0};
    wchar_t sources[MAX_PATH * 4] = {0};
    Host hosts[3] = {0};
    int path_removed = 0;
    int success = 1;
    unsigned int cores_removed = 0;
    if (!user_home(install_directory, _countof(install_directory)) ||
        !append_path(install_directory, _countof(install_directory), L"skillcli")) {
        fwprintf(stderr, L"skillcli could not locate its installed directory.\n");
        return 1;
    }
    wprintf(L"skillcli %ls\n", clean ? L"clean uninstall" : L"uninstall");
    if (!remove_from_user_path(install_directory, &path_removed)) {
        fwprintf(stderr, L"  Could not update the user PATH.\n");
        success = 0;
    } else {
        wprintf(L"  User PATH: %ls\n", path_removed ? L"removed skillcli entry" : L"already clear");
    }
    if (!safe_existing_directory(install_directory)) {
        if (GetFileAttributesW(install_directory) != INVALID_FILE_ATTRIBUTES) {
            fwprintf(stderr, L"  Refused installed-file cleanup: unsafe directory or reparse point.\n");
            success = 0;
        } else {
            wprintf(L"  Installed executable: not found.\n");
        }
    } else if (!child_path(install_directory, L"skillcli.exe", executable, _countof(executable))) {
        success = 0;
    } else {
        wchar_t running[MAX_PATH * 4] = {0};
        DWORD attributes = GetFileAttributesW(executable);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"  Installed executable: not found.\n");
        } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            fwprintf(stderr, L"  Refused installed EXE cleanup: file is unsafe or a reparse point.\n");
            success = 0;
        } else if (!GetModuleFileNameW(NULL, running, _countof(running)) ||
                   _wcsicmp(running, executable) != 0) {
            fwprintf(stderr, L"  Refused installed EXE cleanup: run this command from the installed skillcli.exe.\n");
            success = 0;
        } else if (MoveFileExW(executable, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            wprintf(L"  Installed executable deletion is scheduled for reboot:\n    %ls\n", executable);
            wprintf(L"  Restart Windows to complete executable removal.\n");
        } else {
            fwprintf(stderr, L"  Could not schedule installed EXE deletion.\n");
            success = 0;
        }
    }
    if (!clean) return success ? 0 : 1;
    if (!safe_existing_directory(install_directory)) return success ? 0 : 1;
    if (!child_path(install_directory, L"sources.json", sources, _countof(sources))) return 1;
    {
        DWORD attributes = GetFileAttributesW(sources);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"  Source configuration: not found.\n");
        } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            fwprintf(stderr, L"  Refused sources.json cleanup: file is unsafe or a reparse point.\n");
            success = 0;
        } else if (DeleteFileW(sources)) {
            wprintf(L"  Removed sources.json.\n");
        } else {
            fwprintf(stderr, L"  Could not remove sources.json.\n");
            success = 0;
        }
    }
    detect_hosts(hosts);
    {
        static const struct {
            const wchar_t *folder;
            WORD resource_id;
        } cores[] = {
            {L"WillEastbury!skillcli!skillcli-skill-zero", SKILL_ZERO},
            {L"WillEastbury!skillcli!skillcli-skill-one", SKILL_ONE},
            {L"WillEastbury!skillcli!skillcli-skill-two", SKILL_TWO}
        };
        for (size_t host = 0; host < _countof(hosts); ++host) {
            for (size_t core = 0; core < _countof(cores); ++core) {
                if (remove_managed_core_directory(hosts[host].folder, cores[core].folder,
                                                  cores[core].resource_id)) ++cores_removed;
            }
        }
    }
    wprintf(L"  Removed %u verified managed core-skill director%ls.\n", cores_removed,
            cores_removed == 1 ? L"y" : L"ies");
    return success ? 0 : 1;
}

static int host_present(const Host *hosts, int index) {
    if (index == 0) return command_exists(L"copilot") || folder_exists(hosts[index].folder);
    return hosts[index].folder[0] && folder_exists(hosts[index].folder);
}

static void clear_console(HANDLE output) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    COORD home = {0, 0};
    DWORD written = 0;
    if (!GetConsoleScreenBufferInfo(output, &info)) return;
    FillConsoleOutputCharacterW(output, L' ', info.dwSize.X * info.dwSize.Y, home, &written);
    FillConsoleOutputAttribute(output, info.wAttributes, info.dwSize.X * info.dwSize.Y, home, &written);
    SetConsoleCursorPosition(output, home);
}

static void render_host_selector(const Host *hosts, const int *present,
                                 const int *selected, int cursor) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    clear_console(output);
    wprintf(L"skillcli host setup\n\n");
    wprintf(L"Detected harnesses are pre-selected and will receive core-skill updates.\n");
    wprintf(L"Select missing harnesses to start their acquisition flow.\n");
    wprintf(L"Use Up/Down to move, Space to select a missing harness, Enter to continue.\n\n");
    for (int index = 0; index < 3; ++index) {
        const wchar_t *state = present[index] ? L"detected" : L"missing";
        wprintf(L"%lc [%lc] %ls (%ls)\n",
                index == cursor ? L'>' : L' ',
                selected[index] ? L'x' : L' ', hosts[index].name, state);
        wprintf(L"      %ls\n", hosts[index].folder[0] ? hosts[index].folder :
                L"OneDrive is not configured");
    }
}

static void select_hosts_tui(const Host *hosts, int *selected) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    int present[3] = {0};
    int cursor = 0;
    for (int index = 0; index < 3; ++index) {
        present[index] = host_present(hosts, index);
        selected[index] = present[index];
    }
    if (input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &original_mode)) return;
    SetConsoleMode(input, (original_mode | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT) &
                          ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
    for (;;) {
        INPUT_RECORD record;
        DWORD read = 0;
        render_host_selector(hosts, present, selected, cursor);
        if (!ReadConsoleInputW(input, &record, 1, &read)) break;
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
        switch (record.Event.KeyEvent.wVirtualKeyCode) {
        case VK_UP:
            cursor = cursor ? cursor - 1 : 2;
            break;
        case VK_DOWN:
            cursor = cursor == 2 ? 0 : cursor + 1;
            break;
        case VK_SPACE:
            if (!present[cursor]) selected[cursor] = !selected[cursor];
            break;
        case VK_ESCAPE:
            for (int index = 0; index < 3; ++index) selected[index] = present[index];
            SetConsoleMode(input, original_mode);
            return;
        case VK_RETURN:
            SetConsoleMode(input, original_mode);
            return;
        }
    }
    SetConsoleMode(input, original_mode);
}

static void acquire_selected_hosts(const Host *hosts, const int *selected) {
    print_heading(L"Host acquisition");
    if (selected[0] && !host_present(hosts, 0)) {
        wprintf(L"  Installing GitHub Copilot CLI with winget.\n");
        install_copilot_via_winget();
    }
    if (selected[1] && !host_present(hosts, 1)) {
        wprintf(L"  Opening the official Microsoft Scout download page.\n");
        open_scout_download();
    }
    if (selected[2] && !host_present(hosts, 2)) {
        wprintf(L"  Copilot Co-Work has no unattended installer. Configure it, then rerun skillcli.\n");
    }
}

static int ensure_copilot_skills_folder(const Host *host) {
    wchar_t parent[MAX_PATH * 4] = {0};
    wchar_t *separator;
    if (folder_exists(host->folder)) return 1;
    wcscpy_s(parent, _countof(parent), host->folder);
    separator = wcsrchr(parent, L'\\');
    if (!separator) return 0;
    *separator = L'\0';
    return ensure_directory(parent) && ensure_directory(host->folder);
}

static void deploy_to_detected_hosts(const Host *hosts) {
    print_heading(L"Core skill deployment");
    for (int index = 0; index < 3; ++index) {
        if (!host_present(hosts, index)) continue;
        if (index == 0 && !ensure_copilot_skills_folder(&hosts[index])) {
            wprintf(L"  Could not create the GitHub Copilot CLI skills folder.\n");
            continue;
        }
        if (!folder_exists(hosts[index].folder)) {
            wprintf(L"  Skipped %ls: skills folder is unavailable.\n", hosts[index].name);
            continue;
        }
        deploy_core_skills(hosts[index].folder);
        wprintf(L"  Installed or updated core skills in %ls.\n", hosts[index].name);
    }
}

static int valid_source_component(const wchar_t *start, size_t length) {
    if (!length || (length == 1 && start[0] == L'.') ||
        (length == 2 && start[0] == L'.' && start[1] == L'.')) return 0;
    for (size_t index = 0; index < length; ++index) {
        if (!(iswalnum(start[index]) || start[index] == L'.' ||
              start[index] == L'_' || start[index] == L'-')) return 0;
    }
    return 1;
}

static int source_parts(const wchar_t *value, wchar_t *repository, size_t repository_capacity,
                        wchar_t *subpath, size_t subpath_capacity) {
    const wchar_t *first = wcschr(value, L'/');
    if (!first || first == value || !first[1] || wcschr(value, L' ') || wcschr(value, L'\\')) return 0;
    const wchar_t *second = wcschr(first + 1, L'/');
    size_t repository_length = second ? (size_t)(second - value) : wcslen(value);
    size_t owner_length = (size_t)(first - value);
    size_t name_length = second ? (size_t)(second - first - 1) : wcslen(first + 1);
    if (repository_length >= repository_capacity ||
        !valid_source_component(value, owner_length) ||
        !valid_source_component(first + 1, name_length)) return 0;
    wcsncpy_s(repository, repository_capacity, value, repository_length);
    repository[repository_length] = L'\0';
    subpath[0] = L'\0';
    if (!second) return 1;
    if (!second[1]) return 0;
    for (const wchar_t *component = second + 1; component;) {
        const wchar_t *next = wcschr(component, L'/');
        size_t length = next ? (size_t)(next - component) : wcslen(component);
        if (!valid_source_component(component, length)) return 0;
        component = next ? next + 1 : NULL;
    }
    wcscpy_s(subpath, subpath_capacity, second + 1);
    return 1;
}

static int valid_source_specification(const wchar_t *value) {
    wchar_t repository[256] = {0};
    wchar_t subpath[256] = {0};
    return source_parts(value, repository, _countof(repository), subpath, _countof(subpath));
}

static int sources_contains(const Sources *sources, const wchar_t *specification) {
    for (size_t index = 0; index < sources->count; ++index) {
        if (_wcsicmp(sources->specifications[index], specification) == 0) return 1;
    }
    return 0;
}

static void sources_add(Sources *sources, const wchar_t *specification) {
    if (sources->count >= _countof(sources->specifications) ||
        sources_contains(sources, specification)) return;
    wcscpy_s(sources->specifications[sources->count++],
             _countof(sources->specifications[0]), specification);
}

static void source_id(const wchar_t *repository, wchar_t *id, size_t capacity) {
    size_t output = 0;
    for (const wchar_t *current = repository; *current && output + 1 < capacity; ++current) {
        id[output++] = *current == L'/' ? L'-' : towlower(*current);
    }
    id[output] = L'\0';
}

static void load_sources(const wchar_t *path, Sources *sources) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size = {0};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart > 1048576) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return;
    }
    char *contents = calloc((size_t)size.QuadPart + 1, 1);
    DWORD read = 0;
    if (!contents || !ReadFile(file, contents, (DWORD)size.QuadPart, &read, NULL)) {
        free(contents);
        CloseHandle(file);
        return;
    }
    CloseHandle(file);
    for (char *current = contents; (current = strstr(current, "\"repository\""));) {
        current = strchr(current, ':');
        if (!current) break;
        while (*++current == ' ' || *current == '\t') {}
        if (*current++ != '"') continue;
        char *end = strchr(current, '"');
        if (!end) break;
        *end = '\0';
        char *object_end = strchr(end + 1, '}');
        char *path_key = object_end ? strstr(end + 1, "\"path\"") : NULL;
        wchar_t specification[512] = {0};
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, current, -1,
                                specification, _countof(specification))) {
            if (path_key && path_key < object_end) {
                path_key = strchr(path_key, ':');
                if (path_key) {
                    while (*++path_key == ' ' || *path_key == '\t') {}
                    char *path_end = *path_key++ == '"' ? strchr(path_key, '"') : NULL;
                    wchar_t subpath[256] = {0};
                    if (path_end) {
                        *path_end = '\0';
                        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_key, -1,
                                                subpath, _countof(subpath))) {
                            wcscat_s(specification, _countof(specification), L"/");
                            wcscat_s(specification, _countof(specification), subpath);
                        }
                    }
                }
            }
            if (valid_source_specification(specification)) sources_add(sources, specification);
        }
        current = end + 1;
    }
    free(contents);
}

static int write_sources(const wchar_t *path, const Sources *sources) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    FILE *file = NULL;
    if (_wfopen_s(&file, path, L"w, ccs=UTF-8") != 0 || !file) return 0;
    fputws(L"{\n  \"sources\": [\n", file);
    for (size_t index = 0; index < sources->count; ++index) {
        wchar_t id[256] = {0};
        wchar_t repository[256] = {0};
        wchar_t subpath[256] = {0};
        if (!source_parts(sources->specifications[index], repository, _countof(repository),
                          subpath, _countof(subpath))) continue;
        source_id(sources->specifications[index], id, _countof(id));
        fwprintf(file,
                 L"    {\n"
                 L"      \"id\": \"%ls\",\n"
                 L"      \"repository\": \"%ls\",\n",
                 id, repository);
        if (subpath[0]) fwprintf(file, L"      \"path\": \"%ls\",\n", subpath);
        fwprintf(file,
                 L"      \"ref\": \"main\",\n"
                 L"      \"private\": false\n"
                 L"    }%ls\n",
                 index + 1 == sources->count ? L"" : L",");
    }
    fputws(L"  ]\n}\n", file);
    fclose(file);
    return 1;
}

static void register_sources(const wchar_t *install_directory) {
    print_heading(L"4. Register additional repositories");
    wchar_t source_file[MAX_PATH * 4] = {0};
    wcscpy_s(source_file, _countof(source_file), install_directory);
    append_path(source_file, _countof(source_file), L"sources.json");
    Sources sources = {0};
    load_sources(source_file, &sources);
    sources_add(&sources, L"WillEastbury/skillcli");
    wprintf(L"  Enter OWNER/REPO or OWNER/REPO/sub/path values, one per line. Submit a blank line when done.\n");
    for (;;) {
        wchar_t repository[256] = {0};
        wprintf(L"  Repository: ");
        if (!fgetws(repository, _countof(repository), stdin)) break;
        repository[wcscspn(repository, L"\r\n")] = L'\0';
        if (!repository[0]) break;
        if (!valid_source_specification(repository)) {
            wprintf(L"  Invalid source. Use OWNER/REPO or OWNER/REPO/sub/path.\n");
            continue;
        }
        if (sources_contains(&sources, repository)) {
            wprintf(L"  %ls is already registered.\n", repository);
            continue;
        }
        sources_add(&sources, repository);
        wprintf(L"  Added %ls.\n", repository);
    }
    if (write_sources(source_file, &sources)) {
        wprintf(L"  Source configuration: %ls\n", source_file);
    } else {
        wprintf(L"  Could not write %ls\n", source_file);
    }
}

static void test_deployment(const Host *hosts, const wchar_t *install_directory) {
    wchar_t executable[MAX_PATH * 4] = {0};
    print_heading(L"Installation status");
    if (wcslen(install_directory) + wcslen(L"\\skillcli.exe") < _countof(executable)) {
        wcscpy_s(executable, _countof(executable), install_directory);
        append_path(executable, _countof(executable), L"skillcli.exe");
        wprintf(L"  Installed executable: %ls\n", executable);
    }
    wprintf(L"  Core skill deployment:\n");
    int deployed = 0;
    for (int index = 0; index < 3; ++index) {
        if (!folder_exists(hosts[index].folder)) {
            wprintf(L"  [not available] %ls\n", hosts[index].name);
            continue;
        }
        wchar_t skill[MAX_PATH * 4] = {0};
        wcscpy_s(skill, _countof(skill), hosts[index].folder);
        append_path(skill, _countof(skill), L"WillEastbury!skillcli!skillcli-skill-zero\\SKILL.md");
        if (GetFileAttributesW(skill) != INVALID_FILE_ATTRIBUTES) {
            wprintf(L"  [updated] %ls\n", hosts[index].name);
            ++deployed;
        } else {
            wprintf(L"  [not deployed] %ls\n", hosts[index].name);
        }
    }
    wprintf(L"\n  %d harness(es) have the core skills installed or updated.\n", deployed);
}

static void show_usage(void) {
    print_splash();
    print_heading(L"skillcli is installed and up to date");
    wprintf(L"Usage:\n");
    wprintf(L"  skillcli search --role <role> --query \"<need>\"\n");
    wprintf(L"  skillcli install --skill <owner>/<repo>/<plugin-name>\n");
    wprintf(L"  skillcli remove --skill <owner>/<repo>/<plugin-name>\n");
    wprintf(L"  skillcli update --skill <owner>/<repo>/<plugin-name>\n");
    wprintf(L"  skillcli update --all\n");
    wprintf(L"  skillcli register <owner>/<repo>[/sub/path]\n");
    wprintf(L"  skillcli self-update\n");
    wprintf(L"  skillcli --uninstall\n");
    wprintf(L"  skillcli --clean\n");
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--uninstall")) {
        return is_elevated() ? uninstall_skillcli(0) : relaunch_elevated(L"--uninstall");
    }
    if (argc == 2 && !wcscmp(argv[1], L"--clean")) {
        return is_elevated() ? uninstall_skillcli(1) : relaunch_elevated(L"--clean");
    }
    if (argc > 1) return marketplace_main(argc, argv);
    if (!is_elevated()) {
        wchar_t install_directory[MAX_PATH * 4] = {0};
        int path_status = user_path_has_skillcli_directory(install_directory, _countof(install_directory));
        if (path_status < 0) {
            fwprintf(stderr, L"skillcli could not inspect the user PATH.\n");
            return 1;
        }
        if (!path_status && !confirm_user_path_update(install_directory)) {
            wprintf(L"skillcli installation cancelled; the user PATH was not changed.\n");
            return 0;
        }
        return relaunch_elevated(L"");
    }

    wchar_t install_directory[MAX_PATH * 4] = {0};
    int installation = install_self(install_directory, _countof(install_directory));
    if (!installation) {
        fwprintf(stderr, L"skillcli could not install itself into the user profile.\n");
        return 1;
    }
    Host hosts[3] = {0};
    int selected_hosts[3] = {0};
    detect_hosts(hosts);
    clean_legacy_installation(install_directory, hosts);
    if (installation == 3) {
        show_usage();
        return 0;
    }
    print_splash();
    wprintf(L"\n  %ls\n", installation == 1 ? L"Installed skillcli." : L"Updated skillcli.");
    select_hosts_tui(hosts, selected_hosts);
    acquire_selected_hosts(hosts, selected_hosts);
    detect_hosts(hosts);
    deploy_to_detected_hosts(hosts);
    register_sources(install_directory);
    test_deployment(hosts, install_directory);
    return 0;
}
