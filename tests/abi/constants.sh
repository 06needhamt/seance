#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Prints a C(name) line for every object-like macro defined in the given
# headers, for gen_abi_checks to include where C is defined, so it can
# compare each one's value with the Windows headers. Each is under #ifdef:
# some are defined for one architecture only. Include guards,
# calling-convention attributes and the names in constants_skip.txt
# (harness-only) are left out, as are the harness's own VFW_ names (but not
# DirectShow's VFW_E_/VFW_S_ HRESULTs).
set -e
skip=$(grep -v '^#' "$(dirname "$0")/constants_skip.txt" | tr '\n' ' ')

sed -n 's/^#define \([A-Z][A-Z0-9_]*\)[ 	]\{1,\}\([^ 	].*\)$/\1 \2/p' "$@" |
	grep -v '__attribute__' |
	awk -v skip="$skip" '
		BEGIN { n = split(skip, s, " "); for (i = 1; i <= n; i++) drop[s[i]] = 1 }
		($1 !~ /^VFW_/ || $1 ~ /^VFW_[ES]_/) && !($1 in drop) { print $1 }' |
	sort -u |
	awk '{ printf "#ifdef %s\nC(%s)\n#endif\n", $1, $1 }'
