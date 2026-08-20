# Native Windows installer

`skillcli-setup.exe` is a native Win32 C executable. It has no .NET or Python
or PowerShell runtime dependency on the target machine. The EXE embeds:

- Skill Zero
- Skill One
- Skill Two

HTTPS requests use the Windows WinHTTP system API and the narrow JSON reader is
compiled directly into the EXE. No libcurl, JSON library, third-party DLL, or
runtime is shipped or installed.

When run with no arguments, it requests UAC elevation, copies itself to
`%USERPROFILE%\skillcli\skillcli.exe`, and adds `%USERPROFILE%\skillcli` to the
user PATH. It opens a console wizard that detects supported harnesses, offers
missing host applications, installs the three embedded core skills into existing
skills folders, registers additional source repositories, and verifies deployment.
Source registration accepts `OWNER/REPO` and `OWNER/REPO/sub/path`.

When GitHub Copilot CLI is not detected, setup offers to install it through
`winget`. When Scout is not detected, setup opens its official download page at
`https://aka.ms/scout-release`. Scout and Co-Work are never created by setup:
their skills are copied only when their target folders already exist.

Build:

```text
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd installer
rc native\setup.rc
cl /nologo /O2 /W4 /DUNICODE /D_UNICODE native\setup.c setup.res /link /SUBSYSTEM:CONSOLE /OUT:skillcli-setup.exe shell32.lib advapi32.lib winhttp.lib
```

The resulting executable is `installer\skillcli-setup.exe`. Building requires
Visual Studio C++ Build Tools. No script interpreter is used by the installer.
