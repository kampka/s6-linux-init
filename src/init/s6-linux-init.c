/* ISC license. */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>

#include <linux/kd.h>

#include <skalibs/types.h>
#include <skalibs/allreadwrite.h>
#include <skalibs/sgetopt.h>
#include <skalibs/strerr2.h>
#include <skalibs/stralloc.h>
#include <skalibs/sig.h>
#include <skalibs/env.h>
#include <skalibs/djbunix.h>
#include <skalibs/exec.h>

#include <s6/config.h>

#include <s6-linux-init/config.h>
#include "defaults.h"
#include "initctl.h"

#define USAGE "s6-linux-init [ -c basedir ] [ -p initpath ] [ -s envdumpdir ] [ -m umask ] [ -d devtmpfs ] [ -D initdefault ] [ -n | -N ] [ -C ] [ -B ]"
#define dieusage() strerr_dieusage(100, USAGE)

#define BANNER "\n  s6-linux-init version " S6_LINUX_INIT_VERSION "\n\n"

static int inns = 0 ;
static int nologger = 0 ;
static int notifpipe[2] ;

static inline char const *scan_cmdline (char const *initdefault, char const *const *argv, unsigned int argc)
{
  if (!inns)
  {
    static char const *valid[] = { "default", "1", "2", "3", "4", "5", 0 } ;
    for (unsigned int i = 0 ; i < argc ; i++)
      for (char const *const *p = valid ; *p ; p++)
        if (!strcmp(argv[i], *p)) return argv[i] ;
  }
  return initdefault ;
}

static inline void wait_for_notif (int fd)
{
  char buf[16] ;
  for (;;)
  {
    ssize_t r = read(fd, buf, 16) ;
    if (r < 0) strerr_diefu1sys(111, "read from notification pipe") ;
    if (!r)
    {
      strerr_warnw1x("s6-svscan failed to send a notification byte!") ;
      break ;
    }
    if (memchr(buf, '\n', r)) break ;
  }
  close(fd) ;
}

static void kbspecials (void)
{
  int fd ;
  if (inns) return ;
  fd = open("/dev/tty0", O_RDONLY | O_NOCTTY) ;
  if (fd < 0)
    strerr_warnwu2sys("open /dev/", "tty0 (kbrequest will not be handled)") ;
  else
  {
    if (ioctl(fd, KDSIGACCEPT, SIGWINCH) < 0)
      strerr_warnwu2sys("ioctl KDSIGACCEPT on ", "tty0 (kbrequest will not be handled)") ;
    close(fd) ;
  }

  sig_block(SIGINT) ; /* don't panic on early cad before s6-svscan catches it */
  if (reboot(RB_DISABLE_CAD) == -1)
    strerr_warnwu1sys("trap ctrl-alt-del") ;
}

static inline void run_stage2 (char const *basedir, char const **argv, unsigned int argc, char const *const *envp, size_t envlen, char const *modifs, size_t modiflen, char const *initdefault)
{
  size_t dirlen = strlen(basedir) ;
  char const *childargv[argc + 3] ;
  char fn[dirlen + sizeof("/scripts/" STAGE2)] ;
  PROG = "s6-linux-init (child)" ;
  memcpy(fn, basedir, dirlen) ;
  memcpy(fn + dirlen, "/scripts/" STAGE2, sizeof("/scripts/" STAGE2)) ;
  childargv[0] = fn ;
  childargv[1] = scan_cmdline(initdefault, argv, argc) ;
  for (unsigned int i = 0 ; i < argc ; i++)
    childargv[i+2] = argv[i] ;
  childargv[argc + 2] = 0 ;
  if (nologger)
  {
    close(notifpipe[1]) ;
    wait_for_notif(notifpipe[0]) ;
  }
  else
  {
   /* block on opening the log fifo until the catch-all logger is up */
    close(1) ;
    if (open(LOGFIFO, O_WRONLY) != 1)
      strerr_diefu1sys(111, "open " LOGFIFO " for writing") ;
    if (fd_copy(2, 1) == -1)
      strerr_diefu1sys(111, "fd_copy stdout to stderr") ;
  }
  xmexec_fm(childargv, envp, envlen, modifs, modiflen) ;
}

/*
   This is ugly voodoo, keep away from innocent eyes.
   If stdin is a terminal, it means we're in a "docker run -it" container
(or equivalent) and stdin is a ctty. We don't want the supervision tree to
have this ctty (else ^C kills everything) but we want stage 2 to keep it,
so that if we have a CMD run by stage 2 (as is the case with s6-overlay),
it remains interactive (control sequences can be sent to it).
   In order to achieve that, we have the child (future stage 2) steal the
ctty from the parent (future s6-svscan). But that may not work, for instance
in a USER container, where we don't have the appropriate permissions; in
which case the parent must be informed that the operation failed, so it can
relinquish the ctty itself - the child won't get the ctty but at least the
parent won't keep it.
   Transmission of the information is done via a pipe, it's the simplest
way to properly order operations.
   If the parent abandons its ctty itself, the child needs to protect against
the SIGHUP it receives when it happens.
*/

