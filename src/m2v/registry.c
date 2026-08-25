/*******************************************************************
 Registry Module
 *******************************************************************/
#include <windows.h>
#include <winreg.h>

#include "config.h"
#include "plugin.h"
#include "filename.h"

#define REGISTRY_C
#include "registry.h"

int get_color_conversion_type();
int get_idct_type();
int get_simd_mode();
int get_field_mode();
int get_resize_mode();
int get_file_check_limit();
int get_color_matrix();
int get_gl_mode();
int get_file_mode();
int get_yuy2_mode();

//static const char REGISTRY_POSITION[] = "Software\\marumo\\mpeg2vid_vfp";

/* Read a decoder setting.

   The ini is shared with the AviUtl2 side of TSMemory, so these live
   in their own [M2V] section. [settings] is the name m2v used as a
   standalone plug-in; it is still read so older ini files keep
   working. Every key here is non-negative, so -1 works as "absent". */
static int get_ini_int(const char *key, int def, const char *inifile)
{
	int value = GetPrivateProfileIntA("M2V", key, -1, inifile);

	if (value < 0)
		value = GetPrivateProfileIntA("settings", key, def, inifile);

	return value;
}

static void get_ini_filename(char *filename)
{
	GetModuleFileNameA((HMODULE)get_dll_handle(),filename,MAX_PATH);
	strcpy(read_suffix(filename),".ini");
}

int get_color_conversion_type()
{
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("re_map",1,inifile);
	if(value){
		return 1;
	}

	return 0;
}

int get_idct_type()
{
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("idct_func",2,inifile);

	return value;
}

int get_simd_mode()
{
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	(void)inifile;
	value=0; /* x64 build: no MMX/SSE assembly available */

	return value;
}

int get_field_mode()
{
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("field_order",0,inifile);

	return value;
}

int get_resize_mode()
{
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("aspect_ratio",M2V_CONFIG_USE_ASPECT_RATIO,inifile);

	return value;
}

int get_filecheck_limit()
{
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("limit",1024*1024*8,inifile);

	return value;
}

int get_color_matrix()
{
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("color_matrix",0,inifile);

	return value;
}

int get_gl_mode()
{
#if 0
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("gl",0,inifile);

	return value;
#else
	return M2V_CONFIG_GL_NEVER_SAVE;
#endif
}

int get_file_mode()
{
#if 0
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("file",M2V_CONFIG_SINGLE_FILE,inifile);

	if(value != M2V_CONFIG_MULTI_FILE){
		return M2V_CONFIG_SINGLE_FILE;
	}

	return value;
#else
	return M2V_CONFIG_SINGLE_FILE;
#endif
}

int get_yuy2_mode()
{
	char inifile[MAX_PATH];
	int value;

	get_ini_filename(inifile);
	value=get_ini_int("yuy2_matrix",M2V_CONFIG_YUY2_CONVERT_NONE,inifile);

	return value;
}

