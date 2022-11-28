/* exechelp.c - Fork and exec helpers for POSIX
 * Copyright (C) 2004, 2007-2009, 2010 Free Software Foundation, Inc.
 * Copyright (C) 2004, 2006-2012, 2014-2017 g10 Code GmbH
 *
 * This file is part of GnuPG.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of either
 *
 *   - the GNU Lesser General Public License as published by the Free
 *     Software Foundation; either version 3 of the License, or (at
 *     your option) any later version.
 *
 * or
 *
 *   - the GNU General Public License as published by the Free
 *     Software Foundation; either version 2 of the License, or (at
 *     your option) any later version.
 *
 * or both in parallel, as here.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 * SPDX-License-Identifier: (LGPL-3.0+ OR GPL-2.0+)
 */

#include <config.h>

#if defined(HAVE_W32_SYSTEM)
#error This code is only used on POSIX
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif
#include <unistd.h>
#include <fcntl.h>

#ifdef WITHOUT_NPTH /* Give the Makefile a chance to build without Pth.  */
#undef HAVE_NPTH
#undef USE_NPTH
#endif

#ifdef HAVE_NPTH
#include <npth.h>
#endif
#include <sys/wait.h>

#ifdef HAVE_GETRLIMIT
#include <sys/time.h>
#include <sys/resource.h>
#endif /*HAVE_GETRLIMIT*/

#ifdef HAVE_STAT
# include <sys/stat.h>
#endif

#if __linux__
# include <sys/types.h>
# include <dirent.h>
#endif /*__linux__ */

#include "util.h"
#include "i18n.h"
#include "sysutils.h"
#include "exechelp.h"


/* Helper */
static inline gpg_error_t
my_error_from_syserror (void)
{
  return gpg_err_make (default_errsource, gpg_err_code_from_syserror ());
}

static inline gpg_error_t
my_error (int errcode)
{
  return gpg_err_make (default_errsource, errcode);
}


/* Return the maximum number of currently allowed open file
   descriptors.  Only useful on POSIX systems but returns a value on
   other systems too.  */
int
get_max_fds (void)
{
  int max_fds = -1;
#ifdef HAVE_GETRLIMIT
  struct rlimit rl;

  /* Under Linux we can figure out the highest used file descriptor by
   * reading /proc/PID/fd.  This is in the common cases much fast than
   * for example doing 4096 close calls where almost all of them will
   * fail.  On a system with a limit of 4096 files and only 8 files
   * open with the highest number being 10, we speedup close_all_fds
   * from 125ms to 0.4ms including readdir.
   *
   * Another option would be to close the file descriptors as returned
   * from reading that directory - however then we need to snapshot
   * that list before starting to close them.  */
#ifdef __linux__
  {
    DIR *dir = NULL;
    struct dirent *dir_entry;
    const char *s;
    int x;

    dir = opendir ("/proc/self/fd");
    if (dir)
      {
        while ((dir_entry = readdir (dir)))
          {
            s = dir_entry->d_name;
            if ( *s < '0' || *s > '9')
              continue;
            x = atoi (s);
            if (x > max_fds)
              max_fds = x;
          }
        closedir (dir);
      }
    if (max_fds != -1)
      return max_fds + 1;
    }
#endif /* __linux__ */


# ifdef RLIMIT_NOFILE
  if (!getrlimit (RLIMIT_NOFILE, &rl))
    max_fds = rl.rlim_max;
# endif

# ifdef RLIMIT_OFILE
  if (max_fds == -1 && !getrlimit (RLIMIT_OFILE, &rl))
    max_fds = rl.rlim_max;

# endif
#endif /*HAVE_GETRLIMIT*/

#ifdef _SC_OPEN_MAX
  if (max_fds == -1)
    {
      long int scres = sysconf (_SC_OPEN_MAX);
      if (scres >= 0)
        max_fds = scres;
    }
#endif

#ifdef _POSIX_OPEN_MAX
  if (max_fds == -1)
    max_fds = _POSIX_OPEN_MAX;
#endif

#ifdef OPEN_MAX
  if (max_fds == -1)
    max_fds = OPEN_MAX;
#endif

  if (max_fds == -1)
    max_fds = 256;  /* Arbitrary limit.  */

  /* AIX returns INT32_MAX instead of a proper value.  We assume that
     this is always an error and use an arbitrary limit.  */
#ifdef INT32_MAX
  if (max_fds == INT32_MAX)
    max_fds = 256;
#endif

  return max_fds;
}


/* Close all file descriptors starting with descriptor FIRST.  If
   EXCEPT is not NULL, it is expected to be a list of file descriptors
   which shall not be closed.  This list shall be sorted in ascending
   order with the end marked by -1.  */
void
close_all_fds (int first, int *except)
{
  int max_fd = get_max_fds ();
  int fd, i, except_start;

  if (except)
    {
      except_start = 0;
      for (fd=first; fd < max_fd; fd++)
        {
          for (i=except_start; except[i] != -1; i++)
            {
              if (except[i] == fd)
                {
                  /* If we found the descriptor in the exception list
                     we can start the next compare run at the next
                     index because the exception list is ordered.  */
                except_start = i + 1;
                break;
                }
            }
          if (except[i] == -1)
            close (fd);
        }
    }
  else
    {
      for (fd=first; fd < max_fd; fd++)
        close (fd);
    }

  gpg_err_set_errno (0);
}