static inline pid_t fork_and_setup_session (int ttyfd)  /* closes ttyfd */
{
  pid_t pid ;
  if (ttyfd < 0)  /* the common case is simple */
  {
    pid = fork() ;
    if (pid == -1) strerr_diefu1sys(111, "fork") ;
    setsid() ;
  }
  else  /* let the insanity begin */
  {
    int p[2] ;
    if (pipe(p) == -1) strerr_diefu1sys(111, "pipe") ;
    pid = fork() ;
    if (pid == -1) strerr_diefu1sys(111, "fork") ;
    if (!pid)
    { /* child */
      PROG = "s6-linux-init (child)" ;
      close(p[0]) ;
      if (setsid() == -1) strerr_diefu1sys(111, "setsid") ;
      if (fd_move(0, ttyfd) == -1) strerr_diefu1sys(111, "move stdin fd") ;
      if (ioctl(0, TIOCSCTTY, 1) == -1)
      {
        strerr_warnwu1sys("grab controlling terminal") ;
        if (!sig_altignore(SIGHUP))
          strerr_diefu1sys(111, "ignore SIGHUP") ;
        if (write(p[1], "1", 1) < 1)  /* '1' == problem */
          strerr_diefu1sys(111, "write to parent") ;
      }
      else if (write(p[1], "0", 1) < 1)  /* '0' == ok */
        strerr_diefu1sys(111, "write to parent") ;
      close(p[1]) ;
    }
    else
    { /* parent */
      int r ;
      char c ;
      close(p[1]) ;
      r = read(p[0], &c, 1) ;
      if (r == -1)
        strerr_diefu1sys(111, "read from child") ;
      else if (!r)
        strerr_dief1x(111, "child died") ;
      else if (c == '1')
      {
        if (ioctl(ttyfd, TIOCNOTTY) == -1)
          strerr_warnwu1sys("relinquish controlling terminal") ;
      }
      else if (c != '0')
        strerr_dief1x(100, "bad parent/child protocol") ;
      close(p[0]) ;
      close(ttyfd) ;
    }
  }
  return pid ;
}

