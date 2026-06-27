/* nwui_fs.c — reusable directory enumeration for toolkit apps (see nwui_fs.h). */
#include "nwui_fs.h"
#include <dirent.h>
#include <string.h>

void *nwui_dir_open(const char *path)
{
	return (void *) opendir(path);
}

int nwui_dir_next(void *d, char *name, int cap, int *is_dir)
{
	DIR *dir = (DIR *) d;
	if (!dir || cap <= 0) return 0;
	struct dirent *e;
	while ((e = readdir(dir)) != 0) {
		if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
		    (e->d_name[1] == '.' && e->d_name[2] == 0)))
			continue;                       /* skip "." and ".." */
		int k = 0;
		for (; e->d_name[k] && k < cap - 1; k++) name[k] = e->d_name[k];
		name[k] = 0;
		if (is_dir) *is_dir = (e->d_type == DT_DIR);
		return 1;
	}
	return 0;
}

void nwui_dir_close(void *d)
{
	if (d) closedir((DIR *) d);
}
