/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "attr.h"

#include "repository.h"
#include "sysdir.h"
#include "config.h"
#include "attr_file.h"
#include "ignore.h"
#include "git2/oid.h"
#include <ctype.h>

const char *git_attr__true  = "[internal]__TRUE__";
const char *git_attr__false = "[internal]__FALSE__";
const char *git_attr__unset = "[internal]__UNSET__";

git_attr_value_t git_attr_value(const char *attr)
{
	if (attr == NULL || attr == git_attr__unset)
		return GIT_ATTR_VALUE_UNSPECIFIED;

	if (attr == git_attr__true)
		return GIT_ATTR_VALUE_TRUE;

	if (attr == git_attr__false)
		return GIT_ATTR_VALUE_FALSE;

	return GIT_ATTR_VALUE_STRING;
}

static int collect_attr_files(
	git_repository *repo,
	git_attr_session *attr_session,
	uint32_t flags,
	const char *path,
	git_vector *files);

static void release_attr_files(git_vector *files);

int git_attr_get(
	const char **value,
	git_repository *repo,
	uint32_t flags,
	const char *pathname,
	const char *name)
{
	int error;
	git_attr_path path;
	git_vector files = GIT_VECTOR_INIT;
	size_t i, j;
	git_attr_file *file;
	git_attr_name attr;
	git_attr_rule *rule;
	git_dir_flag dir_flag = GIT_DIR_FLAG_UNKNOWN;

	GIT_ASSERT_ARG(value);
	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(name);

	*value = NULL;

	if (git_repository_is_bare(repo))
		dir_flag = GIT_DIR_FLAG_FALSE;

	if (git_attr_path__init(&path, repo, pathname, git_repository_workdir(repo), dir_flag) < 0)
		return -1;

	if ((error = collect_attr_files(repo, NULL, flags, pathname, &files)) < 0)
		goto cleanup;

	memset(&attr, 0, sizeof(attr));
	attr.name = name;
	attr.name_hash = git_attr_file__name_hash(name);

	git_vector_foreach(&files, i, file) {

		git_attr_file__foreach_matching_rule(file, &path, j, rule) {
			size_t pos;

			if (!git_vector_bsearch(&pos, &rule->assigns, &attr)) {
				*value = ((git_attr_assignment *)git_vector_get(
							  &rule->assigns, pos))->value;
				goto cleanup;
			}
		}
	}

cleanup:
	release_attr_files(&files);
	git_attr_path__free(&path);

	return error;
}


typedef struct {
	git_attr_name name;
	git_attr_assignment *found;
} attr_get_many_info;

int git_attr_get_many_with_session(
	const char **values,
	git_repository *repo,
	git_attr_session *attr_session,
	uint32_t flags,
	const char *pathname,
	size_t num_attr,
	const char **names)
{
	int error;
	git_attr_path path;
	git_vector files = GIT_VECTOR_INIT;
	size_t i, j, k;
	git_attr_file *file;
	git_attr_rule *rule;
	attr_get_many_info *info = NULL;
	size_t num_found = 0;
	git_dir_flag dir_flag = GIT_DIR_FLAG_UNKNOWN;

	if (!num_attr)
		return 0;

	GIT_ASSERT_ARG(values);
	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(pathname);
	GIT_ASSERT_ARG(names);

	if (git_repository_is_bare(repo))
		dir_flag = GIT_DIR_FLAG_FALSE;

	if (git_attr_path__init(&path, repo, pathname, git_repository_workdir(repo), dir_flag) < 0)
		return -1;

	if ((error = collect_attr_files(repo, attr_session, flags, pathname, &files)) < 0)
		goto cleanup;

	info = git__calloc(num_attr, sizeof(attr_get_many_info));
	GIT_ERROR_CHECK_ALLOC(info);

	git_vector_foreach(&files, i, file) {

		git_attr_file__foreach_matching_rule(file, &path, j, rule) {

			for (k = 0; k < num_attr; k++) {
				size_t pos;

				if (info[k].found != NULL) /* already found assignment */
					continue;

				if (!info[k].name.name) {
					info[k].name.name = names[k];
					info[k].name.name_hash = git_attr_file__name_hash(names[k]);
				}

				if (!git_vector_bsearch(&pos, &rule->assigns, &info[k].name)) {
					info[k].found = (git_attr_assignment *)
						git_vector_get(&rule->assigns, pos);
					values[k] = info[k].found->value;

					if (++num_found == num_attr)
						goto cleanup;
				}
			}
		}
	}

	for (k = 0; k < num_attr; k++) {
		if (!info[k].found)
			values[k] = NULL;
	}

cleanup:
	release_attr_files(&files);
	git_attr_path__free(&path);
	git__free(info);

	return error;
}

