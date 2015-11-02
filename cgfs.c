/*
 * Copyright © 2015 Canonical Limited
 *
 * Authors:
 *   Serge Hallyn <serge.hallyn@ubuntu.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <grp.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <cgfs.h>

/* directory under which we mount the controllers - /run/lxcfs/controllers */
char *basedir;
#define basedir RUNTIME_PATH "/lxcfs/controllers"

char *must_copy_string(const char *str)
{
	char *dup = NULL;
	if (!str)
		return NULL;
	do {
		dup = strdup(str);
	} while (!dup);

	return dup;
}

static inline void drop_trailing_newlines(char *s)
{
	int l;

	for (l=strlen(s); l>0 && s[l-1] == '\n'; l--)
		s[l-1] = '\0';
}

static void append_line(char **contents, char *line, size_t *len)
{
	size_t newlen = *len + strlen(line);

	if (!*contents) {
		do {
			*contents = malloc(*len + 1);
		} while (!*contents);
	} else {
		char *tmp;
		do {
			tmp = realloc(*contents, newlen + 1);
		} while (!tmp);

		*contents = tmp;
	}

	strcpy(*contents + *len, line);
	*len = newlen;
}

static char *read_file(const char *from)
{
	char *line = NULL;
	char *contents = NULL;
	FILE *f = fopen(from, "r");
	size_t len = 0, fulllen = 0;

	if (!f)
		return NULL;

	while (getline(&line, &len, f) != -1) {
		append_line(&contents, line, &fulllen);
	}
	fclose(f);

	if (contents)
		drop_trailing_newlines(contents);
	free(line);
	return contents;
}

static bool write_string(const char *fnam, const char *string)
{
	FILE *f;
	size_t len, ret;

	if (!(f = fopen(fnam, "w")))
		return false;
	len = strlen(string);
	ret = fwrite(string, 1, len, f);
	if (ret != len) {
		fprintf(stderr, "Error writing to file: %s\n", strerror(errno));
		fclose(f);
		return false;
	}
	if (fclose(f) < 0) {
		fprintf(stderr, "Error writing to file: %s\n", strerror(errno));
		return false;
	}
	return true;
}

static bool store_hierarchy(char *stridx, char *h)
{
	int idx = atoi(stridx);
	size_t needed_len = (idx + 1) * sizeof(char *);

	if (idx < 0 || idx > 30) {
		fprintf(stderr, "Error: corrupt /proc/self/cgroup\n");
		return false;
	}

	if (!hierarchies) {
		hierarchies = malloc(needed_len);
		memset(hierarchies, 0, needed_len);
		num_hierarchies = idx + 1;
	} else if (idx >= num_hierarchies) {
		char **tmp;
		size_t old_len = (num_hierarchies + 1) * sizeof(char *);
		do {
			tmp = malloc(needed_len);
		} while (!tmp);
		memset(tmp, 0, needed_len);
		memcpy(tmp, hierarchies, old_len);
		free(hierarchies);
		hierarchies = tmp;
		num_hierarchies = idx + 1;
	}
	
	if (hierarchies[idx]) {
		fprintf(stderr, "Error: corrupt /proc/self/cgroup\n");
		return false;
	}
	hierarchies[idx] = must_copy_string(h);
	return true;
}

void print_subsystems(void)
{
	int i;

	printf("hierarchies:");
	for (i = 0; i < num_hierarchies; i++) {
		if (hierarchies[i])
			printf(" %d: %s\n", i, hierarchies[i]);
	}
}

static bool collect_subsystems(void)
{
	FILE *f;
	char *line = NULL;
	size_t len = 0;
	bool bret = false;

	if ((f = fopen("/proc/self/cgroup", "r")) == NULL) {
		fprintf(stderr, "Error opening /proc/self/cgroup: %s\n", strerror(errno));
		return false;
	}
	while (getline(&line, &len, f) != -1) {
		char *p, *p2;

		p = strchr(line, ':');
		if (!p)
			goto out;
		*(p++) = '\0';

		p2 = strrchr(p, ':');
		if (!p2)
			goto out;
		*p2 = '\0';

		if (!store_hierarchy(line, p))
			goto out;
	}

	print_subsystems();
	bret = true;

out:
	free(line);
	fclose(f);
	return bret;
}

static bool do_mount_cgroups(void)
{
	int i, ret;
	char target[MAXPATHLEN];

	for (i=0; i<num_hierarchies; i++) {
		if (!hierarchies[i])
			continue;
		ret = snprintf(target, MAXPATHLEN, "%s/%s", basedir, hierarchies[i]);
		if (ret < 0 || ret >= MAXPATHLEN)
			return false;
		if (mkdir(target, 0755) < 0 && errno != EEXIST)
			return false;
		if (mount(hierarchies[i], target, "cgroup", 0, hierarchies[i]) < 0) {
			fprintf(stderr, "Failed mounting cgroups\n");
			return false;
		}
	}

	return true;
}

