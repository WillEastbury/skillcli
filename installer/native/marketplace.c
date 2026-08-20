#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "marketplace.h"

#define CLI_MAX_SOURCES 64
#define CLI_MAX_PLUGINS 256
#define CLI_MAX_FILES 256
#define CLI_MAX_RESPONSE (16 * 1024 * 1024)

typedef struct {
    char *body;
    size_t length;
    DWORD status;
} CliResponse;

typedef struct {
    char id[128];
    char repository[256];
    char ref[128];
    char path[512];
    char host[256];
    char gh_user[128];
    char commit[65];
    char error[512];
    int private_source;
} CliSource;

typedef struct {
    CliSource values[CLI_MAX_SOURCES];
    size_t count;
} CliSources;

typedef struct {
    char relative[512];
    char remote[1024];
    char sha256[65];
} CliFile;

typedef struct {
    CliSource *source;
    char marketplace[128];
    char path[512];
    char name[128];
    char version[64];
    char description[1024];
    char *metadata;
} CliPlugin;

typedef struct {
    CliSources sources;
    CliPlugin plugins[CLI_MAX_PLUGINS];
    size_t count;
    size_t available;
} CliCatalogues;

typedef struct {
    const wchar_t *name;
    wchar_t root[MAX_PATH * 4];
} CliDestination;

static void cli_response_free(CliResponse *response) {
    free(response->body);
    response->body = NULL;
    response->length = 0;
    response->status = 0;
}

static int cli_http_get(const wchar_t *url, const wchar_t *headers, CliResponse *response) {
    URL_COMPONENTS parts = {0};
    wchar_t host[256] = {0};
    wchar_t path[4096] = {0};
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    int success = 0;

    cli_response_free(response);
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
    if (!request || (headers && !WinHttpAddRequestHeaders(request, headers, -1,
                                                            WINHTTP_ADDREQ_FLAG_ADD)) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) goto done;
    {
        DWORD size = sizeof(response->status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &response->status, &size,
                                 WINHTTP_NO_HEADER_INDEX)) goto done;
    }
    for (;;) {
        DWORD available = 0;
        DWORD read = 0;
        char *expanded;
        if (!WinHttpQueryDataAvailable(request, &available)) goto done;
        if (!available) break;
        if (available > CLI_MAX_RESPONSE || response->length > CLI_MAX_RESPONSE - available) goto done;
        expanded = realloc(response->body, response->length + available + 1);
        if (!expanded) goto done;
        response->body = expanded;
        if (!WinHttpReadData(request, response->body + response->length, available, &read)) goto done;
        response->length += read;
        response->body[response->length] = '\0';
    }
    success = response->status >= 200 && response->status < 300;
done:
    if (!success) cli_response_free(response);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return success;
}

static const char *cli_skip_ws(const char *value) {
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') ++value;
    return value;
}

static const char *cli_skip_string(const char *value) {
    if (*value++ != '"') return NULL;
    while (*value) {
        unsigned char character = (unsigned char)*value++;
        if (character == '"') return value;
        if (character < 0x20) return NULL;
        if (character == '\\') {
            int count = 0;
            if (!*value) return NULL;
            if (*value == 'u') {
                ++value;
                for (; count < 4; ++count, ++value) {
                    if (!((*value >= '0' && *value <= '9') ||
                          (*value >= 'a' && *value <= 'f') ||
                          (*value >= 'A' && *value <= 'F'))) return NULL;
                }
            } else if (!strchr("\"\\/bfnrt", *value++)) {
                return NULL;
            }
        }
    }
    return NULL;
}

static const char *cli_skip_value(const char *value) {
    const char *next;
    value = cli_skip_ws(value);
    if (*value == '"') return cli_skip_string(value);
    if (*value == '{') {
        value = cli_skip_ws(value + 1);
        if (*value == '}') return value + 1;
        for (;;) {
            if (!(value = cli_skip_string(value))) return NULL;
            value = cli_skip_ws(value);
            if (*value++ != ':') return NULL;
            if (!(value = cli_skip_value(value))) return NULL;
            value = cli_skip_ws(value);
            if (*value == '}') return value + 1;
            if (*value++ != ',') return NULL;
            value = cli_skip_ws(value);
        }
    }
    if (*value == '[') {
        value = cli_skip_ws(value + 1);
        if (*value == ']') return value + 1;
        for (;;) {
            if (!(value = cli_skip_value(value))) return NULL;
            value = cli_skip_ws(value);
            if (*value == ']') return value + 1;
            if (*value++ != ',') return NULL;
            value = cli_skip_ws(value);
        }
    }
    next = value;
    while (*next && !strchr(",]}\t\r\n ", *next)) ++next;
    if (next == value) return NULL;
    return next;
}

static int cli_copy_json_string(const char *value, char *output, size_t capacity) {
    const char *end;
    size_t length;
    if (!value || *value != '"') return 0;
    end = cli_skip_string(value);
    if (!end || end <= value + 1) {
        if (end == value + 2 && capacity) {
            output[0] = '\0';
            return 1;
        }
        return 0;
    }
    length = (size_t)(end - value - 2);
    if (length >= capacity || memchr(value + 1, '\\', length)) return 0;
    memcpy(output, value + 1, length);
    output[length] = '\0';
    return 1;
}

static int cli_object_value(const char *object, const char *key, const char **result) {
    const char *value = cli_skip_ws(object);
    size_t key_length = strlen(key);
    if (*value++ != '{') return 0;
    for (;;) {
        const char *name;
        const char *end;
        value = cli_skip_ws(value);
        if (*value == '}') return 0;
        name = value;
        end = cli_skip_string(value);
        if (!end) return 0;
        value = cli_skip_ws(end);
        if (*value++ != ':') return 0;
        value = cli_skip_ws(value);
        if ((size_t)(end - name - 2) == key_length &&
            !memcmp(name + 1, key, key_length)) {
            *result = value;
            return 1;
        }
        value = cli_skip_value(value);
        if (!value) return 0;
        value = cli_skip_ws(value);
        if (*value == '}') return 0;
        if (*value++ != ',') return 0;
    }
}

static int cli_string_field(const char *object, const char *key, char *output, size_t capacity) {
    const char *value;
    return cli_object_value(object, key, &value) && cli_copy_json_string(value, output, capacity);
}

static int cli_bool_field(const char *object, const char *key, int *output) {
    const char *value;
    if (!cli_object_value(object, key, &value)) return 0;
    if (!strncmp(value, "true", 4) && !strchr("abcdefghijklmnopqrstuvwxyz", value[4])) {
        *output = 1;
        return 1;
    }
    if (!strncmp(value, "false", 5) && !strchr("abcdefghijklmnopqrstuvwxyz", value[5])) {
        *output = 0;
        return 1;
    }
    return 0;
}

static int cli_array_field(const char *object, const char *key, const char **cursor) {
    const char *value;
    if (!cli_object_value(object, key, &value)) return 0;
    value = cli_skip_ws(value);
    if (*value != '[') return 0;
    *cursor = value + 1;
    return 1;
}

static int cli_array_next(const char **cursor, const char **item) {
    const char *value = cli_skip_ws(*cursor);
    const char *end;
    if (*value == ']') {
        *cursor = value + 1;
        return 0;
    }
    end = cli_skip_value(value);
    if (!end) return -1;
    *item = value;
    value = cli_skip_ws(end);
    if (*value == ',') {
        *cursor = value + 1;
        return 1;
    }
    if (*value == ']') {
        *cursor = value;
        return 1;
    }
    return -1;
}

static int cli_safe_component(const char *value) {
    size_t index;
    if (!value[0] || !strcmp(value, ".") || !strcmp(value, "..")) return 0;
    for (index = 0; value[index]; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (!(isalnum(character) || character == '.' || character == '_' || character == '-')) return 0;
    }
    return value[index - 1] != '.' && value[index - 1] != ' ';
}

static int cli_reserved_component(const char *value) {
    char stem[16] = {0};
    size_t index = 0;
    while (value[index] && value[index] != '.' && index + 1 < _countof(stem)) {
        stem[index] = (char)toupper((unsigned char)value[index]);
        ++index;
    }
    return !strcmp(stem, "CON") || !strcmp(stem, "PRN") || !strcmp(stem, "AUX") ||
           !strcmp(stem, "NUL") ||
           (strlen(stem) == 4 && !strncmp(stem, "COM", 3) &&
            stem[3] >= '1' && stem[3] <= '9') ||
           (strlen(stem) == 4 && !strncmp(stem, "LPT", 3) &&
            stem[3] >= '1' && stem[3] <= '9');
}

static int cli_safe_relative(const char *value) {
    const char *component = value;
    if (!value[0] || value[0] == '/' || value[strlen(value) - 1] == '/') return 0;
    for (;;) {
        const char *slash = strchr(component, '/');
        char part[256] = {0};
        size_t length = slash ? (size_t)(slash - component) : strlen(component);
        if (!length || length >= _countof(part)) return 0;
        memcpy(part, component, length);
        if (!cli_safe_component(part) || cli_reserved_component(part)) return 0;
        if (!slash) return 1;
        component = slash + 1;
    }
}

static int cli_repository_parts(const char *value, char *repository, size_t repository_capacity,
                                char *path, size_t path_capacity) {
    const char *first = strchr(value, '/');
    const char *second;
    size_t repository_length;
    if (!first || first == value || !first[1] || strchr(value, '\\') || strchr(value, ' ')) return 0;
    second = strchr(first + 1, '/');
    repository_length = second ? (size_t)(second - value) : strlen(value);
    if (repository_length >= repository_capacity) return 0;
    {
        char owner[128] = {0};
        char name[128] = {0};
        size_t owner_length = (size_t)(first - value);
        size_t name_length = second ? (size_t)(second - first - 1) : strlen(first + 1);
        if (owner_length >= _countof(owner) || name_length >= _countof(name)) return 0;
        memcpy(owner, value, owner_length);
        memcpy(name, first + 1, name_length);
        if (!cli_safe_component(owner) || !cli_safe_component(name)) return 0;
    }
    memcpy(repository, value, repository_length);
    repository[repository_length] = '\0';
    path[0] = '\0';
    if (!second) return 1;
    if (!second[1] || strlen(second + 1) >= path_capacity || !cli_safe_relative(second + 1)) return 0;
    strcpy_s(path, path_capacity, second + 1);
    return 1;
}

static int cli_utf8_to_wide(const char *value, wchar_t *output, size_t capacity) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, output, (int)capacity) != 0;
}

static int cli_module_directory(wchar_t *path, size_t capacity) {
    wchar_t *last;
    if (!GetModuleFileNameW(NULL, path, (DWORD)capacity)) return 0;
    last = wcsrchr(path, L'\\');
    if (!last) return 0;
    *last = L'\0';
    return 1;
}