int git_attr_get_many(
	const char **values,
	git_repository *repo,
	uint32_t flags,
	const char *pathname,
	size_t num_attr,
	const char **names)
{
	return git_attr_get_many_with_session(
		values, repo, NULL, flags, pathname, num_attr, names);
}

int git_attr_foreach(
	git_repository *repo,
	uint32_t flags,
	const char *pathname,
	int (*callback)(const char *name, const char *value, void *payload),
	void *payload)
{
	int error;
	git_attr_path path;
	git_vector files = GIT_VECTOR_INIT;
	size_t i, j, k;
	git_attr_file *file;
	git_attr_rule *rule;
	git_attr_assignment *assign;
	git_strmap *seen = NULL;
	git_dir_flag dir_flag = GIT_DIR_FLAG_UNKNOWN;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(callback);

	if (git_repository_is_bare(repo))
		dir_flag = GIT_DIR_FLAG_FALSE;

	if (git_attr_path__init(&path, repo, pathname, git_repository_workdir(repo), dir_flag) < 0)
		return -1;

	if ((error = collect_attr_files(repo, NULL, flags, pathname, &files)) < 0 ||
	    (error = git_strmap_new(&seen)) < 0)
		goto cleanup;

	git_vector_foreach(&files, i, file) {

		git_attr_file__foreach_matching_rule(file, &path, j, rule) {

			git_vector_foreach(&rule->assigns, k, assign) {
				/* skip if higher priority assignment was already seen */
				if (git_strmap_exists(seen, assign->name))
					continue;

				if ((error = git_strmap_set(seen, assign->name, assign)) < 0)
					goto cleanup;

				error = callback(assign->name, assign->value, payload);
				if (error) {
					git_error_set_after_callback(error);
					goto cleanup;
				}
			}
		}
	}

cleanup:
	git_strmap_free(seen);
	release_attr_files(&files);
	git_attr_path__free(&path);

	return error;
}

static int preload_attr_source(
	git_repository *repo,
	git_attr_session *attr_session,
	git_attr_file_source *source)
{
	int error;
	git_attr_file *preload = NULL;

	if (!source)
		return 0;

	error = git_attr_cache__get(&preload, repo, attr_session, source,
	                            git_attr_file__parse_buffer, true);

	if (!error)
		git_attr_file__free(preload);

	return error;
}

GIT_INLINE(int) preload_attr_file(
	git_repository *repo,
	git_attr_session *attr_session,
	const char *base,
	const char *filename)
{
	git_attr_file_source source = { GIT_ATTR_FILE_SOURCE_FILE };

	if (!filename)
		return 0;

	source.base = base;
	source.filename = filename;

	return preload_attr_source(repo, attr_session, &source);
}

static int system_attr_file(
	git_buf *out,
	git_attr_session *attr_session)
{
	int error;

	if (!attr_session) {
		error = git_sysdir_find_system_file(out, GIT_ATTR_FILE_SYSTEM);

		if (error == GIT_ENOTFOUND)
			git_error_clear();

		return error;
	}

	if (!attr_session->init_sysdir) {
		error = git_sysdir_find_system_file(&attr_session->sysdir, GIT_ATTR_FILE_SYSTEM);

		if (error == GIT_ENOTFOUND)
			git_error_clear();
		else if (error)
			return error;

		attr_session->init_sysdir = 1;
	}

	if (attr_session->sysdir.size == 0)
		return GIT_ENOTFOUND;

	/* We can safely provide a git_buf with no allocation (asize == 0) to
	 * a consumer. This allows them to treat this as a regular `git_buf`,
	 * but their call to `git_buf_dispose` will not attempt to free it.
	 */
	git_buf_attach_notowned(
		out, attr_session->sysdir.ptr, attr_session->sysdir.size);
	return 0;
}

