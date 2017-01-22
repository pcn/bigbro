/* bigbro filetracking library
   Copyright (C) 2015,2016 David Roundy

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA */

#define _GNU_SOURCE

#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1

#include "bigbro.h"

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux-proc.h"

#include "errors.h"

#include <sys/wait.h>
#include <stdarg.h>
#include <fcntl.h> /* for flags to open(2) */

#include <sys/stat.h>

#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <linux/limits.h>
#include <errno.h>

#include <stdint.h>

#include "syscalls/linux.h"

static const void *my_ptrace_options =
  (void *)(PTRACE_O_TRACESYSGOOD |
           PTRACE_O_TRACEFORK |
           PTRACE_O_TRACEVFORK |
           PTRACE_O_TRACEVFORKDONE |
           PTRACE_O_TRACECLONE |
           PTRACE_O_TRACEEXEC);

enum arguments {
  RETURN_VALUE = -1
};

#ifdef __x86_64__
struct i386_user_regs_struct {
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
  uint32_t esi;
  uint32_t edi;
  uint32_t ebp;
  uint32_t eax;
  uint32_t xds;
  uint32_t xes;
  uint32_t xfs;
  uint32_t xgs;
  uint32_t orig_eax;
  uint32_t eip;
  uint32_t xcs;
  uint32_t eflags;
  uint32_t esp;
  uint32_t xss;
};

static long get_syscall_arg_64(const struct user_regs_struct *regs, int which) {
    switch (which) {
    case RETURN_VALUE: return regs->rax;
    case 0: return regs->rdi;
    case 1: return regs->rsi;
    case 2: return regs->rdx;
    case 3: return regs->r10;
    case 4: return regs->r8;
    case 5: return regs->r9;
    default: return -1L;
    }
}

static long get_syscall_arg_32(const struct i386_user_regs_struct *regs, int which) {
#else
static long get_syscall_arg_32(const struct user_regs_struct *regs, int which) {
#endif
    switch (which) {
    case RETURN_VALUE: return regs->eax;
    case 0: return regs->ebx;
    case 1: return regs->ecx;
    case 2: return regs->edx;
    case 3: return regs->esi;
    case 4: return regs->edi;
    case 5: return regs->ebp;
    default: return -1L;
    }
}

static char *read_a_string(pid_t child, unsigned long addr) {
    if (addr == 0) return 0;

    // There is a tradeoff here between allocating something too large
    // and wasting memory vs the cost of reallocing repeatedly.
    char *val = malloc(1024);
    int allocated = 1024;
    int read = 0;
    unsigned long tmp;
    while (1) {
        if (read + sizeof tmp > allocated) {
            allocated *= 2;
            val = realloc(val, allocated);
        }
        tmp = ptrace(PTRACE_PEEKDATA, child, addr + read);
        if(errno != 0) {
            val[read] = 0;
            break;
        }
        memcpy(val + read, &tmp, sizeof tmp);
        if (memchr(&tmp, 0, sizeof tmp) != NULL)
            break;
        read += sizeof tmp;
    }
    return val;
}

static pid_t wait_for_syscall(rw_status *h, int firstborn) {
  pid_t child = 0;
  int status = 0;
  while (1) {
    long signal_to_send_back = 0;
    child = waitpid(-firstborn, &status, __WALL);
    if (child == -1) {
      fprintf(stderr, "had trouble waiting: %s", strerror(errno));
      exit(1);
    }
    if (WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80)) {
      return child;
    } else if (WIFEXITED(status)) {
      debugprintf("%d: exited -> %d\n", child, -WEXITSTATUS(status));
      if (child == firstborn) return -WEXITSTATUS(status);
      continue; /* no need to do anything more for this guy */
    } else if (WIFSIGNALED(status)) {
      debugprintf("process %d died of a signal!\n", child);
      if (child == firstborn) return -WTERMSIG(status);
      continue; /* no need to do anything more for this guy */
    } else if (WIFSTOPPED(status) && (status>>8) == (SIGTRAP | PTRACE_EVENT_FORK << 8)) {
      unsigned long pid;
      ptrace(PTRACE_GETEVENTMSG, child, 0, &pid);
      debugprintf("%ld: forked from %d\n", pid, child);
    } else if (WIFSTOPPED(status) && (status>>8) == (SIGTRAP | PTRACE_EVENT_VFORK << 8)) {
      unsigned long pid;
      ptrace(PTRACE_GETEVENTMSG, child, 0, &pid);
      debugprintf("%ld: vforked from %d\n", pid, child);
    } else if (WIFSTOPPED(status) && (status>>8) == (SIGTRAP | PTRACE_EVENT_CLONE << 8)) {
      unsigned long pid;
      ptrace(PTRACE_GETEVENTMSG, child, 0, &pid);
      debugprintf("%ld: cloned from %d\n", pid, child);
    } else if (WIFSTOPPED(status) && (status>>8) == (SIGTRAP | PTRACE_EVENT_EXEC << 8)) {
      unsigned long pid;
      ptrace(PTRACE_GETEVENTMSG, child, 0, &pid);
      debugprintf("%ld: execed from %d\n", pid, child);
    } else if (WIFSTOPPED(status)) {
      // ensure that the signal we interrupted is actually delivered.
      switch (WSTOPSIG(status)) {
      case SIGCHLD: // I don't know why forwarding SIGCHLD along causes trouble.  :(
      case SIGTRAP: // SIGTRAP is what we get from ptrace
      case SIGVTALRM: // for some reason this causes trouble with ghc
        debugprintf("%d: ignoring signal %d\n", child, WSTOPSIG(status));
        break;
      default:
        signal_to_send_back = WSTOPSIG(status);
        debugprintf("%d: sending signal %d\n", child, signal_to_send_back);
      }
    } else {
      debugprintf("%d: unexpected something\n", child);
    }
    // tell the child to keep going!
    if (ptrace(PTRACE_SYSCALL, child, 0, (void *)signal_to_send_back) == -1) {
      /* Assume child died and that we will get a WIFEXITED
         shortly. */
    }
  }
}