static bool umount_if_mounted(void)
{
	if (umount2(basedir, MNT_DETACH) < 0 && errno != EINVAL) {
		fprintf(stderr, "failed to umount %s: %s\n", basedir,
			strerror(errno));
		return false;
	}
	return true;
}

static bool mkdir_p(const char *dir, mode_t mode)
{
	const char *tmp = dir;
	const char *orig = dir;
	char *makeme;

	do {
		dir = tmp + strspn(tmp, "/");
		tmp = dir + strcspn(dir, "/");
		makeme = strndup(orig, dir - orig);
		if (*makeme) {
			if (mkdir(makeme, mode) && errno != EEXIST) {
				fprintf(stderr, "failed to create directory '%s': %s",
					makeme, strerror(errno));
				free(makeme);
				return false;
			}
		}
		free(makeme);
	} while(tmp != dir);

	return true;
}
static bool setup_cgfs_dir(void)
{
	if (!mkdir_p(basedir, 0700)) {
		fprintf(stderr, "Failed to create lxcfs cgdir\n");
		return false;
	}
	if (!umount_if_mounted()) {
		fprintf(stderr, "Failed to clean up old lxcfs cgdir\n");
		return false;
	}
	if (mount("tmpfs", basedir, "tmpfs", 0, "size=100000,mode=700") < 0) {
		fprintf(stderr, "Failed to mount tmpfs for private controllers\n");
		return false;
	}
	return true;
}

bool cgfs_setup_controllers(void)
{
	if (!setup_cgfs_dir()) {
		return false;
	}

	if (!collect_subsystems()) {
		fprintf(stderr, "failed to collect cgroup subsystems\n");
		return false;
	}

	if (!do_mount_cgroups()) {
		fprintf(stderr, "Failed to set up cgroup mounts\n");
		return false;
	}

	return true;
}

/* do we need to do any massaging here?  I'm not sure... */
char *find_mounted_controller(const char *controller)
{
	int i = 0;
	for (i = 0; i < num_hierarchies; i++) {
		if (hierarchies[i] && strcmp(hierarchies[i], controller) == 0)
			return hierarchies[i];
	}

	return NULL;
}