static int attr_setup(
	git_repository *repo,
	git_attr_session *attr_session,
	uint32_t flags)
{
	git_buf system = GIT_BUF_INIT, info = GIT_BUF_INIT;
	git_attr_file_source index_source = { GIT_ATTR_FILE_SOURCE_INDEX, NULL, GIT_ATTR_FILE };
	git_attr_file_source head_source = { GIT_ATTR_FILE_SOURCE_COMMIT, NULL, GIT_ATTR_FILE };
	git_index *idx = NULL;
	const char *workdir;
	int error = 0;

	if (attr_session && attr_session->init_setup)
		return 0;

	if ((error = git_attr_cache__init(repo)) < 0)
		return error;

	/*
	 * Preload attribute files that could contain macros so the
	 * definitions will be available for later file parsing.
	 */

	if ((error = system_attr_file(&system, attr_session)) < 0 ||
	    (error = preload_attr_file(repo, attr_session, NULL, system.ptr)) < 0) {
		if (error != GIT_ENOTFOUND)
			goto out;

		error = 0;
	}

	if ((error = preload_attr_file(repo, attr_session, NULL,
	                               git_repository_attr_cache(repo)->cfg_attr_file)) < 0)
		goto out;

	if ((error = git_repository_item_path(&info, repo, GIT_REPOSITORY_ITEM_INFO)) < 0 ||
	    (error = preload_attr_file(repo, attr_session, info.ptr, GIT_ATTR_FILE_INREPO)) < 0) {
		if (error != GIT_ENOTFOUND)
			goto out;

		error = 0;
	}

	if ((workdir = git_repository_workdir(repo)) != NULL &&
	    (error = preload_attr_file(repo, attr_session, workdir, GIT_ATTR_FILE)) < 0)
			goto out;

	if ((error = git_repository_index__weakptr(&idx, repo)) < 0 ||
	    (error = preload_attr_source(repo, attr_session, &index_source)) < 0)
			goto out;

	if ((flags & GIT_ATTR_CHECK_INCLUDE_HEAD) != 0 &&
	    (error = preload_attr_source(repo, attr_session, &head_source)) < 0)
		goto out;

	if (attr_session)
		attr_session->init_setup = 1;

out:
	git_buf_dispose(&system);
	git_buf_dispose(&info);

	return error;
}

int git_attr_add_macro(
	git_repository *repo,
	const char *name,
	const char *values)
{
	int error;
	git_attr_rule *macro = NULL;
	git_pool *pool;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(name);

	if ((error = git_attr_cache__init(repo)) < 0)
		return error;

	macro = git__calloc(1, sizeof(git_attr_rule));
	GIT_ERROR_CHECK_ALLOC(macro);

	pool = &git_repository_attr_cache(repo)->pool;

	macro->match.pattern = git_pool_strdup(pool, name);
	GIT_ERROR_CHECK_ALLOC(macro->match.pattern);

	macro->match.length = strlen(macro->match.pattern);
	macro->match.flags = GIT_ATTR_FNMATCH_MACRO;

	error = git_attr_assignment__parse(repo, pool, &macro->assigns, &values);

	if (!error)
		error = git_attr_cache__insert_macro(repo, macro);

	if (error < 0)
		git_attr_rule__free(macro);

	return error;
}

typedef struct {
	git_repository *repo;
	git_attr_session *attr_session;
	uint32_t flags;
	const char *workdir;
	git_index *index;
	git_vector *files;
} attr_walk_up_info;

static int attr_decide_sources(
	uint32_t flags,
	bool has_wd,
	bool has_index,
	git_attr_file_source_t *srcs)
{
	int count = 0;

	switch (flags & 0x03) {
	case GIT_ATTR_CHECK_FILE_THEN_INDEX:
		if (has_wd)
			srcs[count++] = GIT_ATTR_FILE_SOURCE_FILE;
		if (has_index)
			srcs[count++] = GIT_ATTR_FILE_SOURCE_INDEX;
		break;
	case GIT_ATTR_CHECK_INDEX_THEN_FILE:
		if (has_index)
			srcs[count++] = GIT_ATTR_FILE_SOURCE_INDEX;
		if (has_wd)
			srcs[count++] = GIT_ATTR_FILE_SOURCE_FILE;
		break;
	case GIT_ATTR_CHECK_INDEX_ONLY:
		if (has_index)
			srcs[count++] = GIT_ATTR_FILE_SOURCE_INDEX;
		break;
	}

	if ((flags & GIT_ATTR_CHECK_INCLUDE_HEAD) != 0)
		srcs[count++] = GIT_ATTR_FILE_SOURCE_COMMIT;

	return count;
}