static int cli_append_path(wchar_t *buffer, size_t capacity, const wchar_t *part) {
    size_t length = wcslen(buffer);
    size_t part_length = wcslen(part);
    if (length + part_length + 2 > capacity) return 0;
    if (length && buffer[length - 1] != L'\\') buffer[length++] = L'\\';
    wcscpy_s(buffer + length, capacity - length, part);
    return 1;
}

static int cli_path_directory(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int cli_ensure_directory(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    return CreateDirectoryW(path, NULL) &&
           !(GetFileAttributesW(path) & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int cli_read_local_file(const wchar_t *path, char **contents, size_t *length) {
    HANDLE file;
    LARGE_INTEGER size = {0};
    DWORD read = 0;
    DWORD attributes = GetFileAttributesW(path);
    *contents = NULL;
    *length = 0;
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) ||
        size.QuadPart < 0 || size.QuadPart > CLI_MAX_RESPONSE) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return 0;
    }
    *contents = calloc((size_t)size.QuadPart + 1, 1);
    if (!*contents || !ReadFile(file, *contents, (DWORD)size.QuadPart, &read, NULL) ||
        read != (DWORD)size.QuadPart) {
        free(*contents);
        *contents = NULL;
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    *length = read;
    return 1;
}

static int cli_command_exists(const wchar_t *command, wchar_t *resolved, size_t capacity);

static void cli_source_default(CliSource *source) {
    ZeroMemory(source, sizeof(*source));
    strcpy_s(source->id, _countof(source->id), "public");
    strcpy_s(source->repository, _countof(source->repository), "WillEastbury/skillcli");
    strcpy_s(source->ref, _countof(source->ref), "main");
}

static int cli_safe_host(const char *value) {
    size_t index;
    if (!value[0] || value[0] == '.' || value[0] == '-' ||
        value[strlen(value) - 1] == '.' || value[strlen(value) - 1] == '-') return 0;
    for (index = 0; value[index]; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (!(isalnum(character) || character == '.' || character == '-')) return 0;
    }
    return 1;
}

static int cli_source_uses_gh(const CliSource *source) {
    return source->private_source || (source->host[0] && _stricmp(source->host, "github.com"));
}

static int cli_source_valid(const CliSource *source) {
    char ignored[512] = {0};
    char repository[256] = {0};
    return cli_repository_parts(source->repository, repository, _countof(repository),
                                ignored, _countof(ignored)) &&
           !ignored[0] && cli_safe_component(source->id) &&
           cli_safe_component(source->ref) &&
           (!source->host[0] || cli_safe_host(source->host)) &&
           (!source->path[0] || cli_safe_relative(source->path));
}

static void cli_source_id(const CliSource *source, char *id, size_t capacity) {
    size_t output = 0;
    const char *parts[] = {source->repository, source->path};
    size_t part;
    for (part = 0; part < _countof(parts); ++part) {
        const char *value = parts[part];
        size_t index;
        if (!value[0]) continue;
        if (output && output + 1 < capacity) id[output++] = '-';
        for (index = 0; value[index] && output + 1 < capacity; ++index) {
            id[output++] = value[index] == '/' ? '-' : (char)tolower((unsigned char)value[index]);
        }
    }
    id[output] = '\0';
}

static int cli_sources_contains(const CliSources *sources, const char *repository, const char *path) {
    size_t index;
    for (index = 0; index < sources->count; ++index) {
        if (!_stricmp(sources->values[index].repository, repository) &&
            !_stricmp(sources->values[index].path, path)) return 1;
    }
    return 0;
}

static int cli_sources_add(CliSources *sources, const CliSource *source) {
    CliSource value = *source;
    if (!cli_source_valid(&value) || sources->count >= _countof(sources->values)) return 0;
    if (!value.id[0]) cli_source_id(&value, value.id, _countof(value.id));
    if (cli_sources_contains(sources, value.repository, value.path)) return 1;
    sources->values[sources->count++] = value;
    return 1;
}

static int cli_load_sources(const wchar_t *configuration, CliSources *sources) {
    char *contents = NULL;
    size_t length = 0;
    const char *cursor;
    const char *item;
    int next;
    ZeroMemory(sources, sizeof(*sources));
    {
        DWORD attributes = GetFileAttributesW(configuration);
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || (attributes & FILE_ATTRIBUTE_DIRECTORY))) {
            return 0;
        }
    }
    if (!cli_read_local_file(configuration, &contents, &length)) {
        CliSource source;
        cli_source_default(&source);
        return cli_sources_add(sources, &source);
    }
    if (!cli_array_field(contents, "sources", &cursor)) {
        free(contents);
        return 0;
    }
    while ((next = cli_array_next(&cursor, &item)) > 0) {
        CliSource source;
        ZeroMemory(&source, sizeof(source));
        strcpy_s(source.ref, _countof(source.ref), "main");
        if (!cli_string_field(item, "repository", source.repository, _countof(source.repository))) {
            free(contents);
            return 0;
        }
        cli_string_field(item, "id", source.id, _countof(source.id));
        cli_string_field(item, "ref", source.ref, _countof(source.ref));
        cli_string_field(item, "path", source.path, _countof(source.path));
        cli_string_field(item, "host", source.host, _countof(source.host));
        cli_string_field(item, "ghUser", source.gh_user, _countof(source.gh_user));
        cli_bool_field(item, "private", &source.private_source);
        if (!cli_sources_add(sources, &source)) {
            free(contents);
            return 0;
        }
    }
    free(contents);
    return next == 0 && sources->count != 0;
}

