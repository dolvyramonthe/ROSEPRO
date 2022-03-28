/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 2021 Todd C. Miller <Todd.Miller@sudo.ws>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This is an open source non-commercial project. Dear PVS-Studio, please check it.
 * PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# include "compat/stdbool.h"
#endif /* HAVE_STDBOOL_H */
#if defined(HAVE_SHL_LOAD)
# include <dl.h>
#elif defined(HAVE_DLOPEN)
# include <dlfcn.h>
#endif

#include "sudo_compat.h"
#include "sudo_debug.h"
#include "sudo_util.h"
#include "pathnames.h"

/* execl flavors */
#define SUDO_EXECL	0x0
#define SUDO_EXECLE	0x1
#define SUDO_EXECLP	0x2

extern char **environ;
extern bool command_allowed(const char *cmnd, char * const argv[], char * const envp[], char **ncmnd, char ***nargv, char ***nenvp);

typedef int (*sudo_fn_execve_t)(const char *, char *const *, char *const *);

/*
 * We do PATH resolution here rather than in the policy because we
 * want to use the PATH in the current environment.
 */
static bool
resolve_path(const char *cmnd, char *out_cmnd, size_t out_size)
{
    struct stat sb;
    int errval = ENOENT;
    char path[PATH_MAX];
    char **p, *cp, *endp;
    int dirlen, len;

    for (p = environ; (cp = *p) != NULL; p++) {
	if (strncmp(cp, "PATH=", sizeof("PATH=") - 1) == 0) {
	    cp += sizeof("PATH=") - 1;
	    break;
	}
    }
    if (cp == NULL) {
	errno = ENOENT;
	return false;
    }

    endp = cp + strlen(cp);
    while (cp < endp) {
	char *colon = strchr(cp, ':');
	dirlen = colon ? (colon - cp) : (endp - cp);
	if (dirlen == 0) {
	    /* empty PATH component is the same as "." */
	    len = snprintf(path, sizeof(path), "./%s", cmnd);
	} else {
	    len = snprintf(path, sizeof(path), "%.*s/%s", dirlen, cp, cmnd);
	}
	cp = colon ? colon + 1 : endp;
	if (len >= ssizeof(path)) {
	    /* skip too long path */
	    errval = ENAMETOOLONG;
	    continue;
	}

	if (stat(path, &sb) == 0) {
	    if (strlcpy(out_cmnd, path, out_size) >= out_size) {
		errval = ENAMETOOLONG;
		break;
	    }
	    return true;
	}
	switch (errno) {
	case EACCES:
	    errval = EACCES;
	    break;
	case ELOOP:
	case ENOTDIR:
	case ENOENT:
	    break;
	default:
	    return false;
	}
    }
    errno = errval;
    return false;
}

static int
exec_wrapper(const char *cmnd, char * const argv[], char * const envp[],
    bool is_execvp)
{
    char *ncmnd = NULL, **nargv = NULL, **nenvp = NULL;
    char cmnd_buf[PATH_MAX];
    void *fn = NULL;
    debug_decl(exec_wrapper, SUDO_DEBUG_EXEC);

    /* Only check PATH for the command for execlp/execvp/execvpe. */
    if (strchr(cmnd, '/') == NULL) {
	if (!is_execvp) {
	    errno = ENOENT;
	    debug_return_int(-1);
	}
	if (!resolve_path(cmnd, cmnd_buf, sizeof(cmnd_buf))) {
	    debug_return_int(-1);
	}
	cmnd = cmnd_buf;
    }

# if defined(HAVE___INTERPOSE)
    fn = execve;
# elif defined(HAVE_DLOPEN)
    fn = dlsym(RTLD_NEXT, "execve");
# elif defined(HAVE_SHL_LOAD)
    fn = sudo_shl_get_next("execve", TYPE_PROCEDURE);
# endif
    if (fn == NULL) {
        errno = EACCES;
        debug_return_int(-1);
    }

    if (command_allowed(cmnd, argv, envp, &ncmnd, &nargv, &nenvp)) {
	/* Execute the command using the "real" execve() function. */
	((sudo_fn_execve_t)fn)(ncmnd, nargv, nenvp);

	/* Fall back to exec via shell for execvp and friends. */
	if (errno == ENOEXEC && is_execvp) {
	    int argc;
	    char **shargv;

	    for (argc = 0; argv[argc] != NULL; argc++)
		continue;
	    shargv = reallocarray(NULL, (argc + 2), sizeof(char *));
	    if (shargv == NULL)
		return -1;
	    shargv[0] = "sh";
	    shargv[1] = ncmnd;
	    memcpy(shargv + 2, nargv + 1, argc * sizeof(char *));
	    ((sudo_fn_execve_t)fn)(_PATH_SUDO_BSHELL, shargv, nenvp);
	    free(shargv);
	}
    } else {
	errno = EACCES;
    }
    if (ncmnd != cmnd)
	free(ncmnd);
    if (nargv != argv)
	free(nargv);
    if (nenvp != envp)
	free(nenvp);

    debug_return_int(-1);
}