/* Returns an array with all currently open file descriptors.  The end
   of the array is marked by -1.  The caller needs to release this
   array using the *standard free* and not with xfree.  This allow the
   use of this function right at startup even before libgcrypt has
   been initialized.  Returns NULL on error and sets ERRNO
   accordingly.  */
int *
get_all_open_fds (void)
{
  int *array;
  size_t narray;
  int fd, max_fd, idx;
#ifndef HAVE_STAT
  array = calloc (1, sizeof *array);
  if (array)
    array[0] = -1;
#else /*HAVE_STAT*/
  struct stat statbuf;

  max_fd = get_max_fds ();
  narray = 32;  /* If you change this change also t-exechelp.c.  */
  array = calloc (narray, sizeof *array);
  if (!array)
    return NULL;

  /* Note:  The list we return is ordered.  */
  for (idx=0, fd=0; fd < max_fd; fd++)
    if (!(fstat (fd, &statbuf) == -1 && errno == EBADF))
      {
        if (idx+1 >= narray)
          {
            int *tmp;

            narray += (narray < 256)? 32:256;
            tmp = realloc (array, narray * sizeof *array);
            if (!tmp)
              {
                free (array);
                return NULL;
              }
            array = tmp;
          }
        array[idx++] = fd;
      }
  array[idx] = -1;
#endif /*HAVE_STAT*/
  return array;
}


/* The exec core used right after the fork. This will never return. */
static void
do_exec (const char *pgmname, const char *argv[],
         int fd_in, int fd_out, int fd_err,
         int *except, unsigned int flags)
{
  char **arg_list;
  int i, j;
  int fds[3];
  int nodevnull[3];

  fds[0] = fd_in;
  fds[1] = fd_out;
  fds[2] = fd_err;

  nodevnull[0] = !!(flags & GNUPG_SPAWN_KEEP_STDIN);
  nodevnull[1] = !!(flags & GNUPG_SPAWN_KEEP_STDOUT);
  nodevnull[2] = !!(flags & GNUPG_SPAWN_KEEP_STDERR);

  /* Create the command line argument array.  */
  i = 0;
  if (argv)
    while (argv[i])
      i++;
  arg_list = xcalloc (i+2, sizeof *arg_list);
  arg_list[0] = strrchr (pgmname, '/');
  if (arg_list[0])
    arg_list[0]++;
  else
    arg_list[0] = xstrdup (pgmname);
  if (argv)
    for (i=0,j=1; argv[i]; i++, j++)
      arg_list[j] = (char*)argv[i];

  /* Assign /dev/null to unused FDs. */
  for (i=0; i <= 2; i++)
    {
      if (nodevnull[i])
        continue;
      if (fds[i] == -1)
        {
          fds[i] = open ("/dev/null", i? O_WRONLY : O_RDONLY);
          if (fds[i] == -1)
            log_fatal ("failed to open '%s': %s\n",
                       "/dev/null", strerror (errno));
        }
    }

  /* Connect the standard files.  */
  for (i=0; i <= 2; i++)
    {
      if (nodevnull[i])
        continue;
      if (fds[i] != i && dup2 (fds[i], i) == -1)
        log_fatal ("dup2 std%s failed: %s\n",
                   i==0?"in":i==1?"out":"err", strerror (errno));
    }

  /* Close all other files. */
  close_all_fds (3, except);

  execv (pgmname, arg_list);
  /* No way to print anything, as we have closed all streams. */
  _exit (127);
}


static gpg_error_t
do_create_pipe (int filedes[2])
{
  gpg_error_t err = 0;

  if (pipe (filedes) == -1)
    {
      err = my_error_from_syserror ();
      filedes[0] = filedes[1] = -1;
    }

  return err;
}


static gpg_error_t
create_pipe_and_estream (int filedes[2], estream_t *r_fp,
                         int outbound, int nonblock)
{
  gpg_error_t err;

  if (pipe (filedes) == -1)
    {
      err = my_error_from_syserror ();
      log_error (_("error creating a pipe: %s\n"), gpg_strerror (err));
      filedes[0] = filedes[1] = -1;
      *r_fp = NULL;
      return err;
    }

  if (!outbound)
    *r_fp = es_fdopen (filedes[0], nonblock? "r,nonblock" : "r");
  else
    *r_fp = es_fdopen (filedes[1], nonblock? "w,nonblock" : "w");
  if (!*r_fp)
    {
      err = my_error_from_syserror ();
      log_error (_("error creating a stream for a pipe: %s\n"),
                 gpg_strerror (err));
      close (filedes[0]);
      close (filedes[1]);
      filedes[0] = filedes[1] = -1;
      return err;
    }
  return 0;
}


/* Portable function to create a pipe.  Under Windows the write end is
   inheritable.  If R_FP is not NULL, an estream is created for the
   read end and stored at R_FP.  */