bool cgfs_set_value(const char *controller, const char *cgroup, const char *file,
		const char *value)
{
	size_t len;
	char *fnam, *tmpc = find_mounted_controller(controller);

	if (!tmpc)
		return false;
	/* basedir / tmpc / cgroup / file \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cgroup) + strlen(file) + 4;
	fnam = alloca(len);
	snprintf(fnam, len, "%s/%s/%s/%s", basedir, tmpc, cgroup, file);
	
	return write_string(fnam, value);
}

bool cgfs_create(const char *controller, const char *cg)
{
	size_t len;
	char *dirnam, *tmpc = find_mounted_controller(controller);

	if (!tmpc)
		return false;
	/* basedir / tmpc / cg \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cg) + 3;
	dirnam = alloca(len);
	snprintf(dirnam, len, "%s/%s/%s", basedir,tmpc, cg);
	if (mkdir(dirnam, 0755) < 0 && errno != EEXIST)
		return false;
	return true;
}

static bool recursive_rmdir(const char *dirname)
{
	struct dirent dirent, *direntp;
	DIR *dir;
	bool ret = false;
	char pathname[MAXPATHLEN];

	dir = opendir(dirname);
	if (!dir) {
		fprintf(stderr, "%s: failed to open %s: %s\n", __func__, dirname, strerror(errno));
		return false;
	}

	while (!readdir_r(dir, &dirent, &direntp)) {
		struct stat mystat;
		int rc;

		if (!direntp)
			break;

		if (!strcmp(direntp->d_name, ".") ||
		    !strcmp(direntp->d_name, ".."))
			continue;

		rc = snprintf(pathname, MAXPATHLEN, "%s/%s", dirname, direntp->d_name);
		if (rc < 0 || rc >= MAXPATHLEN) {
			fprintf(stderr, "pathname too long\n");
			continue;
		}

		ret = lstat(pathname, &mystat);
		if (ret) {
			fprintf(stderr, "%s: failed to stat %s: %s\n", __func__, pathname, strerror(errno));
			continue;
		}
		if (S_ISDIR(mystat.st_mode)) {
			if (!recursive_rmdir(pathname))
				fprintf(stderr, "Error removing %s\n", pathname);
		}
	}

	ret = true;
	if (closedir(dir) < 0) {
		fprintf(stderr, "%s: failed to close directory %s: %s\n", __func__, dirname, strerror(errno));
		ret = false;
	}

	if (rmdir(dirname) < 0) {
		fprintf(stderr, "%s: failed to delete %s: %s\n", __func__, dirname, strerror(errno));
		ret = false;
	}

	return ret;
}

bool cgfs_remove(const char *controller, const char *cg)
{
	size_t len;
	char *dirnam, *tmpc = find_mounted_controller(controller);

	if (!tmpc)
		return false;
	/* basedir / tmpc / cg \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cg) + 3;
	dirnam = alloca(len);
	snprintf(dirnam, len, "%s/%s/%s", basedir,tmpc, cg);
	return recursive_rmdir(dirnam);
}

bool cgfs_chmod_file(const char *controller, const char *file, mode_t mode)
{
	size_t len;
	char *pathname, *tmpc = find_mounted_controller(controller);

	if (!tmpc)
		return false;
	/* basedir / tmpc / file \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(file) + 3;
	pathname = alloca(len);
	snprintf(pathname, len, "%s/%s/%s", basedir, tmpc, file);
	if (chmod(pathname, mode) < 0)
		return false;
	return true;
}

bool cgfs_chown_file(const char *controller, const char *file, uid_t uid, gid_t gid)
{
	size_t len;
	char *pathname, *tmpc = find_mounted_controller(controller);

	if (!tmpc)
		return false;
	/* basedir / tmpc / file \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(file) + 3;
	pathname = alloca(len);
	snprintf(pathname, len, "%s/%s/%s", basedir, tmpc, file);
	if (chown(pathname, uid, gid) < 0)
		return false;
	return true;
}

FILE *open_pids_file(const char *controller, const char *cgroup)
{
	size_t len;
	char *pathname, *tmpc = find_mounted_controller(controller);

	if (!tmpc)
		return NULL;
	/* basedir / tmpc / cgroup / "cgroup.procs" \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cgroup) + 4 + strlen("cgroup.procs");
	pathname = alloca(len);
	snprintf(pathname, len, "%s/%s/%s/cgroup.procs", basedir, tmpc, cgroup);
	return fopen(pathname, "w");
}

#define BATCH_SIZE 50
bool cgfs_list_children(const char *controller, const char *cgroup, char ***list)
{
	size_t len;
	char *dirname, *tmpc = find_mounted_controller(controller);
	char pathname[MAXPATHLEN];
	size_t sz = 0, asz = BATCH_SIZE;
	struct dirent dirent, *direntp;
	DIR *dir;
	int ret;

	do {
		*list = malloc(asz * sizeof(char *));
	} while (!*list);
	(*list)[0] = NULL;

	if (!tmpc)
		return NULL;

	/* basedir / tmpc / cgroup \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cgroup) + 3;
	dirname = alloca(len);
	snprintf(dirname, len, "%s/%s/%s", basedir, tmpc, cgroup);

	dir = opendir(dirname);
	if (!dir)
		return false;

	while (!readdir_r(dir, &dirent, &direntp)) {
		struct stat mystat;
		int rc;

		if (!direntp)
			break;

		if (!strcmp(direntp->d_name, ".") ||
		    !strcmp(direntp->d_name, ".."))
			continue;

		rc = snprintf(pathname, MAXPATHLEN, "%s/%s", dirname, direntp->d_name);
		if (rc < 0 || rc >= MAXPATHLEN) {
			fprintf(stderr, "%s: pathname too long under %s\n", __func__, dirname);
			continue;
		}

		ret = lstat(pathname, &mystat);
		if (ret) {
			fprintf(stderr, "%s: failed to stat %s: %s\n", __func__, pathname, strerror(errno));
			continue;
		}
		if (!S_ISDIR(mystat.st_mode))
			continue;

		if (sz+2 >= asz) {
			char **tmp;
			asz += BATCH_SIZE;
			do {
				tmp = realloc(*list, asz * sizeof(char *));
			} while  (!tmp);
			*list = tmp;
		}
		do {
			(*list)[sz] = strdup(direntp->d_name);
		} while (!(*list)[sz]);
		(*list)[sz+1] = NULL;
		sz++;
	}
	if (closedir(dir) < 0) {
		fprintf(stderr, "%s: failed closedir for %s: %s\n", __func__, dirname, strerror(errno));
		return false;
	}
	return true;
}

void free_key(struct cgfs_files *k)
{
	if (!k)
		return;
	free(k->name);
	free(k);
}

void free_keys(struct cgfs_files **keys)
{
	int i;

	if (!keys)
		return;
	for (i = 0; keys[i]; i++) {
		free_key(keys[i]);
	}
	free(keys);
}

bool cgfs_get_value(const char *controller, const char *cgroup, const char *file, char **value)
{
	size_t len;
	char *fnam, *tmpc = find_mounted_controller(controller);

	if (!tmpc)
		return false;
	/* basedir / tmpc / cgroup / file \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cgroup) + strlen(file) + 4;
	fnam = alloca(len);
	snprintf(fnam, len, "%s/%s/%s/%s", basedir, tmpc, cgroup, file);

	*value = read_file(fnam);
	return *value != NULL;
}

struct cgfs_files *cgfs_get_key(const char *controller, const char *cgroup, const char *file)
{
	size_t len;
	char *fnam, *tmpc = find_mounted_controller(controller);
	struct stat sb;
	struct cgfs_files *newkey;
	int ret;

	if (!tmpc)
		return false;

	if (*file == '/')
		file++;

	if (index(file, '/'))
		return NULL;

	/* basedir / tmpc / cgroup / file \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cgroup) + strlen(file) + 4;
	fnam = alloca(len);
	snprintf(fnam, len, "%s/%s/%s/%s", basedir, tmpc, cgroup, file);

	ret = stat(fnam, &sb);
	if (ret < 0)
		return NULL;

	if (!S_ISREG(sb.st_mode))
		return NULL;
	do {
		newkey = malloc(sizeof(struct cgfs_files));
	} while (!newkey);
	newkey->name = must_copy_string(file);
	newkey->uid = sb.st_uid;
	newkey->gid = sb.st_gid;
	newkey->mode = sb.st_mode;

	return newkey;
}

bool cgfs_list_keys(const char *controller, const char *cgroup, struct cgfs_files ***keys)
{
	size_t len;
	char *dirname, *tmpc = find_mounted_controller(controller);
	char pathname[MAXPATHLEN];
	size_t sz = 0, asz = 0;
	struct dirent dirent, *direntp;
	DIR *dir;
	int ret;

	*keys = NULL;
	if (!tmpc)
		return NULL;

	/* basedir / tmpc / cgroup \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cgroup) + 3;
	dirname = alloca(len);
	snprintf(dirname, len, "%s/%s/%s", basedir, tmpc, cgroup);

	dir = opendir(dirname);
	if (!dir)
		return false;

	while (!readdir_r(dir, &dirent, &direntp)) {
		struct stat mystat;
		int rc;

		if (!direntp)
			break;

		if (!strcmp(direntp->d_name, ".") ||
		    !strcmp(direntp->d_name, ".."))
			continue;

		rc = snprintf(pathname, MAXPATHLEN, "%s/%s", dirname, direntp->d_name);
		if (rc < 0 || rc >= MAXPATHLEN) {
			fprintf(stderr, "%s: pathname too long under %s\n", __func__, dirname);
			continue;
		}

		ret = lstat(pathname, &mystat);
		if (ret) {
			fprintf(stderr, "%s: failed to stat %s: %s\n", __func__, pathname, strerror(errno));
			continue;
		}
		if (!S_ISREG(mystat.st_mode))
			continue;

		if (sz+2 >= asz) {
			struct cgfs_files **tmp;
			asz += BATCH_SIZE;
			do {
				tmp = realloc(*keys, asz * sizeof(struct cgfs_files *));
			} while  (!tmp);
			*keys = tmp;
		}
		(*keys)[sz] = cgfs_get_key(controller, cgroup, direntp->d_name);
		(*keys)[sz+1] = NULL;
		if (!(*keys)[sz]) {
			fprintf(stderr, "%s: Error getting files under %s:%s\n",
				__func__, controller, cgroup);
			continue;
		}
		sz++;
	}
	if (closedir(dir) < 0) {
		fprintf(stderr, "%s: failed closedir for %s: %s\n", __func__, dirname, strerror(errno));
		return false;
	}
	return true;
}

bool is_child_cgroup(const char *controller, const char *cgroup, const char *f)
{      size_t len;
	char *fnam, *tmpc = find_mounted_controller(controller);
	int ret;
	struct stat sb;

	if (!tmpc)
		return false;
	/* basedir / tmpc / cgroup / f \0 */
	len = strlen(basedir) + strlen(tmpc) + strlen(cgroup) + strlen(f) + 4;
	fnam = alloca(len);
	snprintf(fnam, len, "%s/%s/%s/%s", basedir, tmpc, cgroup, f);

	ret = stat(fnam, &sb);
	if (ret < 0 || !S_ISDIR(sb.st_mode))
		return false;
	return true;
}