static enum syscall get_registers(pid_t child, void **voidregs,
                                  long (**get_syscall_arg)(void *regs, int which)) {
  struct user_regs_struct *regs = malloc(sizeof(struct user_regs_struct));

  if (ptrace(PTRACE_GETREGS, child, NULL, regs) == -1) {
    debugprintf("error getting registers for %d!\n", child);
    free(regs);
    return sc_invalid_syscall;
  }
#ifdef __x86_64__
  if (regs->cs == 0x23) {
    struct i386_user_regs_struct *i386_regs = malloc(sizeof(struct i386_user_regs_struct));
    i386_regs->ebx = regs->rbx;
    i386_regs->ecx = regs->rcx;
    i386_regs->edx = regs->rdx;
    i386_regs->esi = regs->rsi;
    i386_regs->edi = regs->rdi;
    i386_regs->ebp = regs->rbp;
    i386_regs->eax = regs->rax;
    i386_regs->orig_eax = regs->orig_rax;
    i386_regs->eip = regs->rip;
    i386_regs->esp = regs->rsp;
    free(regs);
    *voidregs = i386_regs;
    struct i386_user_regs_struct *regs = i386_regs;
#else
    *voidregs = regs;
#endif
    *get_syscall_arg = (long (*)(void *regs, int which))get_syscall_arg_32;
    enum syscall val = syscalls_32(regs->orig_eax);
    if (val == sc_invalid_syscall) {
      debugprintf("%d: weird 32-bit system call:  %ld\n", child, regs->orig_eax);
      free(regs);
    }
    return val;
#ifdef __x86_64__
  } else {
    *voidregs = regs;
    *get_syscall_arg = (long (*)(void *regs, int which))get_syscall_arg_64;
    enum syscall val = syscalls_64(regs->orig_rax);
    if (val == sc_invalid_syscall) {
      debugprintf("%d: weird system call number:  %ld\n", child, regs->orig_rax);
      free(regs);
    }
    return val;
  }
#endif
}

static long wait_for_return_value(pid_t child, rw_status *h) {
  ptrace(PTRACE_SYSCALL, child, 0, 0); // ignore return value
  wait_for_syscall(h, -child);
  void *regs = 0;
  long (*get_syscall_arg)(void *regs, int which) = 0;
  get_registers(child, &regs, &get_syscall_arg);
  long retval = get_syscall_arg(regs, RETURN_VALUE);
  free(regs);
  return retval;
}