gpg_error_t
gnupg_create_inbound_pipe (int filedes[2], estream_t *r_fp, int nonblock)
{
  if (r_fp)
    return create_pipe_and_estream (filedes, r_fp, 0, nonblock);
  else
    return do_create_pipe (filedes);
}


/* Portable function to create a pipe.  Under Windows the read end is
   inheritable.  If R_FP is not NULL, an estream is created for the
   write end and stored at R_FP.  */
gpg_error_t
gnupg_create_outbound_pipe (int filedes[2], estream_t *r_fp, int nonblock)
{
  if (r_fp)
    return create_pipe_and_estream (filedes, r_fp, 1, nonblock);
  else
    return do_create_pipe (filedes);
}


/* Portable function to create a pipe.  Under Windows both ends are
   inheritable.  */
gpg_error_t
gnupg_create_pipe (int filedes[2])
{
  return do_create_pipe (filedes);
}


/* Close the end of a pipe.  */
void
gnupg_close_pipe (int fd)
{
  if (fd != -1)
    close (fd);
}


/* Fork and exec the PGMNAME, see exechelp.h for details.  */
gpg_error_t
gnupg_spawn_process (const char *pgmname, const char *argv[],
                     int *except, unsigned int flags,
                     estream_t *r_infp,
                     estream_t *r_outfp,
                     estream_t *r_errfp,
                     pid_t *pid)
{
  gpg_error_t err;
  int inpipe[2] = {-1, -1};
  int outpipe[2] = {-1, -1};
  int errpipe[2] = {-1, -1};
  estream_t infp = NULL;
  estream_t outfp = NULL;
  estream_t errfp = NULL;
  int nonblock = !!(flags & GNUPG_SPAWN_NONBLOCK);

  if (r_infp)
    *r_infp = NULL;
  if (r_outfp)
    *r_outfp = NULL;
  if (r_errfp)
    *r_errfp = NULL;
  *pid = (pid_t)(-1); /* Always required.  */

  if (r_infp)
    {
      err = create_pipe_and_estream (inpipe, &infp, 1, nonblock);
      if (err)
        return err;
    }

  if (r_outfp)
    {
      err = create_pipe_and_estream (outpipe, &outfp, 0, nonblock);
      if (err)
        {
          if (infp)
            es_fclose (infp);
          else if (inpipe[1] != -1)
            close (inpipe[1]);
          if (inpipe[0] != -1)
            close (inpipe[0]);

          return err;
        }
    }

  if (r_errfp)
    {
      err = create_pipe_and_estream (errpipe, &errfp, 0, nonblock);
      if (err)
        {
          if (infp)
            es_fclose (infp);
          else if (inpipe[1] != -1)
            close (inpipe[1]);
          if (inpipe[0] != -1)
            close (inpipe[0]);

          if (outfp)
            es_fclose (outfp);
          else if (outpipe[0] != -1)
            close (outpipe[0]);
          if (outpipe[1] != -1)
            close (outpipe[1]);

          return err;
        }
    }


  *pid = fork ();
  if (*pid == (pid_t)(-1))
    {
      err = my_error_from_syserror ();
      log_error (_("error forking process: %s\n"), gpg_strerror (err));

      if (infp)
        es_fclose (infp);
      else if (inpipe[1] != -1)
        close (inpipe[1]);
      if (inpipe[0] != -1)
        close (inpipe[0]);

      if (outfp)
        es_fclose (outfp);
      else if (outpipe[0] != -1)
        close (outpipe[0]);
      if (outpipe[1] != -1)
        close (outpipe[1]);

      if (errfp)
        es_fclose (errfp);
      else if (errpipe[0] != -1)
        close (errpipe[0]);
      if (errpipe[1] != -1)
        close (errpipe[1]);
      return err;
    }

  if (!*pid)
    {
      /* This is the child. */
      gcry_control (GCRYCTL_TERM_SECMEM);
      es_fclose (infp);
      es_fclose (outfp);
      es_fclose (errfp);
      do_exec (pgmname, argv, inpipe[0], outpipe[1], errpipe[1],
               except, flags);
      /*NOTREACHED*/
    }

  /* This is the parent. */
  if (inpipe[0] != -1)
    close (inpipe[0]);
  if (outpipe[1] != -1)
    close (outpipe[1]);
  if (errpipe[1] != -1)
    close (errpipe[1]);

  if (r_infp)
    *r_infp = infp;
  if (r_outfp)
    *r_outfp = outfp;
  if (r_errfp)
    *r_errfp = errfp;

  return 0;
}



/* Simplified version of gnupg_spawn_process.  This function forks and
   then execs PGMNAME, while connecting INFD to stdin, OUTFD to stdout
   and ERRFD to stderr (any of them may be -1 to connect them to
   /dev/null).  The arguments for the process are expected in the NULL
   terminated array ARGV.  The program name itself should not be
   included there.  Calling gnupg_wait_process is required.

   Returns 0 on success or an error code. */
gpg_error_t
gnupg_spawn_process_fd (const char *pgmname, const char *argv[],
                        int infd, int outfd, int errfd, pid_t *pid)
{
  gpg_error_t err;

  *pid = fork ();
  if (*pid == (pid_t)(-1))
    {
      err = my_error_from_syserror ();
      log_error (_("error forking process: %s\n"), strerror (errno));
      return err;
    }

  if (!*pid)
    {
      gcry_control (GCRYCTL_TERM_SECMEM);
      /* Run child. */
      do_exec (pgmname, argv, infd, outfd, errfd, NULL, 0);
      /*NOTREACHED*/
    }

  return 0;
}




