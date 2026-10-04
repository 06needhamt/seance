/* SPDX-License-Identifier: GPL-2.0-only */
/* Regression coverage for the compatibility entry points added as a group. */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "test.h"

static void *proc(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	CHECK(p != NULL);
	return p;
}

int main(void)
{
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);
	struct module *k = mod_load("kernel32.dll", NULL);
	struct module *u = mod_load("user32.dll", NULL);
	struct module *g = mod_load("gdi32.dll", NULL);
	struct module *c = mod_load("msvcrt.dll", NULL);
	struct module *m = mod_load("winmm.dll", NULL);
	CHECK(k && u && g && c && m);
	if (!k || !u || !g || !c || !m) return test_summary("test_missing_apis");

	const char *kn[] = {"AddAtomA","FindAtomA","GetAtomNameA","DeviceIoControl",
		"_hread","_hwrite","_lclose","_lcreat","_llseek",NULL};
	const char *un[] = {"GetScrollRange","GetWindowWord",NULL};
	const char *gn[] = {"CreatePatternBrush","DeleteEnhMetaFile","PlayEnhMetaFile","SetEnhMetaFileBits",NULL};
	const char *cn[] = {"_EH_prolog","__mb_cur_max","_assert","_ctype","_errno",
		"_fdopen","_filbuf","_filelengthi64","_flsbuf","_fstat","_isctype",
		"_lseek","_read","_write","fgetpos","fsetpos","localeconv","setlocale",
		"strcoll","strxfrm","ungetc","wcsncmp",NULL};
	const char *mn[] = {"waveOutClose","waveOutOpen","waveOutPause","waveOutPrepareHeader",
		"waveOutReset","waveOutRestart","waveOutUnprepareHeader","waveOutWrite",NULL};
	for (int i=0; kn[i]; i++) proc(k,kn[i]); for(int i=0;un[i];i++)proc(u,un[i]);
	for (int i=0; gn[i]; i++) proc(g,gn[i]); for(int i=0;cn[i];i++)proc(c,cn[i]);
	for (int i=0; mn[i]; i++) proc(m,mn[i]);

	ATOM (WINAPI *add)(LPCSTR)=proc(k,"AddAtomA");
	ATOM (WINAPI *find)(LPCSTR)=proc(k,"FindAtomA");
	UINT (WINAPI *name)(ATOM,LPSTR,int)=proc(k,"GetAtomNameA");
	ATOM a=add("LocalTest"); char buf[32]; CHECK(a>=0xc000); CHECK_EQ(find("localtest"),a);
	CHECK_EQ(name(a,buf,sizeof(buf)),9); CHECK_STR(buf,"LocalTest");

	int (*isctype_)(int,int)=proc(c,"_isctype");
	int (*wcmp)(const WCHAR*,const WCHAR*,size_t)=proc(c,"wcsncmp");
	WCHAR wa[]={ 'a','b',0 }, wb[]={ 'a','c',0 };
	CHECK(isctype_('A',0x100)); CHECK(wcmp(wa,wb,1)==0); CHECK(wcmp(wa,wb,2)<0);
	int *mb=proc(c,"__mb_cur_max"); CHECK_EQ(*mb,1);

	HWAVEOUT h=NULL; WAVEFORMATEX fmt={WAVE_FORMAT_PCM,1,8000,8000,1,8};
	UINT (WINAPI *open_)(HWAVEOUT*,UINT,const WAVEFORMATEX*,DWORD_PTR,DWORD_PTR,DWORD)=proc(m,"waveOutOpen");
	UINT (WINAPI *prep)(HWAVEOUT,WAVEHDR*,UINT)=proc(m,"waveOutPrepareHeader");
	UINT (WINAPI *write_)(HWAVEOUT,WAVEHDR*,UINT)=proc(m,"waveOutWrite");
	UINT (WINAPI *close_)(HWAVEOUT)=proc(m,"waveOutClose");
	char sample[4]={0}; WAVEHDR hdr={.lpData=sample,.dwBufferLength=sizeof(sample)};
	CHECK_EQ(open_(&h,WAVE_MAPPER,&fmt,0,0,0),MMSYSERR_NOERROR);
	CHECK_EQ(prep(h,&hdr,sizeof(hdr)),MMSYSERR_NOERROR); CHECK_EQ(write_(h,&hdr,sizeof(hdr)),0);
	CHECK(hdr.dwFlags & WHDR_DONE); CHECK_EQ(close_(h),0);

	mod_free(m); mod_free(c); mod_free(g); mod_free(u); mod_free(k);
	return test_summary("test_missing_apis");
}
