#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <locale.h>
#include <errno.h>
#include <stdio.h>

#include "opts.h"
#include "findbranch.h"
#include "cache.h"
#include "general.h"
#include "cow.h"
#include "cow_utils.h"


/**
  * Actually create the directory here.
  */
static int do_create(const char *path, int nroot_ro, int nroot_rw) {
	char dirp[PATHLEN_MAX]; // dir path to create
	sprintf(dirp, "%s%s", uopt.roots[nroot_rw].path, path);

	struct stat buf;
	int res = stat(dirp, &buf);
	if (res != -1) return 0; // already exists

	// root does not exist yet, create it with stat data from the root
	char o_dirp[PATHLEN_MAX]; // the pathname we want to copy
	sprintf(o_dirp, "%s%s", uopt.roots[nroot_ro].path, path);

	res = stat(o_dirp, &buf);
	if (res == -1) return 1; // lower level root removed in the mean time?

	res = mkdir(dirp, buf.st_mode);
	if (res == -1) {
		syslog (LOG_DAEMON, "Creating %s failed: \n", dirp);
		return 1;
	}

	res = chown(dirp, buf.st_uid, buf.st_gid);
	if (res == -1) return 1; // directory already removed by another process?

	// TODO: time, but its values are modified by the next dir/file creation steps?

	return 0;
}

/**
  * l_nroot (lower nroot than nroot) is write protected, create the dir path on
  * nroot for an other COW operation.
  */
int path_create(const char *path, int nroot_ro, int nroot_rw) {
	if (!uopt.cow_enabled) return 0;

	if (strlen(path) + strlen(uopt.roots[nroot_rw].path) > PATHLEN_MAX
	|| strlen(path) + strlen(uopt.roots[nroot_ro].path) > PATHLEN_MAX) {
		// TODO: how to handle that?
		return 1;
	}

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", uopt.roots[nroot_rw].path, path);

	struct stat st;
	if (!stat(p, &st)) {
		// path does already exists, no need to create it
		return 0;
	}

	char *walk = (char *)path;

	// first slashes
	while (*walk != '\0' && *walk == '/') walk++;

	do {
		while (*walk != '\0' && *walk != '/') walk++;
	
		// +1 due to \0, which gets added automatically
		snprintf(p, (walk - path) + 1, path);
		int res = do_create(p, nroot_ro, nroot_rw);
		if (res) return res; // creating the direcory failed

		// slashes between path names
		while (*walk != '\0' && *walk == '/') walk++;
	} while (*walk != '\0');

	return 0;
}

/**
 * As path_create(), but ignore the last segment in path,
 * i.e. it might be a filename.
 **/
int path_create_cutlast(const char *path, int nroot_ro, int nroot_rw) {
	char *dname = u_dirname(path);
	int ret = path_create(dname, nroot_ro, nroot_rw);
	free(dname);

	return ret;
}
/**
 * initiate the cow-copy action
 */
int cow_cp(const char *path, int root_ro, int root_rw) {
	// create the path to the file
	path_create_cutlast(path, root_ro, root_rw);

	char from[PATHLEN_MAX], to[PATHLEN_MAX];
	snprintf(from, PATHLEN_MAX, "%s%s", uopt.roots[root_ro].path, path);
	snprintf(to, PATHLEN_MAX, "%s%s", uopt.roots[root_rw].path, path);

	setlocale(LC_ALL, "");

	struct cow cow;

	cow.uid = getuid();

	// Copy the umask for explicit mode setting.
	cow.umask = umask(0);
	umask(cow.umask);

	cow.from_path = from;
	cow.to_path = to;

	struct stat buf;
	lstat(cow.from_path, &buf);
	cow.stat = &buf;

	switch (buf.st_mode & S_IFMT) {
		case S_IFLNK:
			return copy_link(&cow);
		case S_IFDIR:
			return path_create(path, root_ro, root_rw);
		case S_IFBLK:
		case S_IFCHR:
			return copy_special(&cow);
		case S_IFIFO:
			return copy_fifo(&cow);
		case S_IFSOCK:
			syslog(LOG_WARNING, "COW of sockets not supported: %s\n", cow.from_path);
			return 1;
		default:
			return copy_file(&cow);
	}	
}