static int save_syscall_access(pid_t child, rw_status *h) {
  void *regs = 0;
  long (*get_syscall_arg)(void *regs, int which) = 0;

  enum syscall sc = get_registers(child, &regs, &get_syscall_arg);
  if (sc == sc_invalid_syscall) {
    /* we can't read the registers right, but let's not give up! */
    debugprintf("%d: Unable to read registers?!\n", child);
    return 0;
  }
  const char *name = syscall_names[sc];

  debugprintf("%d: %s(?)\n", child, name);

  /*  TODO:

      chroot? mkdir? mkdirat? rmdir? rmdirat?
  */

  if (sc == sc_open || sc == sc_openat) {
    char *arg;
    long flags;
    int fd = wait_for_return_value(child, h);
    if (fd >= 0) {
      int dirfd = -1;
      if (sc == sc_open) {
        arg = read_a_string(child, get_syscall_arg(regs, 0));
        flags = get_syscall_arg(regs, 1);
      } else {
        arg = read_a_string(child, get_syscall_arg(regs, 1));
        flags = get_syscall_arg(regs, 2);
        dirfd = get_syscall_arg(regs, 0);
      }
      if (!arg) {
        debugprintf("%d: %s(NULL) -> %d\n", child, name, fd);
      } else if (flags & O_DIRECTORY) {
        if (!strcmp(name, "open")) {
          debugprintf("%d: opendir('%s') -> %d\n", child, arg, fd);
        } else {
          debugprintf("%d: opendirat(%d, '%s') -> %d\n", child, get_syscall_arg(regs, 0), arg, fd);
        }
      } else if (flags & (O_WRONLY | O_RDWR)) {
        debugprintf("%d: open('%s', 'w') -> %d\n", child, arg, fd);
        if (fd >= 0) {
          write_file_at(child, dirfd, arg, h);
        }
      } else {
        debugprintf("%d: open('%s', 'r') -> %d\n", child, arg, fd);
        read_file_at(child, dirfd, arg, h);
      }
      free(arg);
    }
  } else if (sc == sc_unlink || sc == sc_unlinkat) {
    char *arg;
    int retval = wait_for_return_value(child, h);
    if (retval == 0) {
      int dirfd = -1;
      if (sc == sc_unlink) {
        arg = read_a_string(child, get_syscall_arg(regs, 0));
        if (arg) debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
      } else {
        arg = read_a_string(child, get_syscall_arg(regs, 1));
        dirfd = get_syscall_arg(regs, 0);
        if (arg) debugprintf("%d: %s(%d, '%s') -> %d\n", child, name,
                             dirfd, arg, retval);
      }
      char *rawpath = interpret_path_at(child, dirfd, arg);
      char *abspath = flexible_realpath(rawpath, 0, h, look_for_symlink, false);
      delete_from_hashset(&h->read, abspath);
      delete_from_hashset(&h->readdir, abspath);
      delete_from_hashset(&h->written, abspath);
      free(rawpath);
      free(abspath);
    }
  } else if (sc == sc_creat || sc == sc_truncate ||
             sc == sc_utime || sc == sc_utimes) {
    char *arg = read_a_string(child, get_syscall_arg(regs, 0));
    int retval = wait_for_return_value(child, h);
    if (arg) {
      debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
      if (retval >= 0) write_file_at(child, -1, arg, h);
      free(arg);
    }
  } else if (sc == sc_utimensat) {
    char *arg = read_a_string(child, get_syscall_arg(regs, 1));
    if (arg) {
      int dirfd = get_syscall_arg(regs,0);
      int flags = get_syscall_arg(regs,3);
      int retval = wait_for_return_value(child, h);
      debugprintf("%d: %s(%d, '%s', ? , %d) -> %d\n", child, name,
                  dirfd, arg, flags, retval);
      if (retval >= 0) {
        if (flags == AT_SYMLINK_NOFOLLOW) {
          read_link_at(child, dirfd, arg, h);
        } else {
          read_file_at(child, dirfd, arg, h);
        }
      }
      free(arg);
    } else {
      int retval = wait_for_return_value(child, h);
      debugprintf("%d: %s(%d, NULL) -> %d\n", child, name,
                  (int)get_syscall_arg(regs,0), retval);
    }
  } else if (sc == sc_futimesat) {
    char *arg = read_a_string(child, get_syscall_arg(regs, 1));
    if (arg) {
      int dirfd = get_syscall_arg(regs,0);
      int retval = wait_for_return_value(child, h);
      debugprintf("%d: %s(%d, '%s') -> %d\n", child, name,
                  dirfd, arg, retval);
      if (retval >= 0) read_file_at(child, dirfd, arg, h);
      free(arg);
    } else {
      int retval = wait_for_return_value(child, h);
      debugprintf("%d: %s(%d, NULL) -> %d\n", child, name,
                  (int)get_syscall_arg(regs,0), retval);
    }
  } else if (sc == sc_lstat || sc == sc_lstat64 ||
             sc == sc_readlink || sc == sc_readlinkat) {
    char *arg;
    int retval = wait_for_return_value(child, h);
    if (retval >= 0) {
      int dirfd = -1;
      if (sc != sc_readlinkat) {
        arg = read_a_string(child, get_syscall_arg(regs, 0));
      } else {
        arg = read_a_string(child, get_syscall_arg(regs, 1));
        dirfd = get_syscall_arg(regs, 0);
      }
      if (arg) {
        debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
        read_link_at(child, dirfd, arg, h);
      }
      free(arg);
    }
  } else if (sc == sc_stat || sc == sc_stat64) {
    int retval = wait_for_return_value(child, h);
    if (retval == 0) {
      char *arg = read_a_string(child, get_syscall_arg(regs, 0));
      if (arg) {
        debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
        read_file_at(child, -1, arg, h);
        free(arg);
      }
    }
  } else if (sc == sc_mkdir || sc == sc_mkdirat) {
    int retval = wait_for_return_value(child, h);
    if (retval == 0) {
      char *arg;
      int dirfd = -1;
      if (sc != sc_mkdirat) {
        arg = read_a_string(child, get_syscall_arg(regs, 0));
      } else {
        dirfd = get_syscall_arg(regs, 0);
        arg = read_a_string(child, get_syscall_arg(regs, 1));
      }
      if (arg) {
        debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
        char *rawpath = interpret_path_at(child, dirfd, arg);
        char *abspath = flexible_realpath(rawpath, 0, h, look_for_file_or_directory, false);
        insert_hashset(&h->mkdir, abspath);
        free(rawpath);
        free(abspath);
        free(arg);
      }
    }
  } else if (sc == sc_symlink || sc == sc_symlinkat) {
    char *arg, *target;
    int retval = wait_for_return_value(child, h);
    if (retval == 0) {
      int dirfd = -1;
      if (sc == sc_symlink) {
        target = read_a_string(child, get_syscall_arg(regs, 0));
        arg = read_a_string(child, get_syscall_arg(regs, 1));
      } else {
        target = read_a_string(child, get_syscall_arg(regs, 0));
        dirfd = get_syscall_arg(regs, 1);
        arg = read_a_string(child, get_syscall_arg(regs, 2));
      }
      debugprintf("%d: %s(%p %p %d)\n", child, name, arg, target, dirfd);
      if (arg && target) {
        if (sc == sc_symlink) {
          debugprintf("%d: %s('%s', '%s')\n", child, name, target, arg);
        } else {
          debugprintf("%d: %s('%s', %d, '%s')\n", child, name, target, dirfd, arg);
        }
        write_link_at(child, dirfd, arg, h);
      }
      free(arg);
      free(target);
    }
  } else if (sc == sc_execve || sc == sc_execveat) {
    char *arg;
    int dirfd = -1;
    if (sc == sc_execve) {
      arg = read_a_string(child, get_syscall_arg(regs, 0));
    } else {
      arg = read_a_string(child, get_syscall_arg(regs, 1));
      dirfd = get_syscall_arg(regs, 0);
    }
    if (arg && strlen(arg)) {
      debugprintf("%d: %s('%s')\n", child, name, arg);
      maybe_read_file_at(child, dirfd, arg, h);
    }
    free(arg);
  } else if (sc == sc_rename || sc == sc_renameat) {
    char *from, *to;
    int dirfd = AT_FDCWD;
    if (sc == sc_rename) {
        from = read_a_string(child, get_syscall_arg(regs, 0));
    } else {
      from = read_a_string(child, get_syscall_arg(regs, 1));
      dirfd = get_syscall_arg(regs, 0);
    }
    if (from) {
      char *rawpath = interpret_path_at(child, dirfd, from);
      char *abspath = flexible_realpath(rawpath, 0, h, look_for_symlink, false);
      free(rawpath);
      free(from);
      from = abspath;
    }
    int retval = wait_for_return_value(child, h);
    if (retval == 0) {
      if (sc == sc_rename) {
        to = read_a_string(child, get_syscall_arg(regs, 1));
      } else {
        to = read_a_string(child, get_syscall_arg(regs, 2));
      }
      if (to && from) {
        debugprintf("%d: %s('%s', '%s') -> %d\n", child, name, from, to, retval);

        struct stat path_stat;
        if (!fstatat(dirfd, to, &path_stat, AT_SYMLINK_NOFOLLOW)
            && S_ISDIR(path_stat.st_mode)) {
          // it is a directory, so we need to generate write events
          // for children, and remove events for children... yuck.
          char *absto = interpret_path_at(child, dirfd, to);
          char *fromslash = malloc(strlen(from)+2);
          strcpy(fromslash, from);
          strcat(fromslash, "/");
          char *toslash = malloc(strlen(absto)+2);
          strcpy(toslash, absto);
          strcat(toslash, "/");
          int fromslashlen = strlen(fromslash);
          int toslashlen = strlen(toslash);
          for (struct hash_entry *e = h->written.first; e; e = e->next) {
            if (strncmp(e->key, fromslash, fromslashlen) == 0) {
              char *newk = malloc(strlen(e->key) - fromslashlen + toslashlen + 1);
              strcpy(newk, toslash);
              strcat(newk, e->key + fromslashlen);
              insert_hashset(&h->written, newk);
              delete_from_hashset(&h->written, e->key);
            }
          }
          for (struct hash_entry *e = h->read.first; e; e = e->next) {
            if (strncmp(e->key, fromslash, fromslashlen) == 0) {
              char *newk = malloc(strlen(e->key) - fromslashlen + toslashlen + 1);
              strcpy(newk, toslash);
              strcat(newk, e->key + fromslashlen);
              insert_hashset(&h->written, newk);
              delete_from_hashset(&h->read, e->key);
            }
          }
          for (struct hash_entry *e = h->readdir.first; e; e = e->next) {
            if (strncmp(e->key, fromslash, fromslashlen) == 0) {
              delete_from_hashset(&h->readdir, e->key);
            }
          }
          insert_hashset(&h->mkdir, absto);
          free(absto);
          free(fromslash);
          free(toslash);
        } else {
          write_link_at(child, dirfd, to, h);
          delete_from_hashset(&h->read, from);
          delete_from_hashset(&h->readdir, from);
          delete_from_hashset(&h->written, from);
        }
      }
      free(to);
    }
    free(from);
  } else if (sc == sc_link || sc == sc_linkat) {
    char *from, *to;
    int retval = wait_for_return_value(child, h);
    if (retval == 0) {
      int dirfd = -1;
      if (sc == sc_link) {
        from = read_a_string(child, get_syscall_arg(regs, 0));
        to = read_a_string(child, get_syscall_arg(regs, 1));
      } else {
        from = read_a_string(child, get_syscall_arg(regs, 1));
        to = read_a_string(child, get_syscall_arg(regs, 2));
        dirfd = get_syscall_arg(regs, 0);
      }
      if (to && from) {
        debugprintf("%d: link('%s', '%s') -> %d\n", child, from, to, retval);
        read_file_at(child, dirfd, from, h);
        write_file_at(child, dirfd, to, h);
      }
      free(from);
      free(to);
    }
  } else if (sc == sc_getdents || sc == sc_getdents64) {
    int fd = get_syscall_arg(regs, 0);
    int retval = wait_for_return_value(child, h);
    debugprintf("%d: %s(%d) -> %d\n", child, name, fd, retval);
    if (retval >= 0) {
      read_dir_fd(child, fd, h);
    }
  } else if (sc == sc_chdir) {
    char *arg = read_a_string(child, get_syscall_arg(regs, 0));
    /* not actually a file, but this gets symlinks in the chdir path */
    read_file_at(child, -1, arg, h);
    int retval = wait_for_return_value(child, h);
    if (arg) {
      debugprintf("%d: chdir(%s) -> %d\n", child, arg, retval);
      free(arg);
    } else {
      debugprintf("%d: chdir(NULL) -> %d\n", child, retval);
    }
  }

  free(regs);
  return 0;
}

