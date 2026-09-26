#!/bin/sh
# Prints VFW_ABI_CONSTANTS(C), listing every object-like macro defined in
# the given headers, so gen_abi_checks can compare each one's value with
# the Windows headers. Include guards, calling-convention attributes and the
# names in constants_skip.txt (harness-only) are left out.
set -e
skip=$(grep -v '^#' "$(dirname "$0")/constants_skip.txt" | tr '\n' ' ')

echo "#define VFW_ABI_CONSTANTS(C) \\"
sed -n 's/^#define \([A-Z][A-Z0-9_]*\)[ 	]\{1,\}\([^ 	].*\)$/\1 \2/p' "$@" |
	grep -v '__attribute__' |
	awk -v skip="$skip" '
		BEGIN { n = split(skip, s, " "); for (i = 1; i <= n; i++) drop[s[i]] = 1 }
		$1 !~ /^VFW_/ && !($1 in drop) { print $1 }' |
	sort -u |
	sed 's/.*/	C(&) \\/'
echo