/* Waiting for child processes.

   waitpid(2) may return information about terminated children that we
   did not yet request, and there is no portable way to wait for a
   specific set of children.

   As a workaround, we store the results of children for later use.

   XXX: This assumes that PIDs are not reused too quickly.  */

struct terminated_child
{
  pid_t pid;
  int exitcode;
  struct terminated_child *next;
};

struct terminated_child *terminated_children;


static gpg_error_t
store_result (pid_t pid, int exitcode)
{
  struct terminated_child *c;

  c = xtrymalloc (sizeof *c);
  if (c == NULL)
    return gpg_err_code_from_syserror ();

  c->pid = pid;
  c->exitcode = exitcode;
  c->next = terminated_children;
  terminated_children = c;

  return 0;
}


static int
get_result (pid_t pid, int *r_exitcode)
{
  struct terminated_child *c, **prevp;

  for (prevp = &terminated_children, c = terminated_children;
       c;
       prevp = &c->next, c = c->next)
    if (c->pid == pid)
      {
        *prevp = c->next;
        *r_exitcode = c->exitcode;
        xfree (c);
        return 1;
      }

  return 0;
}


/* See exechelp.h for a description.  */
gpg_error_t
gnupg_wait_process (const char *pgmname, pid_t pid, int hang, int *r_exitcode)
{
  gpg_err_code_t ec;
  int i, status;

  if (r_exitcode)
    *r_exitcode = -1;

  if (pid == (pid_t)(-1))
    return gpg_error (GPG_ERR_INV_VALUE);

#ifdef USE_NPTH
  i = npth_waitpid (pid, &status, hang? 0:WNOHANG);
#else
  while ((i=waitpid (pid, &status, hang? 0:WNOHANG)) == (pid_t)(-1)
	 && errno == EINTR);
#endif

  if (i == (pid_t)(-1))
    {
      ec = gpg_err_code_from_errno (errno);
      log_error (_("waiting for process %d to terminate failed: %s\n"),
                 (int)pid, strerror (errno));
    }
  else if (!i)
    {
      ec = GPG_ERR_TIMEOUT; /* Still running.  */
    }
  else if (WIFEXITED (status) && WEXITSTATUS (status) == 127)
    {
      log_error (_("error running '%s': probably not installed\n"), pgmname);
      ec = GPG_ERR_CONFIGURATION;
    }
  else if (WIFEXITED (status) && WEXITSTATUS (status))
    {
      if (!r_exitcode)
        log_error (_("error running '%s': exit status %d\n"), pgmname,
                   WEXITSTATUS (status));
      else
        *r_exitcode = WEXITSTATUS (status);
      ec = GPG_ERR_GENERAL;
    }
  else if (!WIFEXITED (status))
    {
      log_error (_("error running '%s': terminated\n"), pgmname);
      ec = GPG_ERR_GENERAL;
    }
  else
    {
      if (r_exitcode)
        *r_exitcode = 0;
      ec = 0;
    }

  return gpg_err_make (GPG_ERR_SOURCE_DEFAULT, ec);
}

/* See exechelp.h for a description.  */
gpg_error_t
gnupg_wait_processes (const char **pgmnames, pid_t *pids, size_t count,
                      int hang, int *r_exitcodes)
{
  gpg_err_code_t ec = 0;
  size_t i, left;
  int *dummy = NULL;

  if (r_exitcodes == NULL)
    {
      dummy = r_exitcodes = xtrymalloc (sizeof *r_exitcodes * count);
      if (dummy == NULL)
        return gpg_err_code_from_syserror ();
    }

  for (i = 0, left = count; i < count; i++)
    {
      int status = -1;

      /* Skip invalid PID.  */
      if (pids[i] == (pid_t)(-1))
        {
          r_exitcodes[i] = -1;
          left -= 1;
          continue;
        }

      /* See if there was a previously stored result for this pid.  */
      if (get_result (pids[i], &status))
        left -= 1;

      r_exitcodes[i] = status;
    }

  while (left > 0)
    {
      pid_t pid;
      int status;

#ifdef USE_NPTH
      pid = npth_waitpid (-1, &status, hang ? 0 : WNOHANG);
#else
      while ((pid = waitpid (-1, &status, hang ? 0 : WNOHANG)) == (pid_t)(-1)
             && errno == EINTR);
#endif

      if (pid == (pid_t)(-1))
        {
          ec = gpg_err_code_from_errno (errno);
          log_error (_("waiting for processes to terminate failed: %s\n"),
                     strerror (errno));
          break;
        }
      else if (!pid)
        {
          ec = GPG_ERR_TIMEOUT; /* Still running.  */
          break;
        }
      else
        {
          for (i = 0; i < count; i++)
            if (pid == pids[i])
              break;

          if (i == count)
            {
              /* No match, store this result.  */
              ec = store_result (pid, status);
              if (ec)
                break;
              continue;
            }

          /* Process PIDS[i] died.  */
          if (r_exitcodes[i] != (pid_t) -1)
            {
              log_error ("PID %d was reused", pid);
              ec = GPG_ERR_GENERAL;
              break;
            }

          left -= 1;
          r_exitcodes[i] = status;
        }
    }

  for (i = 0; i < count; i++)
    {
      if (r_exitcodes[i] == -1)
        continue;

      if (WIFEXITED (r_exitcodes[i]) && WEXITSTATUS (r_exitcodes[i]) == 127)
        {
          log_error (_("error running '%s': probably not installed\n"),
                     pgmnames[i]);
          ec = GPG_ERR_CONFIGURATION;
        }
      else if (WIFEXITED (r_exitcodes[i]) && WEXITSTATUS (r_exitcodes[i]))
        {
          if (dummy)
            log_error (_("error running '%s': exit status %d\n"),
                       pgmnames[i], WEXITSTATUS (r_exitcodes[i]));
          else
            r_exitcodes[i] = WEXITSTATUS (r_exitcodes[i]);
          ec = GPG_ERR_GENERAL;
        }
      else if (!WIFEXITED (r_exitcodes[i]))
        {
          log_error (_("error running '%s': terminated\n"), pgmnames[i]);
          ec = GPG_ERR_GENERAL;
        }
    }

  xfree (dummy);
  return gpg_err_make (GPG_ERR_SOURCE_DEFAULT, ec);
}