int bigbro(const char *workingdir, pid_t *child_ptr,
           int stdoutfd, int stderrfd, char **envp,
           const char *cmdline,
           char ***read_from_directories_out,
           char ***read_from_files_out,
           char ***written_to_files_out) {
  char **mkdir_directories = 0;
  printf("calling bigbro_with_mkdir...\n");
  int retval = bigbro_with_mkdir(workingdir, child_ptr, stdoutfd, stderrfd,
                                 envp, cmdline,
                                 read_from_directories_out, &mkdir_directories,
                                 read_from_files_out, written_to_files_out);
  free(mkdir_directories);
  return retval;
}

int bigbro_with_mkdir(const char *workingdir, pid_t *child_ptr,
                      int stdoutfd, int stderrfd, char **envp,
                      const char *cmdline,
                      char ***read_from_directories_out,
                      char ***mkdir_directories,
                      char ***read_from_files_out,
                      char ***written_to_files_out) {
  pid_t firstborn = fork();
  if (firstborn == -1) {
    // Not sure what to do in case of fork error...
  }
  setpgid(firstborn, firstborn); // causes grandchildren to be killed along with firstborn

  if (firstborn == 0) {
    if (stdoutfd > 0 || stderrfd > 0) {
      close(0); // close stdin so programs won't wait on input
      open("/dev/null", O_RDONLY);
      if (stdoutfd > 0) {
        close(1);
        dup2(stdoutfd, 1);
      }
      if (stderrfd > 0) {
        close(2);
        dup2(stderrfd, 2);
      }
    }
    if (workingdir && chdir(workingdir) != 0) return -1;
    ptrace(PTRACE_TRACEME);
    kill(getpid(), SIGSTOP);
    char **args = (char **)malloc(4*sizeof(char *));
    args[0] = "/bin/sh";
    args[1] = "-c";
    args[2] = (char *)cmdline;
    args[3] = 0;
    // when envp == 0, we are supposed to inherit our environment.
    if (envp == 0) envp = environ;
    return execve(args[0], args, envp);
  } else {
    *child_ptr = firstborn;
    waitpid(firstborn, 0, __WALL);
    ptrace(PTRACE_SETOPTIONS, firstborn, 0, my_ptrace_options);
    if (ptrace(PTRACE_SYSCALL, firstborn, 0, 0) == -1) {
      return -1;
    }

    rw_status h;
    init_hashset(&h.read, 1024);
    init_hashset(&h.readdir, 1024);
    init_hashset(&h.written, 1024);
    init_hashset(&h.mkdir, 1024);

    while (1) {
      pid_t child = wait_for_syscall(&h, firstborn);
      if (child <= 0) {
        debugprintf("Returning with exit value %d\n", -child);
        *read_from_files_out = hashset_to_array(&h.read);
        *read_from_directories_out = hashset_to_array(&h.readdir);
        *written_to_files_out = hashset_to_array(&h.written);
        *mkdir_directories = hashset_to_array(&h.mkdir);
        free_hashset(&h.read);
        free_hashset(&h.readdir);
        free_hashset(&h.written);
        free_hashset(&h.mkdir);
        return -child;
      }

      if (save_syscall_access(child, &h) == -1) {
        /* We were unable to read the process's registers.  Assume
           that this is bad news, and that we should exit.  I'm not
           sure what else to do here. */
        return -1;
      }

      ptrace(PTRACE_SYSCALL, child, 0, 0); // ignore return value
    }
  }
  return 0;
}