static int cli_write_sources(const wchar_t *configuration, const CliSources *sources) {
    wchar_t temporary[MAX_PATH * 4] = {0};
    FILE *file = NULL;
    DWORD attributes = GetFileAttributesW(configuration);
    size_t index;
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    if (wcslen(configuration) + 5 >= _countof(temporary)) return 0;
    wcscpy_s(temporary, _countof(temporary), configuration);
    wcscat_s(temporary, _countof(temporary), L".new");
    if (GetFileAttributesW(temporary) != INVALID_FILE_ATTRIBUTES) return 0;
    if (_wfopen_s(&file, temporary, L"wb") != 0 || !file) return 0;
    fputs("{\n  \"sources\": [\n", file);
    for (index = 0; index < sources->count; ++index) {
        const CliSource *source = &sources->values[index];
        fprintf(file, "    {\n      \"id\": \"%s\",\n      \"repository\": \"%s\",\n",
                source->id, source->repository);
        if (source->path[0]) fprintf(file, "      \"path\": \"%s\",\n", source->path);
        if (source->host[0]) fprintf(file, "      \"host\": \"%s\",\n", source->host);
        if (source->gh_user[0]) fprintf(file, "      \"ghUser\": \"%s\",\n", source->gh_user);
        fprintf(file, "      \"ref\": \"%s\",\n      \"private\": %s\n    }%s\n",
                source->ref, source->private_source ? "true" : "false",
                index + 1 == sources->count ? "" : ",");
    }
    fputs("  ]\n}\n", file);
    if (fclose(file) || !MoveFileExW(temporary, configuration,
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        return 0;
    }
    return 1;
}

static void cli_source_error(CliSource *source, const char *message) {
    strcpy_s(source->error, _countof(source->error), message);
}

static int cli_can_prompt(void) {
    DWORD mode = 0;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    return input != INVALID_HANDLE_VALUE && GetFileType(input) == FILE_TYPE_CHAR &&
           GetConsoleMode(input, &mode);
}

static int cli_prompt_for_gh_login(const char *host) {
    wchar_t response[16] = {0};
    wprintf(L"Authentication is required for %S. Run `gh auth login --hostname %S` now? [y/N]: ",
            host, host);
    return fgetws(response, _countof(response), stdin) &&
           (response[0] == L'y' || response[0] == L'Y');
}

static int cli_prompt_for_gh_install(void) {
    wchar_t response[16] = {0};
    wprintf(L"GitHub CLI (`gh`) is required for this source. Install it with winget now? [y/N]: ");
    return fgetws(response, _countof(response), stdin) &&
           (response[0] == L'y' || response[0] == L'Y');
}

static int cli_run_winget_gh_install(void) {
    wchar_t executable[MAX_PATH * 4] = {0};
    wchar_t command[1024] = {0};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    DWORD exit_code = 1;
    if (!cli_command_exists(L"winget", executable, _countof(executable)) ||
        swprintf_s(command, _countof(command),
                   L"\"%ls\" install --id GitHub.cli --exact --source winget "
                   L"--accept-source-agreements --accept-package-agreements", executable) < 0) {
        return 0;
    }
    startup.cb = sizeof(startup);
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        return 0;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

static int cli_ensure_gh(CliSource *source) {
    wchar_t executable[MAX_PATH * 4] = {0};
    const char *host = source->host[0] ? source->host : "github.com";
    if (cli_command_exists(L"gh", executable, _countof(executable))) return 1;
    if (!cli_can_prompt() || !cli_prompt_for_gh_install()) {
        sprintf_s(source->error, _countof(source->error),
                  "private or GitHub Enterprise source requires gh; install GitHub CLI and run "
                  "`gh auth login --hostname %s`.", host);
        return 0;
    }
    if (!cli_run_winget_gh_install()) {
        sprintf_s(source->error, _countof(source->error),
                  "could not install gh with winget; install GitHub CLI, then run "
                  "`gh auth login --hostname %s`.", host);
        return 0;
    }
    if (!cli_command_exists(L"gh", executable, _countof(executable))) {
        sprintf_s(source->error, _countof(source->error),
                  "gh was installed but is not yet on PATH; restart the terminal, then run "
                  "`gh auth login --hostname %s`.", host);
        return 0;
    }
    return 1;
}

static int cli_run_gh(CliSource *source, const wchar_t *arguments, int interactive) {
    wchar_t executable[MAX_PATH * 4] = {0};
    wchar_t command[4096] = {0};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    HANDLE null_output = INVALID_HANDLE_VALUE;
    DWORD exit_code = 1;
    const char *host = source->host[0] ? source->host : "github.com";
    if (!cli_command_exists(L"gh", executable, _countof(executable))) {
        sprintf_s(source->error, _countof(source->error),
                  "private or GitHub Enterprise source requires the host-managed gh CLI; "
                  "install gh and run `gh auth login --hostname %s`.", host);
        return 0;
    }
    if (swprintf_s(command, _countof(command), L"\"%ls\" %ls", executable, arguments) < 0) return 0;
    startup.cb = sizeof(startup);
    if (!interactive) {
        null_output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (null_output == INVALID_HANDLE_VALUE) return 0;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = null_output;
        startup.hStdError = null_output;
    }
    if (!CreateProcessW(executable, command, NULL, NULL, interactive ? FALSE : TRUE,
                        interactive ? 0 : CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        if (null_output != INVALID_HANDLE_VALUE) CloseHandle(null_output);
        return 0;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (null_output != INVALID_HANDLE_VALUE) CloseHandle(null_output);
    return exit_code == 0;
}

static int cli_gh_authenticated(CliSource *source) {
    wchar_t arguments[512] = {0};
    const char *host = source->host[0] ? source->host : "github.com";
    if (!cli_ensure_gh(source)) return 0;
    if (swprintf_s(arguments, _countof(arguments), L"auth status --hostname %S", host) >= 0 &&
        cli_run_gh(source, arguments, 0)) return 1;
    if (source->error[0]) return 0;
    sprintf_s(source->error, _countof(source->error),
              "gh is not authorised for %s; run `gh auth login --hostname %s` "
              "with an account authorised for the repository.", host, host);
    if (!cli_can_prompt() || !cli_prompt_for_gh_login(host) ||
        swprintf_s(arguments, _countof(arguments), L"auth login --hostname %S", host) < 0 ||
        !cli_run_gh(source, arguments, 1) ||
        swprintf_s(arguments, _countof(arguments), L"auth status --hostname %S", host) < 0 ||
        !cli_run_gh(source, arguments, 0)) {
        sprintf_s(source->error, _countof(source->error),
                  "gh authentication is still unavailable for %s; run "
                  "`gh auth login --hostname %s` and confirm repository access.", host, host);
        return 0;
    }
    source->error[0] = '\0';
    return 1;
}

static int cli_gh_api(CliSource *source, const wchar_t *endpoint, int raw,
                      CliResponse *response) {
    wchar_t executable[MAX_PATH * 4] = {0};
    wchar_t command[4096] = {0};
    const char *host = source->host[0] ? source->host : "github.com";
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    DWORD read = 0;
    DWORD exit_code = 1;
    int success = 0;

    cli_response_free(response);
    if (!cli_command_exists(L"gh", executable, _countof(executable))) {
        sprintf_s(source->error, _countof(source->error),
                  "private or GitHub Enterprise source requires the host-managed gh CLI; "
                  "install gh and run `gh auth login --hostname %s`.", host);
        return 0;
    }
    if (swprintf_s(command, _countof(command),
                   raw ? L"\"%ls\" api --hostname %S -H \"Accept: application/vnd.github.raw+json\" %ls"
                       : L"\"%ls\" api --hostname %S %ls",
                   executable, host, endpoint) < 0 ||
        !CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) goto done;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    if (!CreateProcessW(executable, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process)) goto done;
    CloseHandle(write_pipe);
    write_pipe = NULL;
    for (;;) {
        DWORD available = 0;
        char *expanded;
        if (!PeekNamedPipe(read_pipe, NULL, 0, NULL, &available, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            goto done;
        }
        if (!available) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
            Sleep(10);
            continue;
        }
        if (available > CLI_MAX_RESPONSE || response->length > CLI_MAX_RESPONSE - available) goto done;
        expanded = realloc(response->body, response->length + available + 1);
        if (!expanded) goto done;
        response->body = expanded;
        if (!ReadFile(read_pipe, response->body + response->length, available, &read, NULL)) goto done;
        response->length += read;
        response->body[response->length] = '\0';
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    success = exit_code == 0;
    if (success) {
        response->status = 200;
    } else {
        sprintf_s(source->error, _countof(source->error),
                  "gh could not access %s on %s; run `gh auth status --hostname %s`, "
                  "then `gh auth login --hostname %s` with an account authorised for the repository.",
                  source->repository, host, host, host);
    }
done:
    if (!success) cli_response_free(response);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) {
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
    }
    if (write_pipe) CloseHandle(write_pipe);
    if (read_pipe) CloseHandle(read_pipe);
    return success;
}

static int cli_source_resolve(CliSource *source) {
    wchar_t url[1024] = {0};
    CliResponse response = {0};
    char sha[65] = {0};
    size_t index;
    if (source->commit[0]) return 1;
    source->error[0] = '\0';
    if ((cli_source_uses_gh(source) &&
         (!cli_gh_authenticated(source) ||
          swprintf_s(url, _countof(url), L"repos/%S/commits/%S",
                     source->repository, source->ref) < 0 ||
          !cli_gh_api(source, url, 0, &response))) ||
        (!cli_source_uses_gh(source) &&
         (swprintf_s(url, _countof(url), L"https://api.github.com/repos/%S/commits/%S",
                     source->repository, source->ref) < 0 ||
          !cli_http_get(url, L"User-Agent: skillcli/1.0\r\n", &response))) ||
        !cli_string_field(response.body, "sha", sha, _countof(sha))) {
        if (!source->error[0]) cli_source_error(source, "HTTPS commit resolution failed.");
        cli_response_free(&response);
        return 0;
    }
    for (index = 0; sha[index]; ++index) {
        if (!isxdigit((unsigned char)sha[index])) {
            cli_response_free(&response);
            return 0;
        }
    }
    if (index != 40 && index != 64) {
        cli_response_free(&response);
        return 0;
    }
    strcpy_s(source->commit, _countof(source->commit), sha);
    cli_response_free(&response);
    return 1;
}

static int cli_source_read(CliSource *source, const char *relative, CliResponse *response) {
    char full_path[1024] = {0};
    wchar_t url[2048] = {0};
    if (!cli_safe_relative(relative) || !cli_source_resolve(source)) return 0;
    if (source->path[0]) {
        if (strlen(source->path) + 1 + strlen(relative) >= _countof(full_path)) return 0;
        strcpy_s(full_path, _countof(full_path), source->path);
        strcat_s(full_path, _countof(full_path), "/");
        strcat_s(full_path, _countof(full_path), relative);
    } else {
        strcpy_s(full_path, _countof(full_path), relative);
    }
    if (cli_source_uses_gh(source)) {
        if (swprintf_s(url, _countof(url), L"repos/%S/contents/%S?ref=%S",
                       source->repository, full_path, source->commit) < 0) return 0;
        return cli_gh_api(source, url, 1, response);
    } else if (swprintf_s(url, _countof(url),
                          L"https://raw.githubusercontent.com/%S/%S/%S",
                          source->repository, source->commit, full_path) < 0) {
        return 0;
    }
    return cli_http_get(url, L"User-Agent: skillcli/1.0\r\n", response);
}

static int cli_sha256(const char *contents, size_t length, char output[65]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    DWORD object_length = 0;
    DWORD hash_length = 0;
    DWORD result_length = 0;
    PUCHAR object = NULL;
    unsigned char digest[32] = {0};
    static const char hex[] = "0123456789abcdef";
    size_t index;
    int success = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_length,
                          sizeof(object_length), &result_length, 0) ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_length,
                          sizeof(hash_length), &result_length, 0) || hash_length != sizeof(digest)) goto done;
    object = malloc(object_length);
    if (!object || BCryptCreateHash(algorithm, &hash, object, object_length, NULL, 0, 0) ||
        BCryptHashData(hash, (PUCHAR)contents, (ULONG)length, 0) ||
        BCryptFinishHash(hash, digest, sizeof(digest), 0)) goto done;
    for (index = 0; index < _countof(digest); ++index) {
        output[index * 2] = hex[digest[index] >> 4];
        output[index * 2 + 1] = hex[digest[index] & 15];
    }
    output[64] = '\0';
    success = 1;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    free(object);
    return success;
}

static int cli_metadata_is_approved(const char *metadata) {
    const char *review;
    char state[32] = {0};
    return cli_object_value(metadata, "review", &review) &&
           cli_string_field(review, "state", state, _countof(state)) &&
           !strcmp(state, "approved");
}

static int cli_plugin_qualified_id(const CliPlugin *plugin, char *output, size_t capacity) {
    return sprintf_s(output, capacity, "%s/%s", plugin->source->repository, plugin->name) > 0;
}

static int cli_plugin_exists(const CliCatalogues *catalogues, const CliPlugin *candidate) {
    char qualified[512] = {0};
    size_t index;
    if (!cli_plugin_qualified_id(candidate, qualified, _countof(qualified))) return 1;
    for (index = 0; index < catalogues->count; ++index) {
        char existing[512] = {0};
        cli_plugin_qualified_id(&catalogues->plugins[index], existing, _countof(existing));
        if (!_stricmp(existing, qualified)) return 1;
    }
    return 0;
}

static void cli_catalogues_free(CliCatalogues *catalogues) {
    size_t index;
    for (index = 0; index < catalogues->count; ++index) {
        free(catalogues->plugins[index].metadata);
        catalogues->plugins[index].metadata = NULL;
    }
    ZeroMemory(catalogues, sizeof(*catalogues));
}

static int cli_load_catalogues(const wchar_t *configuration, CliCatalogues *catalogues) {
    size_t source_index;
    ZeroMemory(catalogues, sizeof(*catalogues));
    if (!cli_load_sources(configuration, &catalogues->sources)) {
        fwprintf(stderr, L"skillcli error: sources.json must contain valid source records.\n");
        return 0;
    }
    for (source_index = 0; source_index < catalogues->sources.count; ++source_index) {
        CliSource *source = &catalogues->sources.values[source_index];
        CliResponse catalogue = {0};
        CliResponse marketplace = {0};
        const char *library;
        const char *cursor;
        const char *entry;
        int next;
        char namespace_value[256] = {0};
        char marketplace_name[128] = {0};
        if (!cli_source_read(source, "catalogue.json", &catalogue) ||
            !cli_source_read(source, ".github/plugin/marketplace.json", &marketplace) ||
            !cli_object_value(catalogue.body, "library", &library) ||
            !cli_string_field(library, "namespace", namespace_value, _countof(namespace_value)) ||
            strcmp(namespace_value, source->repository) ||
            !cli_string_field(marketplace.body, "name", marketplace_name,
                              _countof(marketplace_name)) ||
            !cli_safe_component(marketplace_name) ||
            !cli_array_field(marketplace.body, "plugins", &cursor)) {
            fwprintf(stderr, L"warning: %S: %S\n", source->id,
                     source->error[0] ? source->error : "could not load a valid marketplace");
            cli_response_free(&catalogue);
            cli_response_free(&marketplace);
            continue;
        }
        ++catalogues->available;
        while ((next = cli_array_next(&cursor, &entry)) > 0) {
            CliPlugin candidate;
            CliResponse manifest = {0};
            CliResponse metadata = {0};
            char expected_name[128] = {0};
            char expected_version[64] = {0};
            char manifest_name[128] = {0};
            char manifest_version[64] = {0};
            char plugin_path[512] = {0};
            char plugin_manifest[1024] = {0};
            char plugin_metadata[1024] = {0};
            ZeroMemory(&candidate, sizeof(candidate));
            if (!cli_string_field(entry, "source", plugin_path, _countof(plugin_path)) ||
                !cli_string_field(entry, "name", expected_name, _countof(expected_name)) ||
                !cli_string_field(entry, "version", expected_version, _countof(expected_version)) ||
                !cli_safe_relative(plugin_path) || !cli_safe_component(expected_name) ||
                strlen(plugin_path) + sizeof("/plugin.json") > _countof(plugin_manifest) ||
                strlen(plugin_path) + sizeof("/skillcli.json") > _countof(plugin_metadata)) {
                fwprintf(stderr, L"warning: %S has an unsafe marketplace plugin entry.\n", source->id);
                continue;
            }
            sprintf_s(plugin_manifest, _countof(plugin_manifest), "%s/plugin.json", plugin_path);
            sprintf_s(plugin_metadata, _countof(plugin_metadata), "%s/skillcli.json", plugin_path);
            if (!cli_source_read(source, plugin_manifest, &manifest) ||
                !cli_source_read(source, plugin_metadata, &metadata) ||
                !cli_string_field(manifest.body, "name", manifest_name, _countof(manifest_name)) ||
                !cli_string_field(manifest.body, "version", manifest_version, _countof(manifest_version)) ||
                strcmp(expected_name, manifest_name) || strcmp(expected_version, manifest_version) ||
                !cli_safe_component(manifest_name)) {
                fwprintf(stderr, L"warning: %S/%S has invalid plugin metadata.\n",
                         source->id, plugin_path);
                cli_response_free(&manifest);
                cli_response_free(&metadata);
                continue;
            }
            candidate.source = source;
            strcpy_s(candidate.marketplace, _countof(candidate.marketplace), marketplace_name);
            strcpy_s(candidate.path, _countof(candidate.path), plugin_path);
            strcpy_s(candidate.name, _countof(candidate.name), manifest_name);
            strcpy_s(candidate.version, _countof(candidate.version), manifest_version);
            cli_string_field(manifest.body, "description", candidate.description,
                             _countof(candidate.description));
            candidate.metadata = metadata.body;
            metadata.body = NULL;
            if (cli_plugin_exists(catalogues, &candidate) ||
                catalogues->count >= _countof(catalogues->plugins)) {
                fwprintf(stderr, L"warning: duplicate or excess plugin %S/%S was ignored.\n",
                         source->repository, candidate.name);
                free(candidate.metadata);
            } else {
                catalogues->plugins[catalogues->count++] = candidate;
            }
            cli_response_free(&manifest);
            cli_response_free(&metadata);
        }
        if (next < 0) fwprintf(stderr, L"warning: %S has malformed plugin entries.\n", source->id);
        cli_response_free(&catalogue);
        cli_response_free(&marketplace);
    }
    if (!catalogues->available) {
        cli_catalogues_free(catalogues);
        fwprintf(stderr, L"skillcli error: no configured plugin marketplace could be loaded.\n");
        return 0;
    }
    return 1;
}

static CliPlugin *cli_find_plugin(CliCatalogues *catalogues, const char *qualified_id) {
    size_t index;
    if (!cli_safe_relative(qualified_id)) return NULL;
    for (index = 0; index < catalogues->count; ++index) {
        char candidate[512] = {0};
        cli_plugin_qualified_id(&catalogues->plugins[index], candidate, _countof(candidate));
        if (!_stricmp(candidate, qualified_id)) return &catalogues->plugins[index];
    }
    return NULL;
}

static int cli_qualified_id_valid(const char *value) {
    const char *first = strchr(value, '/');
    const char *second = first ? strchr(first + 1, '/') : NULL;
    char repository[256] = {0};
    char path[512] = {0};
    if (!first || !second || strchr(second + 1, '/') || !second[1] ||
        (size_t)(second - value) >= _countof(repository)) return 0;
    memcpy(repository, value, (size_t)(second - value));
    return cli_repository_parts(repository, repository, _countof(repository), path, _countof(path)) &&
           !path[0] && cli_safe_component(second + 1);
}

static int cli_list_contains(const char *array_object, const char *field, const char *needle) {
    const char *cursor;
    const char *item;
    int next;
    char value[256] = {0};
    if (!cli_array_field(array_object, field, &cursor)) return 0;
    while ((next = cli_array_next(&cursor, &item)) > 0) {
        if (!cli_copy_json_string(item, value, _countof(value))) return 0;
        if (!_stricmp(value, needle)) return 1;
    }
    return 0;
}

static int cli_word_match(const char *text, const char *word) {
    size_t length = strlen(word);
    const char *current;
    if (!length) return 0;
    for (current = text; *current; ++current) {
        if ((current == text || !isalnum((unsigned char)current[-1])) &&
            !_strnicmp(current, word, length) &&
            !isalnum((unsigned char)current[length])) return 1;
    }
    return 0;
}

static size_t cli_terms(const char *text, char values[][64], size_t capacity) {
    size_t count = 0;
    while (*text) {
        char term[64] = {0};
        size_t length = 0;
        size_t index;
        while (*text && !isalnum((unsigned char)*text)) ++text;
        while (isalnum((unsigned char)*text) && length + 1 < _countof(term)) {
            term[length++] = (char)tolower((unsigned char)*text++);
        }
        while (isalnum((unsigned char)*text)) ++text;
        if (length < 2) continue;
        for (index = 0; index < count; ++index) {
            if (!strcmp(values[index], term)) break;
        }
        if (index == count && count < capacity) strcpy_s(values[count++], _countof(values[0]), term);
    }
    return count;
}

static int cli_plugin_term_match(const CliPlugin *plugin, const char *term) {
    const char *cursor;
    const char *item;
    int next;
    char value[512] = {0};
    if (cli_word_match(plugin->name, term) || cli_word_match(plugin->description, term)) return 1;
    if (cli_array_field(plugin->metadata, "taskCategories", &cursor)) {
        while ((next = cli_array_next(&cursor, &item)) > 0) {
            if (cli_copy_json_string(item, value, _countof(value)) && cli_word_match(value, term)) return 1;
        }
    }
    return 0;
}

static int cli_plugin_score(const CliPlugin *plugin, const char *role, const char *query,
                            char *why, size_t why_capacity) {
    char terms[32][64] = {{0}};
    size_t count = cli_terms(query, terms, _countof(terms));
    size_t index;
    int matched = 0;
    int role_match = cli_list_contains(plugin->metadata, "roles", role);
    why[0] = '\0';
    if (!cli_metadata_is_approved(plugin->metadata)) return -1;
    for (index = 0; index < count; ++index) {
        if (cli_plugin_term_match(plugin, terms[index])) {
            if (why[0]) strcat_s(why, why_capacity, ", ");
            strcat_s(why, why_capacity, terms[index]);
            ++matched;
        }
    }
    if (count && !matched) return -1;
    if (!why[0]) strcpy_s(why, why_capacity, role_match ? "role match" : "approved");
    return matched * 4 + (role_match ? 3 : 0);
}

static int cli_plugin_files(const CliPlugin *plugin, CliFile *files, size_t *count) {
    const char *cursor;
    const char *item;
    int next;
    char skill_root[512] = {0};
    size_t root_length;
    *count = 0;
    if (!cli_string_field(plugin->metadata, "skillRoot", skill_root, _countof(skill_root)) ||
        !cli_safe_relative(skill_root) ||
        !cli_array_field(plugin->metadata, "files", &cursor)) return 0;
    root_length = strlen(skill_root);
    while ((next = cli_array_next(&cursor, &item)) > 0) {
        char path[1024] = {0};
        char target[32] = {0};
        char digest[65] = {0};
        CliFile *file;
        size_t index;
        if (!cli_string_field(item, "target", target, _countof(target))) return 0;
        if (strcmp(target, "skill")) continue;
        if (!cli_string_field(item, "path", path, _countof(path)) ||
            !cli_string_field(item, "sha256", digest, _countof(digest)) ||
            !cli_safe_relative(path) || strncmp(path, skill_root, root_length) ||
            path[root_length] != '/' || !path[root_length + 1] || strlen(digest) != 64 ||
            *count >= CLI_MAX_FILES) return 0;
        for (index = 0; index < 64; ++index) {
            if (!((digest[index] >= '0' && digest[index] <= '9') ||
                  (digest[index] >= 'a' && digest[index] <= 'f'))) return 0;
        }
        file = &files[(*count)++];
        strcpy_s(file->relative, _countof(file->relative), path + root_length + 1);
        if (!cli_safe_relative(file->relative) ||
            sprintf_s(file->remote, _countof(file->remote), "%s/%s", plugin->path, path) < 0) return 0;
        strcpy_s(file->sha256, _countof(file->sha256), digest);
        for (index = 0; index + 1 < *count; ++index) {
            if (!_stricmp(files[index].relative, file->relative)) return 0;
        }
    }
    if (next < 0 || !*count) return 0;
    for (next = 0; next < (int)*count; ++next) {
        if (!strcmp(files[next].relative, "SKILL.md")) return 1;
    }
    return 0;
}

static int cli_validate_declared_files(const CliPlugin *plugin) {
    const char *cursor;
    const char *item;
    int next;
    if (!cli_array_field(plugin->metadata, "files", &cursor)) return 0;
    while ((next = cli_array_next(&cursor, &item)) > 0) {
        char path[1024] = {0};
        char target[32] = {0};
        char expected[65] = {0};
        char remote[1536] = {0};
        char actual[65] = {0};
        CliResponse response = {0};
        size_t index;
        if (!cli_string_field(item, "path", path, _countof(path)) ||
            !cli_string_field(item, "target", target, _countof(target)) ||
            !cli_string_field(item, "sha256", expected, _countof(expected)) ||
            !cli_safe_relative(path) || (strcmp(target, "skill") && strcmp(target, "tool")) ||
            strlen(expected) != 64 ||
            sprintf_s(remote, _countof(remote), "%s/%s", plugin->path, path) < 0) return 0;
        for (index = 0; index < 64; ++index) {
            if (!((expected[index] >= '0' && expected[index] <= '9') ||
                  (expected[index] >= 'a' && expected[index] <= 'f'))) return 0;
        }
        if (!cli_source_read(plugin->source, remote, &response) ||
            !cli_sha256(response.body, response.length, actual) || strcmp(actual, expected)) {
            cli_response_free(&response);
            return 0;
        }
        cli_response_free(&response);
    }
    return next == 0;
}

static int cli_hash_file(const wchar_t *path, char digest[65]) {
    char *contents = NULL;
    size_t length = 0;
    int success = cli_read_local_file(path, &contents, &length) && cli_sha256(contents, length, digest);
    free(contents);
    return success;
}

static int cli_make_qualified_folder(const CliPlugin *plugin, char *output, size_t capacity) {
    char repository[256] = {0};
    size_t index;
    if (strlen(plugin->source->repository) + strlen(plugin->name) + 2 >= capacity) return 0;
    strcpy_s(repository, _countof(repository), plugin->source->repository);
    for (index = 0; repository[index]; ++index) {
        if (repository[index] == '/') repository[index] = '!';
    }
    return sprintf_s(output, capacity, "%s!%s", repository, plugin->name) > 0;
}

static int cli_target_path(const wchar_t *root, const CliPlugin *plugin,
                           wchar_t *target, size_t capacity) {
    char folder[512] = {0};
    wchar_t wide_folder[512] = {0};
    if (!cli_make_qualified_folder(plugin, folder, _countof(folder)) ||
        !cli_utf8_to_wide(folder, wide_folder, _countof(wide_folder)) ||
        wcslen(root) + wcslen(wide_folder) + 2 > capacity) return 0;
    wcscpy_s(target, capacity, root);
    return cli_append_path(target, capacity, wide_folder);
}

static int cli_relative_path(const wchar_t *root, const char *relative,
                             wchar_t *path, size_t capacity, int make_directories) {
    const char *component = relative;
    wchar_t current[MAX_PATH * 4] = {0};
    if (!cli_safe_relative(relative) || wcslen(root) >= _countof(current)) return 0;
    wcscpy_s(current, _countof(current), root);
    for (;;) {
        const char *slash = strchr(component, '/');
        char part[256] = {0};
        wchar_t wide_part[256] = {0};
        size_t length = slash ? (size_t)(slash - component) : strlen(component);
        if (!length || length >= _countof(part)) return 0;
        memcpy(part, component, length);
        if (!cli_utf8_to_wide(part, wide_part, _countof(wide_part)) ||
            !cli_append_path(current, _countof(current), wide_part)) return 0;
        if (!slash) {
            if (wcslen(current) >= capacity) return 0;
            wcscpy_s(path, capacity, current);
            return 1;
        }
        if (make_directories && !cli_ensure_directory(current)) return 0;
        if (!make_directories && !cli_path_directory(current)) return 0;
        component = slash + 1;
    }
}

static int cli_write_staged_file(const wchar_t *root, const CliFile *file,
                                 const char *contents, size_t length) {
    wchar_t destination[MAX_PATH * 4] = {0};
    HANDLE handle;
    DWORD written = 0;
    if (!cli_relative_path(root, file->relative, destination, _countof(destination), 1)) return 0;
    if (GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES) return 0;
    handle = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(handle, contents, (DWORD)length, &written, NULL) || written != length) {
        CloseHandle(handle);
        DeleteFileW(destination);
        return 0;
    }
    CloseHandle(handle);
    return 1;
}