void
gnupg_release_process (pid_t pid)
{
  (void)pid;
}


/* Spawn a new process and immediately detach from it.  The name of
   the program to exec is PGMNAME and its arguments are in ARGV (the
   programname is automatically passed as first argument).
   Environment strings in ENVP are set.  An error is returned if
   pgmname is not executable; to make this work it is necessary to
   provide an absolute file name.  All standard file descriptors are
   connected to /dev/null. */
gpg_error_t
gnupg_spawn_process_detached (const char *pgmname, const char *argv[],
                              const char *envp[] )
{
  gpg_err_code_t ec;
  pid_t pid;
  int i;

  if (getuid() != geteuid())
    return my_error (GPG_ERR_BUG);

  if ((ec = gnupg_access (pgmname, X_OK)))
    return gpg_err_make (default_errsource, ec);

  pid = fork ();
  if (pid == (pid_t)(-1))
    {
      log_error (_("error forking process: %s\n"), strerror (errno));
      return my_error_from_syserror ();
    }
  if (!pid)
    {
      pid_t pid2;

      gcry_control (GCRYCTL_TERM_SECMEM);
      if (setsid() == -1 || chdir ("/"))
        _exit (1);

      pid2 = fork (); /* Double fork to let init take over the new child. */
      if (pid2 == (pid_t)(-1))
        _exit (1);
      if (pid2)
        _exit (0);  /* Let the parent exit immediately. */

      if (envp)
        for (i=0; envp[i]; i++)
          putenv (xstrdup (envp[i]));

      do_exec (pgmname, argv, -1, -1, -1, NULL, 0);

      /*NOTREACHED*/
    }

  if (waitpid (pid, NULL, 0) == -1)
    log_error ("waitpid failed in gnupg_spawn_process_detached: %s",
               strerror (errno));

  return 0;
}


/* Kill a process; that is send an appropriate signal to the process.
   gnupg_wait_process must be called to actually remove the process
   from the system.  An invalid PID is ignored.  */
void
gnupg_kill_process (pid_t pid)
{
  if (pid != (pid_t)(-1))
    {
      kill (pid, SIGTERM);
    }
}

#include <sys/socket.h>

struct gnupg_process {
  const char *pgmname;
  unsigned int terminated   :1; /* or detached */
  unsigned int flags;
  pid_t pid;
  int fd_in;
  int fd_out;
  int fd_err;
  int wstatus;
};

static int gnupg_process_syscall_func_initialized;

/* Functions called before and after blocking syscalls.  */
static void (*pre_syscall_func) (void);
static void (*post_syscall_func) (void);

static void
check_syscall_func (void)
{
  if (!gnupg_process_syscall_func_initialized)
    {
      gpgrt_get_syscall_clamp (&pre_syscall_func, &post_syscall_func);
      gnupg_process_syscall_func_initialized = 1;
    }
}

static void
pre_syscall (void)
{
  if (pre_syscall_func)
    pre_syscall_func ();
}

static void
post_syscall (void)
{
  if (post_syscall_func)
    post_syscall_func ();
}


static gpg_err_code_t
do_create_socketpair (int filedes[2])
{
  gpg_error_t err = 0;

  pre_syscall ();
  if (socketpair (AF_LOCAL, SOCK_STREAM, 0, filedes) == -1)
    {
      err = gpg_err_code_from_syserror ();
      filedes[0] = filedes[1] = -1;
    }
  post_syscall ();

  return err;
}

static int
posix_open_null (int for_write)
{
  int fd;

  fd = open ("/dev/null", for_write? O_WRONLY : O_RDONLY);
  if (fd == -1)
    log_fatal ("failed to open '/dev/null': %s\n", strerror (errno));
  return fd;
}