static int
execl_wrapper(int type, const char *name, const char *arg, va_list ap)
{
    char **argv, **envp = environ;
    int argc = 1;
    va_list ap2;
    debug_decl(execl_wrapper, SUDO_DEBUG_EXEC);

    va_copy(ap2, ap);
    while (va_arg(ap2, char *) != NULL)
	argc++;
    va_end(ap2);
    argv = reallocarray(NULL, (argc + 1), sizeof(char *));
    if (argv == NULL)
	debug_return_int(-1);

    argc = 0;
    argv[argc++] = (char *)arg;
    while ((argv[argc] = va_arg(ap, char *)) != NULL)
	argc++;
    if (type == SUDO_EXECLE)
	envp = va_arg(ap, char **);

    exec_wrapper(name, argv, envp, type == SUDO_EXECLP);
    free(argv);

    debug_return_int(-1);
}

#ifdef HAVE___INTERPOSE
/*
 * Mac OS X 10.4 and above has support for library symbol interposition.
 * There is a good explanation of this in the Mac OS X Internals book.
 */
typedef struct interpose_s {
    void *new_func;
    void *orig_func;
} interpose_t;

static int
my_execve(const char *cmnd, char * const argv[], char * const envp[])
{
    return exec_wrapper(cmnd, argv, environ, false);
}

static int
my_execv(const char *cmnd, char * const argv[])
{
    return execve(cmnd, argv, environ);
}

#ifdef HAVE_EXECVPE
static int
my_execvpe(const char *cmnd, char * const argv[], char * const envp[])
{
    return exec_wrapper(cmnd, argv, envp, true);
}
#endif

static int
my_execvp(const char *cmnd, char * const argv[])
{
    return exec_wrapper(cmnd, argv, environ, true);
}

static int
my_execl(const char *name, const char *arg, ...)
{
    va_list ap;

    va_start(ap, arg);
    execl_wrapper(SUDO_EXECL, name, arg, ap);
    va_end(ap);

    return -1;
}

static int
my_execle(const char *name, const char *arg, ...)
{
    va_list ap;

    va_start(ap, arg);
    execl_wrapper(SUDO_EXECLE, name, arg, ap);
    va_end(ap);

    return -1;
}

static int
my_execlp(const char *name, const char *arg, ...)
{
    va_list ap;

    va_start(ap, arg);
    execl_wrapper(SUDO_EXECLP, name, arg, ap);
    va_end(ap);

    return -1;
}

/* Magic to tell dyld to do symbol interposition. */
__attribute__((__used__)) static const interpose_t interposers[]
__attribute__((__section__("__DATA,__interpose"))) = {
    { (void *)my_execl, (void *)execl },
    { (void *)my_execle, (void *)execle },
    { (void *)my_execlp, (void *)execlp },
    { (void *)my_execv, (void *)execv },
    { (void *)my_execve, (void *)execve },
    { (void *)my_execvp, (void *)execvp },
#ifdef HAVE_EXECVPE
    { (void *)my_execvpe, (void *)execvpe }
#endif
};

#else /* HAVE___INTERPOSE */

# if defined(HAVE_SHL_LOAD)
static void *
sudo_shl_get_next(const char *symbol, short type)
{
    const char *name, *myname;
    struct shl_descriptor *desc;
    void *fn = NULL;
    int idx = 0;
    debug_decl(sudo_shl_get_next, SUDO_DEBUG_EXEC);

    /* Search for symbol but skip this shared object. */
    /* XXX - could be set to a different path in sudo.conf */
    myname = sudo_basename(_PATH_SUDO_INTERCEPT);
    while (shl_get(idx++, &desc) == 0) {
        name = sudo_basename(desc->filename);
        if (strcmp(name, myname) == 0)
            continue;
        if (shl_findsym(&desc->handle, symbol, type, &fn) == 0)
            break;
    }

    debug_return_ptr(fn);
}
# endif /* HAVE_SHL_LOAD */

sudo_dso_public int
execve(const char *cmnd, char * const argv[], char * const envp[])
{
    return exec_wrapper(cmnd, argv, environ, false);
}

sudo_dso_public int
execv(const char *cmnd, char * const argv[])
{
    return execve(cmnd, argv, environ);
}

#ifdef HAVE_EXECVPE
sudo_dso_public int
execvpe(const char *cmnd, char * const argv[], char * const envp[])
{
    return exec_wrapper(cmnd, argv, envp, true);
}
#endif

sudo_dso_public int
execvp(const char *cmnd, char * const argv[])
{
    return exec_wrapper(cmnd, argv, environ, true);
}

sudo_dso_public int
execl(const char *name, const char *arg, ...)
{
    va_list ap;

    va_start(ap, arg);
    execl_wrapper(SUDO_EXECL, name, arg, ap);
    va_end(ap);

    return -1;
}

sudo_dso_public int
execle(const char *name, const char *arg, ...)
{
    va_list ap;

    va_start(ap, arg);
    execl_wrapper(SUDO_EXECLE, name, arg, ap);
    va_end(ap);

    return -1;
}

sudo_dso_public int
execlp(const char *name, const char *arg, ...)
{
    va_list ap;

    va_start(ap, arg);
    execl_wrapper(SUDO_EXECLP, name, arg, ap);
    va_end(ap);

    return -1;
}
#endif /* HAVE___INTERPOSE) */