static int cli_write_receipt(const wchar_t *stage, const CliPlugin *plugin,
                             const CliFile *files, size_t count) {
    wchar_t path[MAX_PATH * 4] = {0};
    HANDLE handle;
    DWORD written = 0;
    char text[65536] = {0};
    size_t used = 0;
    char qualified[512] = {0};
    size_t index;
    if (!cli_plugin_qualified_id(plugin, qualified, _countof(qualified)) ||
        wcslen(stage) >= _countof(path)) return 0;
    wcscpy_s(path, _countof(path), stage);
    if (!cli_append_path(path, _countof(path), L".skillcli.json")) return 0;
    used += (size_t)sprintf_s(text + used, _countof(text) - used,
                              "{\n  \"qualifiedId\": \"%s\",\n"
                              "  \"pluginName\": \"%s\",\n"
                              "  \"marketplace\": \"%s\",\n"
                              "  \"source\": {\"id\": \"%s\", \"repository\": \"%s\", "
                              "\"path\": \"%s\", \"host\": \"%s\", \"ref\": \"%s\", \"commit\": \"%s\"},\n"
                              "  \"version\": \"%s\",\n  \"files\": [\n",
                              qualified, plugin->name, plugin->marketplace, plugin->source->id,
                              plugin->source->repository, plugin->source->path, plugin->source->host,
                              plugin->source->ref, plugin->source->commit, plugin->version);
    if (used >= _countof(text)) return 0;
    for (index = 0; index < count; ++index) {
        int wrote = sprintf_s(text + used, _countof(text) - used,
                              "    {\"relativePath\": \"%s\", \"sha256\": \"%s\"}%s\n",
                              files[index].relative, files[index].sha256,
                              index + 1 == count ? "" : ",");
        if (wrote < 0) return 0;
        used += (size_t)wrote;
    }
    if (used + 7 >= _countof(text)) return 0;
    memcpy(text + used, "  ]\n}\n", 7);
    used += 7;
    handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(handle, text, (DWORD)used, &written, NULL) || written != used) {
        CloseHandle(handle);
        DeleteFileW(path);
        return 0;
    }
    CloseHandle(handle);
    return 1;
}

