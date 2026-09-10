#!/bin/sh
set -eu

source_specification=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source)
            [ "$#" -ge 2 ] || {
                echo "Frontier Agent Helper setup error: --source requires OWNER/REPO[/path]." >&2
                exit 2
            }
            source_specification=$2
            shift 2
            ;;
        *)
            echo "Usage: install-skillcli.sh --source OWNER/REPO[/path]" >&2
            exit 2
            ;;
    esac
done

old_ifs=$IFS
IFS=/
set -f
set -- $source_specification
set +f
IFS=$old_ifs
[ "$#" -ge 2 ] || {
    echo "Frontier Agent Helper setup error: source must include OWNER/REPO." >&2
    exit 2
}
for component in "$@"; do
    case "$component" in
        ""|"."|".."|*[!A-Za-z0-9_.-]*)
            echo "Frontier Agent Helper setup error: invalid private source." >&2
            exit 2
            ;;
    esac
done

if [ "preview" != "notarized" ] &&
   [ "${FRONTIER_ALLOW_UNSIGNED_MACOS_PREVIEW:-}" != "1" ]; then
    echo "Frontier Agent Helper setup error: this macOS build is an unsigned preview." >&2
    echo "Set FRONTIER_ALLOW_UNSIGNED_MACOS_PREVIEW=1 only on an approved test device." >&2
    exit 1
fi

for command_name in awk chmod curl mktemp rm shasum uname; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "Frontier Agent Helper setup error: $command_name is required." >&2
        exit 1
    }
done

case "$(uname -m)" in
    arm64)
        artifact_path="bin/darwin-arm64/skillcli"
        expected_sha256="DC9F3DE24EFF826756FDDD01A292E052588A13F1E743CE2362388B6E44377766"
        ;;
    x86_64)
        artifact_path="bin/darwin-x86_64/skillcli"
        expected_sha256="08576E9C9A46C66FBAED23644098943A95749F3D55C32DE636A3A8476CE7FAF7"
        ;;
    *)
        echo "Frontier Agent Helper setup error: unsupported macOS architecture $(uname -m)." >&2
        exit 1
        ;;
esac

download_url="https://raw.githubusercontent.com/WillEastbury/skillcli/c5305fe26299c095187b08456c76c5be0568ec30/$artifact_path"
temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/frontier-agent-helper.XXXXXX")
installer="$temporary_directory/skillcli"

cleanup() {
    rm -rf "$temporary_directory"
}
trap cleanup EXIT HUP INT TERM

curl --fail --location --silent --show-error "$download_url" --output "$installer"
actual_sha256=$(shasum -a 256 "$installer" | awk '{print toupper($1)}')
if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "Frontier Agent Helper checksum verification failed." >&2
    exit 1
fi

chmod 700 "$installer"
"$installer" setup --source "$source_specification"