int main (int argc, char const **argv, char const *const *envp)
{
  mode_t mask = 0022 ;
  char const *basedir = BASEDIR ;
  char const *path = INITPATH ;
  char const *slashdev = 0 ;
  char const *envdumpdir = 0 ;
  char const *initdefault = "default" ;
  int mounttype = 1 ;
  int hasconsole = 1 ;
  int ttyfd = -1 ;
  stralloc envmodifs = STRALLOC_ZERO ;
  PROG = "s6-linux-init" ;

  if (getpid() != 1)
  {
    argv[0] = S6_LINUX_INIT_BINPREFIX "s6-linux-init-telinit" ;
    xexec_e(argv, envp) ;
  }

  {
    subgetopt l = SUBGETOPT_ZERO ;
    for (;;)
    {
      int opt = subgetopt_r(argc, argv, "c:p:s:m:d:D:nNCB", &l) ;
      if (opt == -1) break ;
      switch (opt)
      {
        case 'c' : basedir = l.arg ; break ;
        case 'p' : path = l.arg ; break ;
        case 's' : envdumpdir = l.arg ; break ;
        case 'm' : if (!uint0_oscan(l.arg, &mask)) dieusage() ; break ;
        case 'd' : slashdev = l.arg ; break ;
        case 'D' : initdefault = l.arg ; break ;
        case 'n' : mounttype = 2 ; break ;
        case 'N' : mounttype = 0 ; break ;
        case 'C' : inns = 1 ; break ;
        case 'B' : nologger = 1 ; break ;
        default : dieusage() ;
      }
    }
    argc -= l.ind ; argv += l.ind ;
  }

  if (fcntl(1, F_GETFD) < 0) hasconsole = 0 ;
  if (inns)
  {
    char c ;
    ssize_t r = read(3, &c, 1) ;
    if (r < 0)
    {
      if (errno != EBADF) strerr_diefu1sys(111, "read from fd 3") ;
    }
    else
    {
      if (r) strerr_warnw1x("parent wrote to fd 3!") ;
      close(3) ;
    }
    if (isatty(0) && !slashdev) ttyfd = dup(0) ;
  }
  else if (hasconsole) allwrite(1, BANNER, sizeof(BANNER) - 1) ;
  if (chdir("/") == -1) strerr_diefu1sys(111, "chdir to /") ;
  umask(mask) ;
  close(0) ;

  if (slashdev)
  {
    int nope, e ;
    close(1) ;
    close(2) ;
   /* at this point we're totally in the dark, hoping /dev/console will work */
    nope = mount("dev", slashdev, "devtmpfs", MS_NOSUID | MS_NOEXEC, "") < 0 ;
    e = errno ;
    if (open("/dev/console", O_WRONLY) && open("/dev/null", O_WRONLY)) return 111 ;
    if (fd_move(2, 0) < 0 || fd_copy(1, 2) < 0) return 111 ;
    if (nope)
    {
      errno = e ;
      strerr_diefu1sys(111, "mount a devtmpfs on /dev") ;
    }
  }

  if (open("/dev/null", O_RDONLY))
  {  /* ghetto /dev/null to the rescue */
    int p[2] ;
    strerr_warnwu1sys("open /dev/null") ;
    if (pipe(p) < 0) strerr_diefu1sys(111, "pipe") ;
    close(p[1]) ;
    if (fd_move(0, p[0]) < 0) strerr_diefu1sys(111, "fd_move to stdin") ;
  }

  if (!hasconsole)
    if (open("/dev/null", O_WRONLY) != 1 || fd_copy(2, 1) == -1)
      return 111 ;

  if (mounttype)
  {
    if (mounttype == 2)
    {
      if (mount("tmpfs", S6_LINUX_INIT_TMPFS, "tmpfs", MS_REMOUNT | MS_NODEV | MS_NOSUID, "mode=0755") == -1)
        strerr_diefu1sys(111, "remount " S6_LINUX_INIT_TMPFS) ;
    }
    else
    {
      if (umount(S6_LINUX_INIT_TMPFS) == -1)
      {
        if (errno != EINVAL)
          strerr_warnwu1sys("umount " S6_LINUX_INIT_TMPFS) ;
      }
      if (mount("tmpfs", S6_LINUX_INIT_TMPFS, "tmpfs", MS_NODEV | MS_NOSUID, "mode=0755") == -1)
        strerr_diefu1sys(111, "mount tmpfs on " S6_LINUX_INIT_TMPFS) ;
    }
  }

  {
    size_t dirlen = strlen(basedir) ;
    char fn[dirlen + 1 + (sizeof(RUNIMAGE) > sizeof(ENVSTAGE1) ? sizeof(RUNIMAGE) : sizeof(ENVSTAGE1))] ;
    memcpy(fn, basedir, dirlen) ;
    fn[dirlen] = '/' ;
    memcpy(fn + dirlen + 1, RUNIMAGE, sizeof(RUNIMAGE)) ;
    if (!hiercopy(fn, S6_LINUX_INIT_TMPFS))
      strerr_diefu3sys(111, "copy ", fn, " to " S6_LINUX_INIT_TMPFS) ;
    memcpy(fn + dirlen + 1, ENVSTAGE1, sizeof(ENVSTAGE1)) ;
    if (envdir(fn, &envmodifs) == -1)
      strerr_warnwu2sys("envdir ", fn) ;
  }
  if (envdumpdir && !env_dump(envdumpdir, 0700, envp))
    strerr_warnwu2sys("dump kernel environment to ", envdumpdir) ;

  if (!nologger)
  {
    int fdr = open_read(LOGFIFO) ;
    if (fdr == -1) strerr_diefu1sys(111, "open " LOGFIFO) ;
    fd_close(1) ;
    if (open(LOGFIFO, O_WRONLY) != 1) strerr_diefu1sys(111, "open " LOGFIFO) ;
    fd_close(fdr) ;
  }

  {
    char const *newenvp[2] = { 0, 0 } ;
    size_t pathlen = path ? strlen(path) : 0 ;
    char fmtfd[2 + UINT_FMT] = "-" ;
    char const *newargv[5] = { S6_EXTBINPREFIX "s6-svscan", fmtfd, "--", S6_LINUX_INIT_TMPFS "/" SCANDIR, 0 } ;
    char pathvar[6 + pathlen] ;
    if (path)
    {
      if (setenv("PATH", path, 1) == -1)
        strerr_diefu1sys(111, "set initial PATH") ;
      memcpy(pathvar, "PATH=", 5) ;
      memcpy(pathvar + 5, path, pathlen + 1) ;
      newenvp[0] = pathvar ;
    }
    if (nologger && pipe(notifpipe) < 0) strerr_diefu1sys(111, "pipe") ;
    if (!fork_and_setup_session(ttyfd))
      run_stage2(basedir, argv, argc, newenvp, !!path, envmodifs.s, envmodifs.len, initdefault) ;
    if (nologger)
    {
      close(notifpipe[0]) ;
      fmtfd[1] = 'd' ;
      fmtfd[2 + uint_fmt(fmtfd + 2, notifpipe[1])] = 0 ;
      kbspecials() ;
    }
    else
    {
      int fd = dup(2) ;
      if (fd < 0) strerr_diefu1sys(111, "dup stderr") ;
      fmtfd[1] = 'X' ;
      fmtfd[2 + uint_fmt(fmtfd + 2, (unsigned int)fd)] = 0 ;
      kbspecials() ;
      if (fd_copy(2, 1) == -1)
        strerr_diefu1sys(111, "redirect output file descriptor") ;
    }
    xmexec_fm(newargv, newenvp, !!path, envmodifs.s, envmodifs.len) ;
  }
}