static int push_attr_source(
	git_repository *repo,
	git_attr_session *attr_session,
	git_vector *list,
	git_attr_file_source *source,
	bool allow_macros)
{
	int error = 0;
	git_attr_file *file = NULL;

	error = git_attr_cache__get(&file, repo, attr_session,
	                            source,
	                            git_attr_file__parse_buffer,
	                            allow_macros);

	if (error < 0)
		return error;

	if (file != NULL) {
		if ((error = git_vector_insert(list, file)) < 0)
			git_attr_file__free(file);
	}

	return error;
}

GIT_INLINE(int) push_attr_file(
	git_repository *repo,
	git_attr_session *attr_session,
	git_vector *list,
	const char *base,
	const char *filename)
{
	git_attr_file_source source = { GIT_ATTR_FILE_SOURCE_FILE, base, filename };
	return push_attr_source(repo, attr_session, list, &source, true);
}

static int push_one_attr(void *ref, const char *path)
{
	attr_walk_up_info *info = (attr_walk_up_info *)ref;
	git_attr_file_source_t src[GIT_ATTR_FILE_NUM_SOURCES];
	int error = 0, n_src, i;
	bool allow_macros;

	n_src = attr_decide_sources(
		info->flags, info->workdir != NULL, info->index != NULL, src);
	allow_macros = info->workdir ? !strcmp(info->workdir, path) : false;

	for (i = 0; !error && i < n_src; ++i) {
		git_attr_file_source source = { src[i], path, GIT_ATTR_FILE };

		error = push_attr_source(info->repo, info->attr_session, info->files,
		                       &source, allow_macros);
	}

	return error;
}

static void release_attr_files(git_vector *files)
{
	size_t i;
	git_attr_file *file;

	git_vector_foreach(files, i, file) {
		git_attr_file__free(file);
		files->contents[i] = NULL;
	}
	git_vector_free(files);
}

static int collect_attr_files(
	git_repository *repo,
	git_attr_session *attr_session,
	uint32_t flags,
	const char *path,
	git_vector *files)
{
	int error = 0;
	git_buf dir = GIT_BUF_INIT, attrfile = GIT_BUF_INIT;
	const char *workdir = git_repository_workdir(repo);
	attr_walk_up_info info = { NULL };

	if ((error = attr_setup(repo, attr_session, flags)) < 0)
		return error;

	/* Resolve path in a non-bare repo */
	if (workdir != NULL) {
		if (!(error = git_repository_workdir_path(&dir, repo, path)))
			error = git_path_find_dir(&dir);
	}
	else {
		error = git_path_dirname_r(&dir, path);
	}

	if (error < 0)
		goto cleanup;

	/* in precendence order highest to lowest:
	 * - $GIT_DIR/info/attributes
	 * - path components with .gitattributes
	 * - config core.attributesfile
	 * - $GIT_PREFIX/etc/gitattributes
	 */

	if ((error = git_repository_item_path(&attrfile, repo, GIT_REPOSITORY_ITEM_INFO)) < 0 ||
	    (error = push_attr_file(repo, attr_session, files, attrfile.ptr, GIT_ATTR_FILE_INREPO)) < 0) {
		if (error != GIT_ENOTFOUND)
			goto cleanup;
	}

	info.repo = repo;
	info.attr_session = attr_session;
	info.flags = flags;
	info.workdir = workdir;
	if (git_repository_index__weakptr(&info.index, repo) < 0)
		git_error_clear(); /* no error even if there is no index */
	info.files = files;

	if (!strcmp(dir.ptr, "."))
		error = push_one_attr(&info, "");
	else
		error = git_path_walk_up(&dir, workdir, push_one_attr, &info);

	if (error < 0)
		goto cleanup;

	if (git_repository_attr_cache(repo)->cfg_attr_file != NULL) {
		error = push_attr_file(repo, attr_session, files, NULL, git_repository_attr_cache(repo)->cfg_attr_file);
		if (error < 0)
			goto cleanup;
	}

	if ((flags & GIT_ATTR_CHECK_NO_SYSTEM) == 0) {
		error = system_attr_file(&dir, attr_session);

		if (!error)
			error = push_attr_file(repo, attr_session, files, NULL, dir.ptr);
		else if (error == GIT_ENOTFOUND)
			error = 0;
	}

 cleanup:
	if (error < 0)
		release_attr_files(files);
	git_buf_dispose(&attrfile);
	git_buf_dispose(&dir);

	return error;
}