static void
my_exec (const char *pgmname, const char *argv[],
         int fd_in, int fd_out, int fd_err,
         void (*spawn_cb) (struct spawn_cb_arg *), void *spawn_cb_arg)
{
  int i;
  struct spawn_cb_arg sca;

  sca.fds[0] = fd_in;
  sca.fds[1] = fd_out;
  sca.fds[2] = fd_err;
  sca.except_fds = NULL;
  sca.arg = spawn_cb_arg;
  if (spawn_cb)
    (*spawn_cb) (&sca);

  /* Assign /dev/null to unused FDs.  */
  for (i = 0; i <= 2; i++)
    if (sca.fds[i] == -1)
      sca.fds[i] = posix_open_null (i);

  /* Connect the standard files.  */
  for (i = 0; i <= 2; i++)
    if (sca.fds[i] != i)
      {
        if (dup2 (sca.fds[i], i) == -1)
          log_fatal ("dup2 std%s failed: %s\n",
                     i==0?"in":i==1?"out":"err", strerror (errno));
        /*
         * We don't close sca.fds[i] here, but close them by
         * close_all_fds.  Note that there may be same one in three of
         * sca.fds[i].
         */
      }

  /* Close all other files.  */
  close_all_fds (3, sca.except_fds);

  execv (pgmname, (char *const *)argv);
  /* No way to print anything, as we have may have closed all streams. */
  _exit (127);
}

static gpg_err_code_t
spawn_detached (gnupg_process_t process,
                const char *pgmname, const char *argv[],
                void (*spawn_cb) (struct spawn_cb_arg *), void *spawn_cb_arg)
{
  gpg_err_code_t ec;
  pid_t pid;

  /* FIXME: Is this GnuPG specific or should we keep it.  */
  if (getuid() != geteuid())
    {
      xfree (process);
      xfree (argv);
      return GPG_ERR_BUG;
    }

  if (access (pgmname, X_OK))
    {
      ec = gpg_err_code_from_syserror ();
      xfree (process);
      xfree (argv);
      return ec;
    }

  pre_syscall ();
  pid = fork ();
  post_syscall ();
  if (pid == (pid_t)(-1))
    {
      ec = gpg_err_code_from_syserror ();
      log_error (_("error forking process: %s\n"), gpg_strerror (ec));
      xfree (process);
      xfree (argv);
      return ec;
    }

  if (!pid)
    {
      pid_t pid2;

      if (setsid() == -1 || chdir ("/"))
        _exit (1);

      pid2 = fork (); /* Double fork to let init take over the new child. */
      if (pid2 == (pid_t)(-1))
        _exit (1);
      if (pid2)
        _exit (0);  /* Let the parent exit immediately. */

      my_exec (pgmname, argv, -1, -1, -1, spawn_cb, spawn_cb_arg);
      /*NOTREACHED*/
    }

  pre_syscall ();
  if (waitpid (pid, NULL, 0) == -1)
    {
      post_syscall ();
      ec = gpg_err_code_from_syserror ();
      log_error ("waitpid failed in gpgrt_spawn_process_detached: %s",
                 gpg_strerror (ec));
      return ec;
    }
  else
    post_syscall ();

  process->pid = (pid_t)-1;
  process->fd_in = -1;
  process->fd_out = -1;
  process->fd_err = -1;
  process->wstatus = -1;
  process->terminated = 1;
  return 0;
}