static int cli_receipt_matches(const wchar_t *target, const CliPlugin *plugin) {
    wchar_t path[MAX_PATH * 4] = {0};
    char *contents = NULL;
    size_t length = 0;
    const char *source;
    char qualified[512] = {0};
    char value[512] = {0};
    int result = 0;
    if (wcslen(target) + wcslen(L"\\.skillcli.json") + 1 >= _countof(path)) return 0;
    wcscpy_s(path, _countof(path), target);
    wcscat_s(path, _countof(path), L"\\.skillcli.json");
    if (!cli_read_local_file(path, &contents, &length) ||
        !cli_plugin_qualified_id(plugin, qualified, _countof(qualified)) ||
        !cli_string_field(contents, "qualifiedId", value, _countof(value)) ||
        strcmp(value, qualified) || !cli_object_value(contents, "source", &source) ||
        !cli_string_field(source, "repository", value, _countof(value)) ||
        strcmp(value, plugin->source->repository) ||
        !cli_string_field(source, "path", value, _countof(value)) ||
        strcmp(value, plugin->source->path) ||
        ((plugin->source->host[0] &&
          (!cli_string_field(source, "host", value, _countof(value)) ||
           strcmp(value, plugin->source->host))) ||
         (!plugin->source->host[0] &&
          cli_string_field(source, "host", value, _countof(value)) && value[0])) ||
        !cli_string_field(source, "commit", value, _countof(value)) || !value[0]) goto done;
    result = 1;
done:
    free(contents);
    return result;
}

static int cli_file_index(const CliFile *files, size_t count, const char *relative) {
    size_t index;
    for (index = 0; index < count; ++index) {
        if (!_stricmp(files[index].relative, relative)) return (int)index;
    }
    return -1;
}

static int cli_has_file_prefix(const CliFile *files, size_t count, const char *relative) {
    size_t index;
    size_t length = strlen(relative);
    for (index = 0; index < count; ++index) {
        if (!_strnicmp(files[index].relative, relative, length) &&
            files[index].relative[length] == '/') return 1;
    }
    return 0;
}

static int cli_wide_to_utf8(const wchar_t *value, char *output, size_t capacity) {
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, output,
                               (int)capacity, NULL, NULL) != 0;
}

static int cli_validate_tree(const wchar_t *current, const char *prefix,
                             const CliFile *files, size_t count, int *seen) {
    wchar_t pattern[MAX_PATH * 4] = {0};
    WIN32_FIND_DATAW data;
    HANDLE find;
    if (wcslen(current) + 3 >= _countof(pattern)) return 0;
    wcscpy_s(pattern, _countof(pattern), current);
    wcscat_s(pattern, _countof(pattern), L"\\*");
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        char name[512] = {0};
        char relative[1024] = {0};
        wchar_t child[MAX_PATH * 4] = {0};
        int index;
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !cli_wide_to_utf8(data.cFileName, name, _countof(name)) ||
            !cli_safe_component(name)) {
            FindClose(find);
            return 0;
        }
        if (prefix[0]) {
            if (sprintf_s(relative, _countof(relative), "%s/%s", prefix, name) < 0) {
                FindClose(find);
                return 0;
            }
        } else {
            strcpy_s(relative, _countof(relative), name);
        }
        if (wcslen(current) + wcslen(data.cFileName) + 2 >= _countof(child)) {
            FindClose(find);
            return 0;
        }
        wcscpy_s(child, _countof(child), current);
        wcscat_s(child, _countof(child), L"\\");
        wcscat_s(child, _countof(child), data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!cli_has_file_prefix(files, count, relative) ||
                !cli_validate_tree(child, relative, files, count, seen)) {
                FindClose(find);
                return 0;
            }
        } else {
            char digest[65] = {0};
            if (!strcmp(relative, ".skillcli.json")) continue;
            index = cli_file_index(files, count, relative);
            if (index < 0 || seen[index] || !cli_hash_file(child, digest) ||
                strcmp(digest, files[index].sha256)) {
                FindClose(find);
                return 0;
            }
            seen[index] = 1;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return GetLastError() == ERROR_NO_MORE_FILES;
}

static int cli_existing_is_safe(const wchar_t *target, const CliPlugin *plugin,
                                const CliFile *files, size_t count) {
    int seen[CLI_MAX_FILES] = {0};
    size_t index;
    DWORD attributes = GetFileAttributesW(target);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !cli_receipt_matches(target, plugin) ||
        !cli_validate_tree(target, "", files, count, seen)) return 0;
    for (index = 0; index < count; ++index) {
        if (!seen[index]) return 0;
    }
    return 1;
}

static int cli_remove_tree(const wchar_t *path) {
    wchar_t pattern[MAX_PATH * 4] = {0};
    WIN32_FIND_DATAW data;
    HANDLE find;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path) != 0;
    if (wcslen(path) + 3 >= _countof(pattern)) return 0;
    wcscpy_s(pattern, _countof(pattern), path);
    wcscat_s(pattern, _countof(pattern), L"\\*");
    find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[MAX_PATH * 4] = {0};
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ||
                wcslen(path) + wcslen(data.cFileName) + 2 >= _countof(child)) {
                FindClose(find);
                return 0;
            }
            wcscpy_s(child, _countof(child), path);
            wcscat_s(child, _countof(child), L"\\");
            wcscat_s(child, _countof(child), data.cFileName);
            if (!cli_remove_tree(child)) {
                FindClose(find);
                return 0;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        if (GetLastError() != ERROR_NO_MORE_FILES) return 0;
    }
    return RemoveDirectoryW(path) != 0;
}

