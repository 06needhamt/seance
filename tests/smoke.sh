#!/bin/sh
# A quick run of every CLI command: the usage paths, failures on files that
# aren't what a command wants, and, when the fixture DLLs were built, info
# and probe on them.
#
# usage: tests/smoke.sh <vfw-harness> <not a DLL or AVI> [<fixture dir>]
set -eu

harness=${1:?usage: $0 <vfw-harness> <file> [<fixture dir>]}
junk=${2:?usage: $0 <vfw-harness> <file> [<fixture dir>]}
fixtures=${3:-}

fails() {
	if "$@" >/dev/null 2>&1; then
		echo "smoke: '$*' should have failed" >&2
		exit 1
	fi
}

"$harness" --version
"$harness" --help >/dev/null
fails "$harness" no-such-command
fails "$harness" info "$junk"
fails "$harness" probe "$junk"
VFW_INI=/nonexistent "$harness" list >/dev/null
fails "$harness" avi "$junk"
fails "$harness" convert "$junk" -o /dev/null --format raw
fails "$harness" decode
fails "$harness" encode
fails "$harness" decode TST0 "$junk" -o /dev/null --format raw
fails "$harness" encode TST0 "$junk" -o /dev/null

if [ -n "$fixtures" ] && [ -f "$fixtures/testcodec.dll" ]; then
	"$harness" info "$fixtures/pe_dep.dll" >/dev/null
	"$harness" probe "$fixtures/testcodec.dll" | grep -q "handler 'TST0'"
	fails "$harness" decode "$fixtures/testcodec.dll" "$junk" -o /dev/null \
		--format raw
fi
echo "smoke: CLI ok"