gpg_err_code_t
gnupg_process_spawn (const char *pgmname, const char *argv1[],
                     unsigned int flags,
                     void (*spawn_cb) (struct spawn_cb_arg *),
                     void *spawn_cb_arg,
                     gnupg_process_t *r_process)
{
  gpg_err_code_t ec;
  gnupg_process_t process;
  int fd_in[2];
  int fd_out[2];
  int fd_err[2];
  pid_t pid;
  const char **argv;
  int i, j;

  check_syscall_func ();

  if (r_process)
    *r_process = NULL;

  /* Create the command line argument array.  */
  i = 0;
  if (argv1)
    while (argv1[i])
      i++;
  argv = xtrycalloc (i+2, sizeof *argv);
  if (!argv)
    return gpg_err_code_from_syserror ();
  argv[0] = strrchr (pgmname, '/');
  if (argv[0])
    argv[0]++;
  else
    argv[0] = pgmname;

  if (argv1)
    for (i=0, j=1; argv1[i]; i++, j++)
      argv[j] = argv1[i];

  process = xtrycalloc (1, sizeof (struct gnupg_process));
  if (process == NULL)
    {
      xfree (argv);
      return gpg_err_code_from_syserror ();
    }

  process->pgmname = pgmname;
  process->flags = flags;

  if ((flags & GNUPG_PROCESS_DETACHED))
    {
      if ((flags & GNUPG_PROCESS_STDFDS_SETTING))
        {
          xfree (process);
          xfree (argv);
          return GPG_ERR_INV_FLAG;
        }

      *r_process = process;
      return spawn_detached (process, pgmname, argv, spawn_cb, spawn_cb_arg);
    }

  if ((flags & GNUPG_PROCESS_STDINOUT_SOCKETPAIR))
    {
      ec = do_create_socketpair (fd_in);
      if (ec)
        {
          xfree (process);
          xfree (argv);
          return ec;
        }
      fd_out[0] = dup (fd_in[0]);
      fd_out[1] = dup (fd_in[1]);
    }
  else
    {
      if ((flags & GNUPG_PROCESS_STDIN_PIPE))
        {
          ec = do_create_pipe (fd_in);
          if (ec)
            {
              xfree (process);
              xfree (argv);
              return ec;
            }
        }
      else if ((flags & GNUPG_PROCESS_STDIN_NULL))
        {
          fd_in[0] = -1;
          fd_in[1] = -1;
        }
      else
        {
          fd_in[0] = 0;
          fd_in[1] = -1;
        }

      if ((flags & GNUPG_PROCESS_STDOUT_PIPE))
        {
          ec = do_create_pipe (fd_out);
          if (ec)
            {
              if (fd_in[0] >= 0 && fd_in[0] != 0)
                close (fd_in[0]);
              if (fd_in[1] >= 0)
                close (fd_in[1]);
              xfree (process);
              xfree (argv);
              return ec;
            }
        }
      else if ((flags & GNUPG_PROCESS_STDOUT_NULL))
        {
          fd_out[0] = -1;
          fd_out[1] = -1;
        }
      else
        {
          fd_out[0] = -1;
          fd_out[1] = 1;
        }
    }

  if ((flags & GNUPG_PROCESS_STDERR_PIPE))
    {
      ec = do_create_pipe (fd_err);
      if (ec)
        {
          if (fd_in[0] >= 0 && fd_in[0] != 0)
            close (fd_in[0]);
          if (fd_in[1] >= 0)
            close (fd_in[1]);
          if (fd_out[0] >= 0)
            close (fd_out[0]);
          if (fd_out[1] >= 0 && fd_out[1] != 1)
            close (fd_out[1]);
          xfree (process);
          xfree (argv);
          return ec;
        }
    }
  else if ((flags & GNUPG_PROCESS_STDERR_NULL))
    {
      fd_err[0] = -1;
      fd_err[1] = -1;
    }
  else
    {
      fd_err[0] = -1;
      fd_err[1] = 2;
    }

  pre_syscall ();
  pid = fork ();
  post_syscall ();
  if (pid == (pid_t)(-1))
    {
      ec = gpg_err_code_from_syserror ();
      log_error (_("error forking process: %s\n"), gpg_strerror (ec));
      if (fd_in[0] >= 0 && fd_in[0] != 0)
        close (fd_in[0]);
      if (fd_in[1] >= 0)
        close (fd_in[1]);
      if (fd_out[0] >= 0)
        close (fd_out[0]);
      if (fd_out[1] >= 0 && fd_out[1] != 1)
        close (fd_out[1]);
      if (fd_err[0] >= 0)
        close (fd_err[0]);
      if (fd_err[1] >= 0 && fd_err[1] != 2)
        close (fd_err[1]);
      xfree (process);
      xfree (argv);
      return ec;
    }

  if (!pid)
    {
      if (fd_in[1] >= 0)
        close (fd_in[1]);
      if (fd_out[0] >= 0)
        close (fd_out[0]);
      if (fd_err[0] >= 0)
        close (fd_err[0]);

      /* Run child. */
      my_exec (pgmname, argv, fd_in[0], fd_out[1], fd_err[1],
               spawn_cb, spawn_cb_arg);
      /*NOTREACHED*/
    }

  xfree (argv);
  process->pid = pid;

  if (fd_in[0] >= 0 && fd_in[0] != 0)
    close (fd_in[0]);
  if (fd_out[1] >= 0 && fd_out[1] != 1)
    close (fd_out[1]);
  if (fd_err[1] >= 0 && fd_err[1] != 2)
    close (fd_err[1]);
  process->fd_in = fd_in[1];
  process->fd_out = fd_out[0];
  process->fd_err = fd_err[0];
  process->wstatus = -1;
  process->terminated = 0;

  if (r_process == NULL)
    {
      ec = gnupg_process_wait (process, 1);
      gnupg_process_release (process);
      return ec;
    }

  *r_process = process;
  return 0;
}

static gpg_err_code_t
process_kill (gnupg_process_t process, int sig)
{
  gpg_err_code_t ec = 0;
  pid_t pid = process->pid;

  pre_syscall ();
  if (kill (pid, sig) < 0)
    ec = gpg_err_code_from_syserror ();
  post_syscall ();
  return ec;
}

gpg_err_code_t
gnupg_process_terminate (gnupg_process_t process)
{
  return process_kill (process, SIGTERM);
}

gpg_err_code_t
gnupg_process_get_fds (gnupg_process_t process, unsigned int flags,
                       int *r_fd_in, int *r_fd_out, int *r_fd_err)
{
  (void)flags;
  if (r_fd_in)
    {
      *r_fd_in = process->fd_in;
      process->fd_in = -1;
    }
  if (r_fd_out)
    {
      *r_fd_out = process->fd_out;
      process->fd_out = -1;
    }
  if (r_fd_err)
    {
      *r_fd_err = process->fd_err;
      process->fd_err = -1;
    }

  return 0;
}