static int cli_make_transaction(const wchar_t *root, const wchar_t *kind,
                                wchar_t *transaction, size_t capacity) {
    unsigned int attempt;
    if (!cli_path_directory(root)) return 0;
    for (attempt = 0; attempt < 16; ++attempt) {
        if (swprintf_s(transaction, capacity, L"%ls\\.skillcli-%ls-%lu-%u", root, kind,
                       GetCurrentProcessId(), GetTickCount() + attempt) < 0) return 0;
        if (CreateDirectoryW(transaction, NULL)) return !(GetFileAttributesW(transaction) &
                                                           FILE_ATTRIBUTE_REPARSE_POINT);
        if (GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    }
    return 0;
}

static int cli_install_destination(const CliDestination *destination, const CliPlugin *plugin,
                                   const CliFile *files, size_t count, int update,
                                   const char **status) {
    wchar_t target[MAX_PATH * 4] = {0};
    wchar_t transaction[MAX_PATH * 4] = {0};
    wchar_t stage[MAX_PATH * 4] = {0};
    wchar_t backup[MAX_PATH * 4] = {0};
    DWORD attributes;
    size_t index;
    int existed;
    *status = "failed";
    if (!cli_path_directory(destination->root) ||
        !cli_target_path(destination->root, plugin, target, _countof(target))) return 0;
    attributes = GetFileAttributesW(target);
    existed = attributes != INVALID_FILE_ATTRIBUTES;
    if (existed && (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    !cli_existing_is_safe(target, plugin, files, count))) {
        *status = "refused: unexpected local files";
        return 1;
    }
    if (existed && !update) {
        *status = "already installed";
        return 1;
    }
    if (!cli_make_transaction(destination->root, L"install", transaction, _countof(transaction))) return 0;
    wcscpy_s(stage, _countof(stage), transaction);
    if (!cli_append_path(stage, _countof(stage), L"staged") || !cli_ensure_directory(stage)) {
        cli_remove_tree(transaction);
        return 0;
    }
    for (index = 0; index < count; ++index) {
        CliResponse response = {0};
        char digest[65] = {0};
        if (!cli_source_read(plugin->source, files[index].remote, &response) ||
            !cli_sha256(response.body, response.length, digest) ||
            strcmp(digest, files[index].sha256) ||
            !cli_write_staged_file(stage, &files[index], response.body, response.length)) {
            cli_response_free(&response);
            cli_remove_tree(transaction);
            *status = "failed: checksum or download";
            return 1;
        }
        cli_response_free(&response);
    }
    if (!cli_write_receipt(stage, plugin, files, count)) {
        cli_remove_tree(transaction);
        return 0;
    }
    if (existed) {
        wcscpy_s(backup, _countof(backup), transaction);
        if (!cli_append_path(backup, _countof(backup), L"backup") ||
            !MoveFileExW(target, backup, MOVEFILE_WRITE_THROUGH)) {
            cli_remove_tree(transaction);
            return 0;
        }
    }
    if (!MoveFileExW(stage, target, MOVEFILE_WRITE_THROUGH)) {
        if (existed) MoveFileExW(backup, target, MOVEFILE_WRITE_THROUGH);
        cli_remove_tree(transaction);
        return 0;
    }
    if (existed && !cli_remove_tree(backup)) {
        *status = "updated; backup retained";
        return 1;
    }
    RemoveDirectoryW(transaction);
    *status = existed ? "updated" : "installed";
    return 1;
}

static int cli_command_exists(const wchar_t *command, wchar_t *resolved, size_t capacity) {
    return SearchPathW(NULL, command, L".exe", (DWORD)capacity, resolved, NULL) != 0;
}

static int cli_run_copilot(const wchar_t *arguments) {
    wchar_t executable[MAX_PATH * 4] = {0};
    wchar_t command[4096] = {0};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    DWORD exit_code = 1;
    if (!cli_command_exists(L"copilot", executable, _countof(executable)) ||
        swprintf_s(command, _countof(command), L"\"%ls\" %ls", executable, arguments) < 0) return 0;
    startup.cb = sizeof(startup);
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) return 0;
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

static int cli_run_copilot_capture(const wchar_t *arguments, char *output, size_t capacity) {
    wchar_t executable[MAX_PATH * 4] = {0};
    wchar_t command[4096] = {0};
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    DWORD read = 0;
    DWORD exit_code = 1;
    size_t used = 0;
    int success = 0;
    output[0] = '\0';
    if (!cli_command_exists(L"copilot", executable, _countof(executable)) ||
        swprintf_s(command, _countof(command), L"\"%ls\" %ls", executable, arguments) < 0 ||
        !CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) goto done;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    if (!CreateProcessW(executable, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process)) goto done;
    CloseHandle(write_pipe);
    write_pipe = NULL;
    while (used + 1 < capacity && ReadFile(read_pipe, output + used,
                                            (DWORD)(capacity - used - 1), &read, NULL) && read) {
        used += read;
    }
    output[used] = '\0';
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    success = exit_code == 0;
done:
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (write_pipe) CloseHandle(write_pipe);
    if (read_pipe) CloseHandle(read_pipe);
    return success;
}

static int cli_output_has_name(const char *output, const char *name) {
    const char *current;
    size_t length = strlen(name);
    for (current = output; *current; ++current) {
        if ((current == output || !isalnum((unsigned char)current[-1])) &&
            !_strnicmp(current, name, length) &&
            !isalnum((unsigned char)current[length]) && current[length] != '-' &&
            current[length] != '_') return 1;
    }
    return 0;
}

static int cli_native_available_for(const CliPlugin *plugin) {
    wchar_t executable[MAX_PATH * 4] = {0};
    return !plugin->source->path[0] && cli_command_exists(L"copilot", executable, _countof(executable));
}

static const char *cli_native_install(const CliPlugin *plugin, int update) {
    wchar_t arguments[1024] = {0};
    char output[65536] = {0};
    if (!cli_native_available_for(plugin)) return NULL;
    swprintf_s(arguments, _countof(arguments), L"plugin marketplace add %S", plugin->source->repository);
    cli_run_copilot(arguments);
    if (update) {
        swprintf_s(arguments, _countof(arguments), L"plugin update %S", plugin->name);
        return cli_run_copilot(arguments) ? "updated" : "failed";
    }
    if (cli_run_copilot_capture(L"plugin list", output, _countof(output)) &&
        cli_output_has_name(output, plugin->name)) return "already installed";
    swprintf_s(arguments, _countof(arguments), L"plugin install %S@%S",
               plugin->name, plugin->marketplace);
    return cli_run_copilot(arguments) ? "installed" : "failed";
}

static const char *cli_native_remove(const CliPlugin *plugin) {
    wchar_t arguments[512] = {0};
    char output[65536] = {0};
    if (!cli_native_available_for(plugin)) return NULL;
    if (!cli_run_copilot_capture(L"plugin list", output, _countof(output)) ||
        !cli_output_has_name(output, plugin->name)) return "not installed";
    swprintf_s(arguments, _countof(arguments), L"plugin uninstall %S", plugin->name);
    return cli_run_copilot(arguments) ? "removed" : "failed";
}

static size_t cli_destinations(const CliPlugin *plugin, CliDestination *destinations,
                               size_t capacity) {
    wchar_t home[MAX_PATH * 4] = {0};
    wchar_t one_drive[MAX_PATH * 4] = {0};
    DWORD length;
    size_t count = 0;
    int native = cli_native_available_for(plugin);
    if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, home) != S_OK) return 0;
    if (!native && count < capacity) {
        destinations[count].name = L"copilot-cli-filesystem";
        wcscpy_s(destinations[count].root, _countof(destinations[count].root), home);
        cli_append_path(destinations[count].root, _countof(destinations[count].root), L".copilot\\skills");
        if (cli_path_directory(destinations[count].root)) ++count;
    }
    if (count < capacity) {
        destinations[count].name = L"scout";
        wcscpy_s(destinations[count].root, _countof(destinations[count].root), home);
        cli_append_path(destinations[count].root, _countof(destinations[count].root), L".scout\\m-skills");
        if (cli_path_directory(destinations[count].root)) ++count;
    }
    length = GetEnvironmentVariableW(L"OneDriveCommercial", one_drive, _countof(one_drive));
    if (!length || length >= _countof(one_drive)) {
        length = GetEnvironmentVariableW(L"OneDrive", one_drive, _countof(one_drive));
    }
    if (length && length < _countof(one_drive) && count < capacity) {
        destinations[count].name = L"copilot-cowork";
        cli_append_path(one_drive, _countof(one_drive), L"Documents\\Cowork\\Skills");
        wcscpy_s(destinations[count].root, _countof(destinations[count].root), one_drive);
        if (cli_path_directory(destinations[count].root)) ++count;
    }
    return count;
}

static int cli_remove_destination(const CliDestination *destination, const CliPlugin *plugin,
                                  const char **status) {
    wchar_t target[MAX_PATH * 4] = {0};
    wchar_t transaction[MAX_PATH * 4] = {0};
    wchar_t removed[MAX_PATH * 4] = {0};
    DWORD attributes;
    *status = "failed";
    if (!cli_path_directory(destination->root) ||
        !cli_target_path(destination->root, plugin, target, _countof(target))) return 0;
    attributes = GetFileAttributesW(target);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        *status = "not installed";
        return 1;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !cli_receipt_matches(target, plugin)) {
        *status = "refused: namespace mismatch";
        return 1;
    }
    if (!cli_make_transaction(destination->root, L"remove", transaction, _countof(transaction))) return 0;
    wcscpy_s(removed, _countof(removed), transaction);
    if (!cli_append_path(removed, _countof(removed), L"removed") ||
        !MoveFileExW(target, removed, MOVEFILE_WRITE_THROUGH)) {
        RemoveDirectoryW(transaction);
        return 0;
    }
    if (!cli_remove_tree(removed)) {
        MoveFileExW(removed, target, MOVEFILE_WRITE_THROUGH);
        RemoveDirectoryW(transaction);
        return 0;
    }
    RemoveDirectoryW(transaction);
    *status = "removed";
    return 1;
}

static int cli_add_installed_id(char values[][512], size_t *count, const char *value) {
    size_t index;
    if (!cli_qualified_id_valid(value)) return 0;
    for (index = 0; index < *count; ++index) {
        if (!_stricmp(values[index], value)) return 1;
    }
    if (*count >= CLI_MAX_PLUGINS) return 0;
    strcpy_s(values[(*count)++], _countof(values[0]), value);
    return 1;
}

