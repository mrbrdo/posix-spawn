/* we want GNU extensions like POSIX_SPAWN_USEVFORK */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ruby.h>
#include <st.h>

#ifndef RUBY_VM
#include "node.h"
#endif

#ifndef RARRAY_LEN
#define RARRAY_LEN(ary) RARRAY(ary)->len
#endif
#ifndef RARRAY_PTR
#define RARRAY_PTR(ary) RARRAY(ary)->ptr
#endif
#ifndef RHASH_SIZE
#define RHASH_SIZE(hash) RHASH(hash)->tbl->num_entries
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

static VALUE rb_mFastSpawn;

static VALUE
rb_fastspawn_vspawn(VALUE self, VALUE env, VALUE argv, VALUE options)
{
	int i;
	int argc = RARRAY_LEN(argv);
	char *cargv[argc + 1];
	pid_t pid;

	cargv[argc] = NULL;
	for (i = 0; i < argc; i++)
		cargv[i] = StringValuePtr(RARRAY_PTR(argv)[i]);

	pid = vfork();
	if (pid < 0) {
		rb_sys_fail("vfork");
	}
	if (!pid) {
		execvp(cargv[0], cargv);
		_exit(1);
	}

	return INT2FIX(pid);
}

/* Hash iterator that sets up the posix_spawn_file_actions_t with addclose
 * instructions. Only hash pairs whose value is :close are processed. Keys may
 * be the :in, :out, :err, an IO object, or a Fixnum fd number.
 *
 * Returns ST_DELETE when an addclose instruction was added; ST_CONTINUE when
 * no operation was performed.
 */
static int
fastspawn_file_actions_addclose_iter(VALUE key, VALUE val, posix_spawn_file_actions_t *fops)
{
	int fd;

	/* we only care about { (IO|FD|:in|:out|:err) => :close } */
	if (TYPE(val) != T_SYMBOL || SYM2ID(val) != rb_intern("close"))
		return ST_CONTINUE;

	fd  = -1;
	switch (TYPE(key)) {
		case T_FIXNUM:
			/* FD => :close */
			fd = FIX2INT(key);
			break;

		case T_SYMBOL:
			/* (:in|:out|:err) => :close */
			if      (SYM2ID(key) == rb_intern("in"))   fd = 0;
			else if (SYM2ID(key) == rb_intern("out"))  fd = 1;
			else if (SYM2ID(key) == rb_intern("err"))  fd = 2;
			break;

		case T_OBJECT:
			/* IO => :close */
			if (rb_respond_to(key, rb_intern("to_io"))) {
				key = rb_funcall(key, rb_intern("to_io"), 0);
				fd = FIX2INT(rb_funcall(key, rb_intern("fileno"), 0));
			}
			break;

		default:
			break;
	}

	if (fd >= 0) {
		posix_spawn_file_actions_addclose(fops, fd);
		return ST_DELETE;
	} else {
		return ST_CONTINUE;
	}
}

static void
fastspawn_file_actions_addclose(posix_spawn_file_actions_t *fops, VALUE options)
{
	rb_hash_foreach(options, fastspawn_file_actions_addclose_iter, (VALUE)fops);
}

static int
each_env_check_i(VALUE key, VALUE val, VALUE arg)
{
	StringValuePtr(key);
	if (!NIL_P(val)) StringValuePtr(val);
	return ST_CONTINUE;
}

static int
each_env_i(VALUE key, VALUE val, VALUE arg)
{
	char *name = StringValuePtr(key);
	size_t len = strlen(name);

	/*
	 * Delete any existing values for this variable before inserting the new value.
	 * This implementation was copied from glibc's unsetenv().
	 */
	char **ep = (char **)arg;
	while (*ep != NULL)
		if (!strncmp (*ep, name, len) && (*ep)[len] == '=')
		{
			/* Found it.  Remove this pointer by moving later ones back.  */
			char **dp = ep;

			do
				dp[0] = dp[1];
			while (*dp++);
			/* Continue the loop in case NAME appears again.  */
		}
		else
			++ep;

	/*
	 * Insert the new value if we have one. We can assume there is space
	 * at the end of the list, since ep was preallocated to be big enough
	 * for the new entries.
	 */
	if (RTEST(val)) {
		char **ep = (char **)arg;
		char *cval = StringValuePtr(val);

		size_t cval_len = strlen(cval);
		size_t ep_len = len + 1 + cval_len + 1; /* +2 for null terminator and '=' separator */

		/* find the last entry */
		while (*ep != NULL) ++ep;
		*ep = malloc(ep_len);

		strncpy(*ep, name, len);
		(*ep)[len] = '=';
		strncpy(*ep + len + 1, cval, cval_len);
		(*ep)[ep_len-1] = 0;
	}

	return ST_CONTINUE;
}

static VALUE
rb_fastspawn_pspawn(VALUE self, VALUE env, VALUE argv, VALUE options)
{
	int i, ret;
	int argc = RARRAY_LEN(argv);
	char **envp = NULL;
	char *cargv[argc + 1];
	pid_t pid;
	posix_spawn_file_actions_t fops;
	posix_spawnattr_t attr;

	if (RTEST(env)) {
		/*
		 * Make sure env is a hash, and all keys and values are strings.
		 * We do this before allocating space for the new environment to
		 * prevent a leak when raising an exception after the calloc() below.
		 */
		Check_Type(env, T_HASH);
		rb_hash_foreach(env, each_env_check_i, 0);

		if (RHASH_SIZE(env) > 0) {
			char **curr = environ;
			int size = 0;
			if (curr) {
				while (*curr != NULL) ++curr, ++size;
			}

			char **new_env = calloc(size+RHASH_SIZE(env)+1, sizeof(char*));
			for (i = 0; i < size; i++) {
				new_env[i] = strdup(environ[i]);
			}
			envp = new_env;

			rb_hash_foreach(env, each_env_i, (VALUE)envp);
		}
	}

	cargv[argc] = NULL;
	for (i = 0; i < argc; i++)
		cargv[i] = StringValuePtr(RARRAY_PTR(argv)[i]);

	posix_spawn_file_actions_init(&fops);
	fastspawn_file_actions_addclose(&fops, options);
	posix_spawn_file_actions_addopen(&fops, 2, "/dev/null", O_WRONLY, 0);

	posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_USEVFORK
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_USEVFORK);
#endif

	ret = posix_spawnp(&pid, cargv[0], &fops, &attr, cargv, envp ? envp : environ);

	posix_spawn_file_actions_destroy(&fops);
	posix_spawnattr_destroy(&attr);
	if (envp) {
		char **ep = envp;
		while (*ep != NULL) free(*ep), ++ep;
		free(envp);
	}

	if (ret != 0) {
		errno = ret;
		rb_sys_fail("posix_spawnp");
	}

	return INT2FIX(pid);
}

void
Init_fastspawn()
{
	rb_mFastSpawn = rb_define_module("FastSpawn");
	rb_define_method(rb_mFastSpawn, "_vspawn", rb_fastspawn_vspawn, 3);
	rb_define_method(rb_mFastSpawn, "_pspawn", rb_fastspawn_pspawn, 3);
}

/* vim: set noexpandtab sts=0 ts=4 sw=4: */
