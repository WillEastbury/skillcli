# Native Windows installer

`skillcli-setup.exe` is a native Win32 C executable. It has no .NET or Python
or PowerShell runtime dependency on the target machine. The EXE embeds:

- Skill Zero
- Skill One
- Skill Two

Public HTTPS requests use the Windows WinHTTP system API and the narrow JSON reader is
compiled directly into the EXE. Private and GitHub Enterprise requests use the
already-authenticated host-managed `gh api` command; the EXE never requests or handles
tokens. It checks `gh auth status --hostname <host>` and, at an interactive console,
offers to install missing `gh` with `winget`, or runs `gh auth login --hostname <host>`
when authentication is absent, before retrying. No libcurl, JSON library, third-party
DLL, or runtime is shipped or installed.

When run with no arguments, it requests UAC elevation, copies itself to
`%USERPROFILE%\skillcli\skillcli.exe`, and adds `%USERPROFILE%\skillcli` to the
user PATH. It opens a console wizard that detects supported harnesses, offers
missing host applications, installs the three embedded core skills into existing
skills folders, registers additional source repositories, and verifies deployment.
Before requesting UAC elevation, it checks whether `%USERPROFILE%\skillcli` is already
on the user PATH. If not, it explains the PATH addition and requires confirmation;
declining exits without elevation or PATH changes.
The host selector uses Up/Down, Space, and Enter with checkbox rows for GitHub Copilot
CLI, Microsoft Scout, and Copilot Co-Work. Detected hosts are pre-checked and receive
core-skill updates automatically; selecting a missing row starts its host acquisition
flow.
The final status view prints the installed `skillcli.exe` path and the core-skill
deployment result for every harness.
Source registration accepts `OWNER/REPO` and `OWNER/REPO/sub/path`. A configured
subpath prefixes every catalogue, marketplace, plugin metadata, and declared plugin
file request while preserving `OWNER/REPO/plugin-name` qualified IDs.
Private or Enterprise entries can set `host`; `gh auth login --hostname <host>` must
already have an account authorised for that repository.

Before deploying the embedded core skills, a no-argument install or upgrade verifies
the copied EXE and cleans only recognised legacy Python/PowerShell installer files
and namespace-bound legacy skill folders. It rejects reparse points and leaves
unrecognised files, `sources.json`, the native EXE, and the current core-skill
folders intact.

When GitHub Copilot CLI is not detected, setup offers to install it through
`winget`. When Scout is not detected, setup opens its official download page at
`https://aka.ms/scout-release`. Scout and Co-Work are never created by setup:
their skills are copied only when their target folders already exist. When the
GitHub Copilot CLI is present, setup creates its skills folder if necessary before
deploying the embedded core skills.

Build:

```text
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd installer
rc native\setup.rc
cl /nologo /O2 /W4 /DUNICODE /D_UNICODE native\setup.c native\marketplace.c native\setup.res /link /SUBSYSTEM:CONSOLE /OUT:skillcli.exe shell32.lib advapi32.lib user32.lib winhttp.lib bcrypt.lib
```

The resulting executable is `installer\skillcli.exe`. Building requires
Visual Studio C++ Build Tools. No script interpreter is used by the installer.

`skillcli --uninstall` removes the user PATH entry and schedules
`%USERPROFILE%\skillcli\skillcli.exe` for deletion at the next reboot. `skillcli --clean`
also removes `sources.json` and only resource-verified managed core-skill directories;
it rejects reparse points and leaves modified or unexpected content untouched.