static void cli_collect_receipts(const CliDestination *destination, char values[][512], size_t *count) {
    wchar_t pattern[MAX_PATH * 4] = {0};
    WIN32_FIND_DATAW data;
    HANDLE find;
    if (!cli_path_directory(destination->root) ||
        wcslen(destination->root) + 3 >= _countof(pattern)) return;
    wcscpy_s(pattern, _countof(pattern), destination->root);
    wcscat_s(pattern, _countof(pattern), L"\\*");
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t receipt[MAX_PATH * 4] = {0};
        char *contents = NULL;
        size_t length = 0;
        char qualified[512] = {0};
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..") ||
            !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            wcslen(destination->root) + wcslen(data.cFileName) + wcslen(L"\\.skillcli.json") + 2 >=
                _countof(receipt)) continue;
        wcscpy_s(receipt, _countof(receipt), destination->root);
        wcscat_s(receipt, _countof(receipt), L"\\");
        wcscat_s(receipt, _countof(receipt), data.cFileName);
        wcscat_s(receipt, _countof(receipt), L"\\.skillcli.json");
        if (cli_read_local_file(receipt, &contents, &length) &&
            cli_string_field(contents, "qualifiedId", qualified, _countof(qualified))) {
            cli_add_installed_id(values, count, qualified);
        }
        free(contents);
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

static void cli_collect_native_plugins(const CliCatalogues *catalogues, char values[][512], size_t *count) {
    char output[65536] = {0};
    size_t index;
    if (!cli_run_copilot_capture(L"plugin list", output, _countof(output))) return;
    for (index = 0; index < catalogues->count; ++index) {
        const CliPlugin *plugin = &catalogues->plugins[index];
        char qualified[512] = {0};
        if (cli_native_available_for(plugin) && cli_output_has_name(output, plugin->name) &&
            cli_plugin_qualified_id(plugin, qualified, _countof(qualified))) {
            cli_add_installed_id(values, count, qualified);
        }
    }
}

static int cli_install_or_update(CliPlugin *plugin, int update) {
    CliFile files[CLI_MAX_FILES];
    CliDestination destinations[3];
    size_t file_count = 0;
    size_t destination_count;
    size_t index;
    int rows = 0;
    const char *native_status;
    if (!cli_metadata_is_approved(plugin->metadata)) {
        fwprintf(stderr, L"skillcli error: plugin is not approved.\n");
        return 0;
    }
    if (!cli_plugin_files(plugin, files, &file_count)) {
        fwprintf(stderr, L"skillcli error: plugin has unsafe or incomplete declared skill files.\n");
        return 0;
    }
    if (!cli_validate_declared_files(plugin)) {
        fwprintf(stderr, L"skillcli error: a declared plugin file failed checksum validation.\n");
        return 0;
    }
    native_status = cli_native_install(plugin, update);
    if (native_status) {
        wprintf(L"copilot-cli-native  %S  %S@%S\n", native_status, plugin->name, plugin->marketplace);
        ++rows;
    }
    destination_count = cli_destinations(plugin, destinations, _countof(destinations));
    for (index = 0; index < destination_count; ++index) {
        const char *status;
        if (!cli_install_destination(&destinations[index], plugin, files, file_count, update, &status)) {
            wprintf(L"%ls  failed  %ls\n", destinations[index].name, destinations[index].root);
            return 0;
        }
        wprintf(L"%ls  %S  %ls\n", destinations[index].name, status, destinations[index].root);
        ++rows;
    }
    if (!rows) {
        fwprintf(stderr, L"skillcli error: no supported host was detected.\n");
        return 0;
    }
    return 1;
}

static int cli_remove_plugin(CliPlugin *plugin) {
    CliDestination destinations[3];
    size_t destination_count;
    size_t index;
    int rows = 0;
    const char *native_status = cli_native_remove(plugin);
    if (native_status) {
        wprintf(L"copilot-cli-native  %S  %S\n", native_status, plugin->name);
        ++rows;
    }
    destination_count = cli_destinations(plugin, destinations, _countof(destinations));
    for (index = 0; index < destination_count; ++index) {
        const char *status;
        if (!cli_remove_destination(&destinations[index], plugin, &status)) {
            wprintf(L"%ls  failed  %ls\n", destinations[index].name, destinations[index].root);
            return 0;
        }
        wprintf(L"%ls  %S  %ls\n", destinations[index].name, status, destinations[index].root);
        ++rows;
    }
    if (!rows) {
        fwprintf(stderr, L"skillcli error: no supported host was detected.\n");
        return 0;
    }
    return 1;
}

static int cli_register_source(const wchar_t *configuration, const wchar_t *argument) {
    char specification[768] = {0};
    char repository[256] = {0};
    char path[512] = {0};
    CliSources sources;
    CliSource candidate;
    CliResponse catalogue = {0};
    CliResponse marketplace = {0};
    const char *library;
    char namespace_value[256] = {0};
    char marketplace_name[128] = {0};
    wchar_t native_arguments[512] = {0};
    if (!cli_wide_to_utf8(argument, specification, _countof(specification)) ||
        !cli_repository_parts(specification, repository, _countof(repository), path, _countof(path)) ||
        !cli_load_sources(configuration, &sources)) {
        fwprintf(stderr, L"skillcli error: register requires OWNER/REPO[/sub/path].\n");
        return 0;
    }
    ZeroMemory(&candidate, sizeof(candidate));
    strcpy_s(candidate.repository, _countof(candidate.repository), repository);
    strcpy_s(candidate.path, _countof(candidate.path), path);
    strcpy_s(candidate.ref, _countof(candidate.ref), "main");
    cli_source_id(&candidate, candidate.id, _countof(candidate.id));
    if (!cli_source_read(&candidate, "catalogue.json", &catalogue) ||
        !cli_source_read(&candidate, ".github/plugin/marketplace.json", &marketplace) ||
        !cli_object_value(catalogue.body, "library", &library) ||
        !cli_string_field(library, "namespace", namespace_value, _countof(namespace_value)) ||
        strcmp(namespace_value, candidate.repository) ||
        !cli_string_field(marketplace.body, "name", marketplace_name, _countof(marketplace_name)) ||
        !cli_safe_component(marketplace_name)) {
        cli_response_free(&catalogue);
        cli_response_free(&marketplace);
        fwprintf(stderr, L"skillcli error: source has no valid namespace-bound marketplace.\n");
        return 0;
    }
    cli_response_free(&catalogue);
    cli_response_free(&marketplace);
    if (!cli_sources_contains(&sources, candidate.repository, candidate.path) &&
        !cli_sources_add(&sources, &candidate)) {
        fwprintf(stderr, L"skillcli error: source configuration is full or invalid.\n");
        return 0;
    }
    if (!cli_write_sources(configuration, &sources)) {
        fwprintf(stderr, L"skillcli error: could not safely write sources.json.\n");
        return 0;
    }
    wprintf(L"%S  %S  %ls\n", candidate.repository,
            candidate.path[0] ? candidate.path : "(repository root)",
            cli_sources_contains(&sources, candidate.repository, candidate.path) ? L"registered" : L"registered");
    if (!candidate.path[0] && cli_command_exists(L"copilot", native_arguments, _countof(native_arguments))) {
        swprintf_s(native_arguments, _countof(native_arguments), L"plugin marketplace add %S",
                   candidate.repository);
        if (cli_run_copilot(native_arguments)) wprintf(L"copilot marketplace: registered\n");
    } else if (candidate.path[0]) {
        wprintf(L"copilot marketplace: filesystem adapter will use configured subpath\n");
    }
    return 1;
}

static int cli_safe_update_stage(const wchar_t *path, const wchar_t *directory) {
    const wchar_t *name;
    size_t directory_length = wcslen(directory);
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        _wcsnicmp(path, directory, directory_length) ||
        (path[directory_length] != L'\\' && path[directory_length] != L'/')) return 0;
    name = path + directory_length + 1;
    return !_wcsnicmp(name, L"skillcli.update.", 16) &&
           wcslen(name) > 20 && !_wcsicmp(name + wcslen(name) - 4, L".exe");
}

