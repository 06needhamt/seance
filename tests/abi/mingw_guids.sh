#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Prints ABI_GUID(name, Data1, Data2, Data3, Data4 bytes...) for every GUID
# the mingw-w64 COM and DirectShow headers define, for check_abi.c to
# compare ours with. Their DEFINE_GUID is redefined each time guiddef.h is
# included, so it can't be redefined to capture the values; instead the
# headers are preprocessed with INITGUID, which turns each GUID into an
# initialised definition, and the initialisers are read back.
#
#   mingw_guids.sh <mingw-gcc>
set -e
cc=${1:?usage: mingw_guids.sh <mingw-gcc>}

# icodecapi.h conflicts with strmif.h, so it is read on its own.
guids() {
	printf '#define INITGUID\n#include <windows.h>\n'
	for h in "$@"; do
		printf '#include <%s>\n' "$h"
	done
}
for set in "dshow.h dvdmedia.h" "icodecapi.h"; do
	# shellcheck disable=SC2086
	guids $set | "$cc" -std=c11 -E -P -x c - |
		tr '\n' ' ' | tr ';' '\n' |
		sed -n 's/^.*[ )]GUID *\(__attribute__((selectany))\)\{0,1\} *\([A-Za-z_][A-Za-z0-9_]*\) *= *{\([^{}]*\), *{\([^{}]*\)} *} *$/ABI_GUID(\2, \3, \4)/p'
done

# codecapi.h defines its GUIDs as uuid-tagged structs, which INITGUID doesn't
# turn into values, but each also has a STATIC_ macro holding them.
printf '#include <windows.h>\n#include <codecapi.h>\n' | "$cc" -std=c11 -E -dD -x c - |
	sed -n 's/^#define STATIC_\(CODECAPI_[A-Za-z0-9_]*\) *\(0x[0-9a-fA-F]*, *0x[0-9a-fA-F]*, *0x[0-9a-fA-F]*\), *\(0x.*\)$/ABI_GUID(\1, \2, \3)/p'
