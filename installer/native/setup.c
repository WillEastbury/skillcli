#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

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

static int user_home(wchar_t *path, size_t capacity) {
    return SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, path) == S_OK &&
           wcslen(path) < capacity;
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
    if (_wcsicmp(source, destination) == 0) return add_to_user_path(install_directory) ? 3 : 0;
    DWORD attributes = GetFileAttributesW(destination);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    if (attributes != INVALID_FILE_ATTRIBUTES && files_equal(source, destination)) {
        return add_to_user_path(install_directory) ? 3 : 0;
    }
    if (!CopyFileW(source, destination, FALSE) || !add_to_user_path(install_directory)) return 0;
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

static int prompt_yes_no(const wchar_t *question) {
    wchar_t response[16] = {0};
    wprintf(L"%ls [y/N]: ", question);
    if (!fgetws(response, _countof(response), stdin)) return 0;
    return response[0] == L'y' || response[0] == L'Y';
}

static void offer_copilot_installation(void) {
    if (command_exists(L"copilot")) return;
    if (!prompt_yes_no(L"GitHub Copilot CLI is not installed. Install it with winget?")) return;
    wchar_t winget[MAX_PATH * 4] = {0};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", winget, _countof(winget));
    if (!length || length >= _countof(winget) ||
        !append_path(winget, _countof(winget), L"Microsoft\\WindowsApps\\winget.exe") ||
        GetFileAttributesW(winget) == INVALID_FILE_ATTRIBUTES) {
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

static void offer_scout_download(void) {
    if (prompt_yes_no(L"Microsoft Scout was not detected. Open its download page?")) {
        ShellExecuteW(NULL, L"open", L"https://aka.ms/scout-release", NULL, NULL, SW_SHOWNORMAL);
    }
}

typedef struct {
    const wchar_t *name;
    wchar_t folder[MAX_PATH * 4];
} Host;

static int folder_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
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

static void show_hosts(const Host *hosts) {
    print_heading(L"1. Detect agent harnesses");
    for (int index = 0; index < 3; ++index) {
        wprintf(L"  [%ls] %ls\n      %ls\n", folder_exists(hosts[index].folder) ? L"found" : L"missing",
                hosts[index].name, hosts[index].folder[0] ? hosts[index].folder : L"OneDrive is not configured");
    }
}

static void offer_missing_hosts(const Host *hosts) {
    print_heading(L"2. Offer missing harnesses");
    if (!folder_exists(hosts[0].folder)) offer_copilot_installation();
    if (!folder_exists(hosts[1].folder)) offer_scout_download();
    if (!hosts[2].folder[0] || !folder_exists(hosts[2].folder)) {
        wprintf(L"  Copilot Co-Work is not configured; no unattended installer is available.\n");
    }
}

static void deploy_to_hosts(const Host *hosts) {
    print_heading(L"3. Install core skills");
    for (int index = 0; index < 3; ++index) {
        if (!folder_exists(hosts[index].folder)) {
            wprintf(L"  Skipped %ls: skills folder is missing.\n", hosts[index].name);
            continue;
        }
        wchar_t question[512] = {0};
        swprintf_s(question, _countof(question), L"Install or update Skills Zero, One, and Two in %ls?", hosts[index].name);
        if (!prompt_yes_no(question)) continue;
        deploy_core_skills(hosts[index].folder);
        wprintf(L"  Installed core skills in %ls.\n", hosts[index].name);
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

static void test_deployment(const Host *hosts) {
    print_heading(L"5. Test core-skill installation");
    int deployed = 0;
    for (int index = 0; index < 3; ++index) {
        if (!folder_exists(hosts[index].folder)) continue;
        wchar_t skill[MAX_PATH * 4] = {0};
        wcscpy_s(skill, _countof(skill), hosts[index].folder);
        append_path(skill, _countof(skill), L"WillEastbury!skillcli!skillcli-skill-zero\\SKILL.md");
        if (GetFileAttributesW(skill) != INVALID_FILE_ATTRIBUTES) {
            wprintf(L"  [ok] %ls\n", hosts[index].name);
            ++deployed;
        } else {
            wprintf(L"  [not installed] %ls\n", hosts[index].name);
        }
    }
    wprintf(L"\n  %d harness(es) have the core skills installed.\n", deployed);
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
    wprintf(L"  skillcli register <owner>/<repo>\n");
    wprintf(L"  skillcli self-update\n");
}

int wmain(int argc, wchar_t **argv) {
    (void)argc;
    (void)argv;
    if (!is_elevated()) {
        wchar_t executable[MAX_PATH * 4] = {0};
        if (!GetModuleFileNameW(NULL, executable, _countof(executable))) return 1;
        SHELLEXECUTEINFOW launch = {0};
        launch.cbSize = sizeof(launch);
        launch.fMask = SEE_MASK_NOCLOSEPROCESS;
        launch.lpVerb = L"runas";
        launch.lpFile = executable;
        launch.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&launch)) return 1;
        WaitForSingleObject(launch.hProcess, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(launch.hProcess, &exit_code);
        CloseHandle(launch.hProcess);
        return (int)exit_code;
    }

    wchar_t install_directory[MAX_PATH * 4] = {0};
    int installation = install_self(install_directory, _countof(install_directory));
    if (!installation) {
        fwprintf(stderr, L"skillcli could not install itself into the user profile.\n");
        return 1;
    }
    if (installation == 3) {
        show_usage();
        return 0;
    }
    Host hosts[3] = {0};
    print_splash();
    wprintf(L"\n  %ls\n", installation == 1 ? L"Installed skillcli." : L"Updated skillcli.");
    detect_hosts(hosts);
    show_hosts(hosts);
    offer_missing_hosts(hosts);
    detect_hosts(hosts);
    deploy_to_hosts(hosts);
    register_sources(install_directory);
    test_deployment(hosts);
    return 0;
}
