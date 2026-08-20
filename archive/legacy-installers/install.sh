#!/usr/bin/env bash
set -euo pipefail

PUBLIC_REPOSITORY="WillEastbury/skillcli"
PUBLIC_REF="${SKILLCLI_PUBLIC_REF:-main}"
TOOL_DIRECTORY="${SKILLCLI_TOOL_DIRECTORY:-$HOME/.local/share/skillcli}"
BIN_DIRECTORY="${SKILLCLI_BIN_DIRECTORY:-$HOME/.local/bin}"

command -v python3 >/dev/null 2>&1 || {
  echo "Python 3.10 or newer is required." >&2
  exit 1
}
python3 -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' || {
  echo "Python 3.10 or newer is required." >&2
  exit 1
}
command -v curl >/dev/null 2>&1 || {
  echo "curl is required." >&2
  exit 1
}

public_commit="$(
  curl -fsSL \
    -H "User-Agent: skillcli-installer" \
    "https://api.github.com/repos/$PUBLIC_REPOSITORY/commits/$PUBLIC_REF" |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["sha"])'
)"

temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT

plugin_path="plugins/skillcli-skill-zero"
curl -fsSL \
  "https://raw.githubusercontent.com/$PUBLIC_REPOSITORY/$public_commit/$plugin_path/skillcli.json" \
  -o "$temporary/skillcli.json"

curl -fsSL \
  "https://raw.githubusercontent.com/$PUBLIC_REPOSITORY/$public_commit/skill-sources.json" \
  -o "$temporary/sources.json"

mkdir -p "$TOOL_DIRECTORY" "$BIN_DIRECTORY"

python3 - "$temporary/skillcli.json" <<'PY' >"$temporary/tool-files.tsv"
import json
import sys

metadata = json.load(open(sys.argv[1], encoding="utf-8"))
records = [record for record in metadata["files"] if record["target"] == "tool"]
if len(records) < 2:
    raise SystemExit("Skill Zero plugin does not declare required tool files")
for record in records:
    print(f"{record['path']}\t{record['sha256']}")
PY

while IFS=$'\t' read -r relative expected; do
  filename="$(basename "$relative")"
  case "$filename" in
    skillcli.py|skillcli_core.py) ;;
    *)
      echo "Unexpected tool file: $relative" >&2
      exit 1
      ;;
  esac
  destination="$TOOL_DIRECTORY/$filename"
  curl -fsSL \
    "https://raw.githubusercontent.com/$PUBLIC_REPOSITORY/$public_commit/$plugin_path/$relative" \
    -o "$destination"
  actual="$(python3 - "$destination" <<'PY'
import hashlib
import pathlib
import sys

content = pathlib.Path(sys.argv[1]).read_bytes()
try:
    content = content.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")
except UnicodeDecodeError:
    pass
print(hashlib.sha256(content).hexdigest())
PY
)"
  if [[ "$actual" != "$expected" ]]; then
    echo "Checksum mismatch for $relative." >&2
    exit 1
  fi
done <"$temporary/tool-files.tsv"

cp "$temporary/sources.json" "$TOOL_DIRECTORY/sources.json"

cat >"$BIN_DIRECTORY/skillcli" <<EOF
#!/usr/bin/env bash
exec python3 "$TOOL_DIRECTORY/skillcli.py" "\$@"
EOF
chmod 755 "$BIN_DIRECTORY/skillcli"

"$BIN_DIRECTORY/skillcli" update --skill WillEastbury/skillcli/skillcli-skill-zero

echo
echo "Installed: skillcli and Skill Zero"
echo "CLI commit: $public_commit"
echo "Catalogue configuration: $PUBLIC_REPOSITORY@$public_commit"
if [[ ":$PATH:" != *":$BIN_DIRECTORY:"* ]]; then
  echo "Add this directory to PATH: $BIN_DIRECTORY"
fi
echo 'Try: skillcli search --role seller --query "prompt quality"'