static int cli_safe_installed_executable(const wchar_t *path, const wchar_t *directory) {
    wchar_t expected[MAX_PATH * 4] = {0};
    DWORD attributes;
    if (wcslen(directory) + wcslen(L"\\skillcli.exe") + 1 >= _countof(expected)) return 0;
    wcscpy_s(expected, _countof(expected), directory);
    wcscat_s(expected, _countof(expected), L"\\skillcli.exe");
    attributes = GetFileAttributesW(path);
    return !_wcsicmp(path, expected) && attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int cli_launch_self(const wchar_t *executable, const wchar_t *arguments) {
    wchar_t command[MAX_PATH * 8] = {0};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    if (swprintf_s(command, _countof(command), L"\"%ls\" %ls", executable, arguments) < 0) return 0;
    startup.cb = sizeof(startup);
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) return 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

static int cli_apply_self_update(int argc, wchar_t **argv) {
    wchar_t stage[MAX_PATH * 4] = {0};
    wchar_t directory[MAX_PATH * 4] = {0};
    wchar_t temporary[MAX_PATH * 4] = {0};
    wchar_t arguments[MAX_PATH * 8] = {0};
    wchar_t self[MAX_PATH * 4] = {0};
    wchar_t *end;
    DWORD old_pid;
    HANDLE old_process;
    if (argc != 4 || !cli_module_directory(directory, _countof(directory)) ||
        !cli_path_directory(directory) ||
        !GetModuleFileNameW(NULL, self, _countof(self)) ||
        !cli_safe_update_stage(self, directory) ||
        !cli_safe_installed_executable(argv[2], directory) ||
        wcslen(argv[2]) >= _countof(stage)) return 0;
    wcscpy_s(stage, _countof(stage), self);
    end = NULL;
    old_pid = wcstoul(argv[3], &end, 10);
    if (!old_pid || !end || *end) return 0;
    old_process = OpenProcess(SYNCHRONIZE, FALSE, old_pid);
    if (old_process) {
        WaitForSingleObject(old_process, INFINITE);
        CloseHandle(old_process);
    }
    if (swprintf_s(temporary, _countof(temporary), L"%ls.new", argv[2]) < 0 ||
        GetFileAttributesW(temporary) != INVALID_FILE_ATTRIBUTES ||
        !CopyFileW(stage, temporary, TRUE) ||
        !MoveFileExW(temporary, argv[2], MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        return 0;
    }
    if (swprintf_s(arguments, _countof(arguments), L"--cleanup-self-update \"%ls\" %lu",
                   stage, GetCurrentProcessId()) < 0 ||
        !cli_launch_self(argv[2], arguments)) return 0;
    return 1;
}

static int cli_cleanup_self_update(int argc, wchar_t **argv) {
    wchar_t directory[MAX_PATH * 4] = {0};
    wchar_t *end;
    DWORD updater_pid;
    HANDLE updater;
    if (argc != 4 || !cli_module_directory(directory, _countof(directory)) ||
        !cli_path_directory(directory) ||
        !cli_safe_update_stage(argv[2], directory)) return 0;
    updater_pid = wcstoul(argv[3], &end, 10);
    if (!updater_pid || !end || *end) return 0;
    updater = OpenProcess(SYNCHRONIZE, FALSE, updater_pid);
    if (updater) {
        WaitForSingleObject(updater, INFINITE);
        CloseHandle(updater);
    }
    if (!DeleteFileW(argv[2]) && GetLastError() != ERROR_FILE_NOT_FOUND) return 0;
    wprintf(L"skillcli self-update completed.\n");
    return 1;
}

static int cli_self_update(CliCatalogues *catalogues) {
    CliPlugin *browser = NULL;
    CliResponse response = {0};
    wchar_t executable[MAX_PATH * 4] = {0};
    wchar_t directory[MAX_PATH * 4] = {0};
    wchar_t stage[MAX_PATH * 4] = {0};
    wchar_t arguments[MAX_PATH * 8] = {0};
    HANDLE file;
    DWORD written = 0;
    size_t index;
    for (index = 0; index < catalogues->count; ++index) {
        CliPlugin *plugin = &catalogues->plugins[index];
        if (!_stricmp(plugin->source->repository, "WillEastbury/skillcli") &&
            !strcmp(plugin->name, "skillcli-skill-zero")) {
            browser = plugin;
            break;
        }
    }
    if (!browser || !cli_metadata_is_approved(browser->metadata) ||
        !cli_validate_declared_files(browser) ||
        !GetModuleFileNameW(NULL, executable, _countof(executable)) ||
        !cli_module_directory(directory, _countof(directory)) ||
        !cli_path_directory(directory) ||
        (GetFileAttributesW(executable) & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !cli_source_read(browser->source, "installer/skillcli.exe", &response) ||
        response.length < 2 || response.body[0] != 'M' || response.body[1] != 'Z' ||
        swprintf_s(stage, _countof(stage), L"%ls\\skillcli.update.%lu.exe",
                   directory, GetCurrentProcessId()) < 0 ||
        GetFileAttributesW(stage) != INVALID_FILE_ATTRIBUTES) {
        cli_response_free(&response);
        fwprintf(stderr, L"skillcli error: no verified native update binary is available.\n");
        return 0;
    }
    file = CreateFileW(stage, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, response.body, (DWORD)response.length, &written, NULL) ||
        written != response.length) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(stage);
        cli_response_free(&response);
        fwprintf(stderr, L"skillcli error: could not stage the native update.\n");
        return 0;
    }
    CloseHandle(file);
    cli_response_free(&response);
    if (swprintf_s(arguments, _countof(arguments), L"--apply-self-update \"%ls\" %lu",
                   executable, GetCurrentProcessId()) < 0 || !cli_launch_self(stage, arguments)) {
        DeleteFileW(stage);
        fwprintf(stderr, L"skillcli error: could not start the native updater.\n");
        return 0;
    }
    wprintf(L"skillcli self-update is applying from %S at %S.\n",
            browser->source->repository, browser->source->commit);
    return 1;
}

static void cli_usage(void) {
    wprintf(L"Usage:\n"
            L"  skillcli search --role <role> --query \"<need>\"\n"
            L"  skillcli install --skill <owner>/<repo>/<plugin-name>\n"
            L"  skillcli remove --skill <owner>/<repo>/<plugin-name>\n"
            L"  skillcli update --skill <owner>/<repo>/<plugin-name>\n"
            L"  skillcli update --all\n"
            L"  skillcli register <owner>/<repo>[/sub/path]\n"
            L"  skillcli self-update\n");
}

static int cli_search(CliCatalogues *catalogues, const wchar_t *role_argument,
                      const wchar_t *query_argument) {
    char role[128] = {0};
    char query[1024] = {0};
    size_t selected[CLI_MAX_PLUGINS];
    int scores[CLI_MAX_PLUGINS];
    char why[CLI_MAX_PLUGINS][512];
    size_t count = 0;
    size_t index;
    if (!cli_wide_to_utf8(role_argument, role, _countof(role)) ||
        !cli_wide_to_utf8(query_argument, query, _countof(query)) || !cli_safe_component(role)) return 0;
    for (index = 0; index < catalogues->count; ++index) {
        int score = cli_plugin_score(&catalogues->plugins[index], role, query, why[count], _countof(why[0]));
        if (score < 0) continue;
        selected[count] = index;
        scores[count] = score;
        ++count;
    }
    for (index = 0; index < count; ++index) {
        size_t other;
        for (other = index + 1; other < count; ++other) {
            char left[512] = {0};
            char right[512] = {0};
            cli_plugin_qualified_id(&catalogues->plugins[selected[index]], left, _countof(left));
            cli_plugin_qualified_id(&catalogues->plugins[selected[other]], right, _countof(right));
            if (scores[other] > scores[index] ||
                (scores[other] == scores[index] && _stricmp(right, left) < 0)) {
                size_t selected_swap = selected[index];
                int score_swap = scores[index];
                char why_swap[512] = {0};
                selected[index] = selected[other];
                selected[other] = selected_swap;
                scores[index] = scores[other];
                scores[other] = score_swap;
                strcpy_s(why_swap, _countof(why_swap), why[index]);
                strcpy_s(why[index], _countof(why[index]), why[other]);
                strcpy_s(why[other], _countof(why[other]), why_swap);
            }
        }
    }
    wprintf(L"#  Plugin ID  Version  Source  Why\n");
    for (index = 0; index < count && index < 10; ++index) {
        CliPlugin *plugin = &catalogues->plugins[selected[index]];
        char qualified[512] = {0};
        cli_plugin_qualified_id(plugin, qualified, _countof(qualified));
        wprintf(L"%u  %S  %S  %S  %S\n", (unsigned)(index + 1), qualified, plugin->version,
                plugin->source->id, why[index]);
    }
    return 1;
}

static int cli_update_all(CliCatalogues *catalogues) {
    CliSource filesystem_source;
    CliPlugin filesystem_plugin;
    CliDestination destinations[3];
    char identifiers[CLI_MAX_PLUGINS][512] = {{0}};
    size_t count = 0;
    size_t destination_count;
    size_t index;
    int success = 1;
    ZeroMemory(&filesystem_source, sizeof(filesystem_source));
    ZeroMemory(&filesystem_plugin, sizeof(filesystem_plugin));
    strcpy_s(filesystem_source.path, _countof(filesystem_source.path), "filesystem");
    filesystem_plugin.source = &filesystem_source;
    destination_count = cli_destinations(&filesystem_plugin, destinations, _countof(destinations));
    for (index = 0; index < destination_count; ++index) {
        cli_collect_receipts(&destinations[index], identifiers, &count);
    }
    cli_collect_native_plugins(catalogues, identifiers, &count);
    if (!count) {
        fwprintf(stderr, L"skillcli error: no installed plugins were found.\n");
        return 0;
    }
    for (index = 0; index < count; ++index) {
        CliPlugin *plugin = cli_find_plugin(catalogues, identifiers[index]);
        if (!plugin) {
            fwprintf(stderr, L"warning: %S is no longer available from its registered source.\n",
                     identifiers[index]);
            success = 0;
            continue;
        }
        wprintf(L"[%S]\n", identifiers[index]);
        if (!cli_install_or_update(plugin, 1)) success = 0;
    }
    return success;
}

int marketplace_main(int argc, wchar_t **argv) {
    wchar_t directory[MAX_PATH * 4] = {0};
    wchar_t configuration[MAX_PATH * 4] = {0};
    CliCatalogues *catalogues;
    CliPlugin *plugin;
    char qualified[512] = {0};
    int result = 0;
    if (argc >= 2 && !wcscmp(argv[1], L"--apply-self-update")) return cli_apply_self_update(argc, argv) ? 0 : 1;
    if (argc >= 2 && !wcscmp(argv[1], L"--cleanup-self-update")) return cli_cleanup_self_update(argc, argv) ? 0 : 1;
    if (!cli_module_directory(directory, _countof(directory)) ||
        !cli_path_directory(directory) ||
        !cli_append_path(directory, _countof(directory), L"sources.json")) {
        fwprintf(stderr, L"skillcli error: could not locate sources.json.\n");
        return 1;
    }
    wcscpy_s(configuration, _countof(configuration), directory);
    if (argc == 3 && !wcscmp(argv[1], L"register")) {
        return cli_register_source(configuration, argv[2]) ? 0 : 1;
    }
    catalogues = calloc(1, sizeof(*catalogues));
    if (!catalogues) {
        fwprintf(stderr, L"skillcli error: insufficient memory.\n");
        return 1;
    }
    if (argc == 2 && !wcscmp(argv[1], L"self-update")) {
        if (!cli_load_catalogues(configuration, catalogues)) {
            free(catalogues);
            return 1;
        }
        result = cli_self_update(catalogues);
        cli_catalogues_free(catalogues);
        free(catalogues);
        return result ? 0 : 1;
    }
    if (!cli_load_catalogues(configuration, catalogues)) {
        free(catalogues);
        return 1;
    }
    if (argc == 6 && !wcscmp(argv[1], L"search") &&
        !wcscmp(argv[2], L"--role") && !wcscmp(argv[4], L"--query")) {
        result = cli_search(catalogues, argv[3], argv[5]);
    } else if (argc == 4 && !wcscmp(argv[1], L"install") && !wcscmp(argv[2], L"--skill") &&
               cli_wide_to_utf8(argv[3], qualified, _countof(qualified)) &&
               cli_qualified_id_valid(qualified) &&
               (plugin = cli_find_plugin(catalogues, qualified)) != NULL) {
        result = cli_install_or_update(plugin, 0);
    } else if (argc == 4 && !wcscmp(argv[1], L"remove") && !wcscmp(argv[2], L"--skill") &&
               cli_wide_to_utf8(argv[3], qualified, _countof(qualified)) &&
               cli_qualified_id_valid(qualified) &&
               (plugin = cli_find_plugin(catalogues, qualified)) != NULL) {
        result = cli_remove_plugin(plugin);
    } else if (argc == 3 && !wcscmp(argv[1], L"update") && !wcscmp(argv[2], L"--all")) {
        result = cli_update_all(catalogues);
    } else if (argc == 4 && !wcscmp(argv[1], L"update") && !wcscmp(argv[2], L"--skill") &&
               cli_wide_to_utf8(argv[3], qualified, _countof(qualified)) &&
               cli_qualified_id_valid(qualified) &&
               (plugin = cli_find_plugin(catalogues, qualified)) != NULL) {
        result = cli_install_or_update(plugin, 1);
    } else {
        if (argc >= 2 && ( !wcscmp(argv[1], L"install") || !wcscmp(argv[1], L"remove") ||
                          !wcscmp(argv[1], L"update"))) {
            fwprintf(stderr, L"skillcli error: unknown or invalid qualified plugin ID.\n");
        }
        cli_usage();
    }
    cli_catalogues_free(catalogues);
    free(catalogues);
    return result ? 0 : 1;
}