gpg_err_code_t
gnupg_process_get_streams (gnupg_process_t process, unsigned int flags,
                           gpgrt_stream_t *r_fp_in, gpgrt_stream_t *r_fp_out,
                           gpgrt_stream_t *r_fp_err)
{
  int nonblock = (flags & GNUPG_PROCESS_STREAM_NONBLOCK)? 1: 0;

  if (r_fp_in)
    {
      *r_fp_in = es_fdopen (process->fd_in, nonblock? "w,nonblock" : "w");
      process->fd_in = -1;
    }
  if (r_fp_out)
    {
      *r_fp_out = es_fdopen (process->fd_out, nonblock? "r,nonblock" : "r");
      process->fd_out = -1;
    }
  if (r_fp_err)
    {
      *r_fp_err = es_fdopen (process->fd_err, nonblock? "r,nonblock" : "r");
      process->fd_err = -1;
    }
  return 0;
}

static gpg_err_code_t
process_vctl (gnupg_process_t process, unsigned int request, va_list arg_ptr)
{
  switch (request)
    {
    case GNUPG_PROCESS_NOP:
      return 0;

    case GNUPG_PROCESS_GET_ID:
      {
        int *r_id = va_arg (arg_ptr, int *);

        if (r_id == NULL)
          return GPG_ERR_INV_VALUE;

        *r_id = (int)process->pid;
        return 0;
      }

    case GNUPG_PROCESS_GET_EXIT_ID:
      {
        int status = process->wstatus;
        int *r_exit_status = va_arg (arg_ptr, int *);

        if (!process->terminated)
          return GPG_ERR_UNFINISHED;

        if (WIFEXITED (status))
          {
            if (r_exit_status)
              *r_exit_status = WEXITSTATUS (status);
          }
        else
          *r_exit_status = -1;

        return 0;
      }

    case GNUPG_PROCESS_GET_PID:
      {
        pid_t *r_pid = va_arg (arg_ptr, pid_t *);

        if (r_pid == NULL)
          return GPG_ERR_INV_VALUE;

        *r_pid = process->pid;
        return 0;
      }

    case GNUPG_PROCESS_GET_WSTATUS:
      {
        int status = process->wstatus;
        int *r_if_exited = va_arg (arg_ptr, int *);
        int *r_if_signaled = va_arg (arg_ptr, int *);
        int *r_exit_status = va_arg (arg_ptr, int *);
        int *r_termsig = va_arg (arg_ptr, int *);

        if (!process->terminated)
          return GPG_ERR_UNFINISHED;

        if (WIFEXITED (status))
          {
            if (r_if_exited)
              *r_if_exited = 1;
            if (r_if_signaled)
              *r_if_signaled = 0;
            if (r_exit_status)
              *r_exit_status = WEXITSTATUS (status);
            if (r_termsig)
              *r_termsig = 0;
          }
        else if (WIFSIGNALED (status))
          {
            if (r_if_exited)
              *r_if_exited = 0;
            if (r_if_signaled)
              *r_if_signaled = 1;
            if (r_exit_status)
              *r_exit_status = 0;
            if (r_termsig)
              *r_termsig = WTERMSIG (status);
          }

        return 0;
      }

    case GNUPG_PROCESS_KILL:
      {
        int sig = va_arg (arg_ptr, int);

        return process_kill (process, sig);
      }

    default:
      break;
    }

  return GPG_ERR_UNKNOWN_COMMAND;
}

gpg_err_code_t
gnupg_process_ctl (gnupg_process_t process, unsigned int request, ...)
{
  va_list arg_ptr;
  gpg_err_code_t ec;

  va_start (arg_ptr, request);
  ec = process_vctl (process, request, arg_ptr);
  va_end (arg_ptr);
  return ec;
}

gpg_err_code_t
gnupg_process_wait (gnupg_process_t process, int hang)
{
  gpg_err_code_t ec;
  int status;
  pid_t pid;

  if (process->terminated)
    /* Already terminated.  */
    return 0;

  pre_syscall ();
  while ((pid = waitpid (process->pid, &status, hang? 0: WNOHANG))
         == (pid_t)(-1) && errno == EINTR);
  post_syscall ();

  if (pid == (pid_t)(-1))
    {
      ec = gpg_err_code_from_syserror ();
      log_error (_("waiting for process %d to terminate failed: %s\n"),
                        (int)pid, gpg_strerror (ec));
    }
  else if (!pid)
    {
      ec = GPG_ERR_TIMEOUT; /* Still running.  */
    }
  else
    {
      process->terminated = 1;
      process->wstatus = status;
      ec = 0;
    }

  return ec;
}

void
gnupg_process_release (gnupg_process_t process)
{
  if (!process)
    return;

  if (process->terminated)
    {
      gnupg_process_terminate (process);
      gnupg_process_wait (process, 1);
    }

  xfree (process);
}

gpg_err_code_t
gnupg_process_wait_list (gnupg_process_t *process_list, int count, int hang)
{
  gpg_err_code_t ec = 0;
  int i;

  for (i = 0; i < count; i++)
    {
      if (process_list[i]->terminated)
        continue;

      ec = gnupg_process_wait (process_list[i], hang);
      if (ec)
        break;
    }

  return ec;
}
