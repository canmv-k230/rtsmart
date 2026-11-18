#!/usr/bin/env python3
"""Exercise production DFS/devfs, device, PTY and N_TTY lifetime under ASan/UBSan.

Includes per-open routing, dup-style references, peer hangup, slot exhaustion,
allocation rollback, controlling-terminal detach, foreground hand-off across
session pipelines, endpoint publication at preemption points, shared-queue
wakeups and concurrent open/close.
Blocking I/O/process-exit behavior is additionally tested on the board.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import test_devfs_open as base

TTY = base.DRIVERS / 'tty'
EXTRA = r'''
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <limits.h>
#define RT_IPC_FLAG_FIFO 0
#define RT_USING_LWP
#define __LWP_H__
#define RT_LWP_MAX_NR 8
#define ENOIOCTLCMD 515
#define INIT_DEVICE_EXPORT(fn) int test_##fn(void) { return fn(); }
#define rt_memcpy memcpy
#define rt_memcmp memcmp
#define rt_strlen strlen
#define rt_spin_lock(l) ((void)0)
#define rt_spin_unlock(l) ((void)0)
#define _LWP_NSIG_WORDS 1
#define _LWP_NSIG_BPW 64
struct tty_struct;
typedef struct { unsigned long sig[1]; } lwp_sigset_t;
struct rt_lwp {
    struct tty_struct *tty;
    struct rt_wqueue wait_queue;
    int tty_old_pgrp, leader, session, __pgrp, finish, pid;
    lwp_sigset_t signal_mask;
    void (*signal_handler[64])(int);
};
struct lwp_avl_struct { void *data; };
extern struct lwp_avl_struct test_pids[RT_LWP_MAX_NR];
extern _Thread_local struct rt_lwp *test_current;
static inline struct rt_lwp *lwp_self(void) { return test_current; }
static inline struct lwp_avl_struct *lwp_get_pid_ary(void) { return test_pids; }
/* Records who is signalled (isig_group()). */
extern int test_kills[16], test_kill_count;
static inline int lwp_kill(int pid, int sig) {
    if (test_kill_count < 16) test_kills[test_kill_count] = pid;
    test_kill_count++; return 0;
}
/* Dereference like the kernel does, so ASan reports a freed foreground. */
static inline int lwp_to_pid(struct rt_lwp *lwp) { return lwp->pid ? lwp->pid : lwp->session; }
/* No pid field in this stub: tty.c falls back to scanning test_pids. */
static inline struct rt_lwp *lwp_from_pid(int pid) { return NULL; }
struct termios;
struct termios *get_old_termios(void);
rt_err_t rt_device_unregister(rt_device_t dev);
/* As the kernel: a signal returns RT-Thread's -RT_EINTR, not a POSIX errno.
 * test_wait_hook models a wakeup after it runs (e.g. a peer drained). */
extern _Thread_local int (*test_wait_hook)(struct rt_wqueue *q);
static inline int rt_wqueue_wait_interruptible(struct rt_wqueue *q, int c, int t) {
    return test_wait_hook ? test_wait_hook(q) : -RT_EINTR;
}
void rt_wqueue_wakeup_all(struct rt_wqueue *q, void *key);
#define RT_WQ_FLAG_WAKEUP 0x01
struct rt_wqueue_node {
    rt_thread_t polling_thread; rt_list_t list;
    int (*wakeup)(struct rt_wqueue_node *wait, void *key); rt_uint32_t key;
};
rt_err_t rt_thread_resume(rt_thread_t thread);
static inline void rt_schedule(void) {}
void test_object_published(struct rt_object *object);
void *test_alloc(size_t n);
void *test_calloc(size_t n, size_t size);
#undef rt_malloc
#undef rt_calloc
#define rt_malloc test_alloc
#define rt_calloc test_calloc
'''
SYNC = r'''
struct rt_mutex { pthread_mutex_t mutex; };
typedef struct rt_mutex *rt_mutex_t;
static inline rt_err_t rt_mutex_init(struct rt_mutex *m, const char *n, int f) {
    pthread_mutexattr_t attr; pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int r = pthread_mutex_init(&m->mutex, &attr); pthread_mutexattr_destroy(&attr); return r;
}
static inline rt_err_t rt_mutex_take(struct rt_mutex *m, int t) { return pthread_mutex_lock(&m->mutex); }
static inline rt_err_t rt_mutex_release(struct rt_mutex *m) { return pthread_mutex_unlock(&m->mutex); }
static inline rt_err_t rt_mutex_detach(struct rt_mutex *m) { return pthread_mutex_destroy(&m->mutex); }
void *test_alloc(size_t n);
static inline rt_mutex_t rt_mutex_create(const char *n, int f) {
    rt_mutex_t m = test_alloc(sizeof(*m)); if (m) rt_mutex_init(m,n,f); return m;
}
static inline void rt_mutex_delete(rt_mutex_t m) { assert(m); rt_mutex_detach(m); free(m); }
struct rt_semaphore { sem_t value; };
typedef struct rt_semaphore *rt_sem_t;
static inline rt_err_t rt_sem_init(rt_sem_t s, const char *n, rt_uint32_t v, rt_uint8_t f) { return sem_init(&s->value,0,v); }
static inline rt_err_t rt_sem_take(rt_sem_t s, int t) { return sem_wait(&s->value); }
#define rt_sem_take_interruptible rt_sem_take
static inline rt_err_t rt_sem_release(rt_sem_t s) { return sem_post(&s->value); }
static inline rt_err_t rt_sem_detach(rt_sem_t s) { return sem_destroy(&s->value); }
struct rt_thread { void *lwp; int tid; rt_uint8_t stat; };
extern _Thread_local struct rt_thread test_thread;
typedef struct rt_thread *rt_thread_t;
static inline rt_thread_t rt_thread_self(void) { return &test_thread; }
extern pthread_mutex_t irq_lock;
/* Depth > 0 models a section that the scheduler cannot preempt. */
extern _Thread_local int test_critical_depth;
static inline void rt_enter_critical(void) { pthread_mutex_lock(&irq_lock); test_critical_depth++; }
static inline void rt_exit_critical(void) { test_critical_depth--; pthread_mutex_unlock(&irq_lock); }
static inline rt_base_t rt_hw_interrupt_disable(void) { rt_enter_critical(); return 0; }
static inline void rt_hw_interrupt_enable(rt_base_t level) { rt_exit_critical(); }
'''
CASES = r'''
#include <tty.h>
#include <dfs_file.h>
#include <devfs.h>
#include <stdatomic.h>

pthread_mutex_t irq_lock;
_Thread_local int test_critical_depth;
static pthread_mutex_t fd_lock = PTHREAD_MUTEX_INITIALIZER;
void dfs_fd_lock(void) { pthread_mutex_lock(&fd_lock); }
void dfs_fd_unlock(void) { pthread_mutex_unlock(&fd_lock); }
struct lwp_avl_struct test_pids[RT_LWP_MAX_NR];
_Thread_local struct rt_lwp *test_current;
_Thread_local struct rt_thread test_thread;
struct termios *get_old_termios(void) { static struct termios t; return &t; }
static _Thread_local int fail_after = -1;
static struct tty_struct *detach_on_alloc;
void *test_alloc(size_t n) {
    if (detach_on_alloc) {
        struct tty_struct *tty=detach_on_alloc; detach_on_alloc=NULL;
        tty_detach_session(tty);
    }
    if (fail_after == 0) { fail_after = -1; return NULL; }
    if (fail_after > 0) fail_after--;
    return malloc(n);
}
void *test_calloc(size_t n, size_t s) { void *p = test_alloc(n*s); if(p) memset(p,0,n*s); return p; }
int test_ptmx_register(void);
/* waitqueue.c/thread.c semantics: wakeup resumes and dequeues the first
 * accepting waiter; wakeup_all resumes every accepting waiter and leaves it
 * queued until it runs. Resuming a thread that is not suspended fails, and
 * wakeup_all then prints "Thread resume failed". */
static int resume_failures;
rt_err_t rt_thread_resume(rt_thread_t thread) {
    if ((thread->stat & RT_THREAD_SUSPEND_MASK) != RT_THREAD_SUSPEND_MASK) return -RT_ERROR;
    thread->stat = 0; return RT_EOK;
}
void rt_wqueue_wakeup(rt_wqueue_t *q, void *key) {
    q->flag = RT_WQ_FLAG_WAKEUP;
    for (rt_list_t *n = q->waiting_list.next; n != &q->waiting_list; n = n->next) {
        struct rt_wqueue_node *e = rt_list_entry(n, struct rt_wqueue_node, list);
        if (e->wakeup(e,key) == 0) { rt_thread_resume(e->polling_thread); rt_list_remove(n); break; }
    }
}
void rt_wqueue_wakeup_all(rt_wqueue_t *q, void *key) {
    q->flag = RT_WQ_FLAG_WAKEUP;
    for (rt_list_t *n = q->waiting_list.next; n != &q->waiting_list; n = n->next) {
        struct rt_wqueue_node *e = rt_list_entry(n, struct rt_wqueue_node, list);
        if (e->wakeup(e,key) == 0 && rt_thread_resume(e->polling_thread) != RT_EOK) resume_failures++;
    }
}
/* rt_object_init() has just made a device findable. Another thread could run
 * here unless the registering thread is unpreemptible; model that thread as a
 * devfs open, which takes the raw rt_device path when fops is still NULL. */
static atomic_int check_publication, publications_checked;
void test_object_published(struct rt_object *object) {
    if (!check_publication || strncmp(object->name,"pt",2) || !strcmp(object->name,"ptmx")) return;
    if (test_critical_depth == 0) {
        rt_device_t d = rt_device_find(object->name);
        if (!d || !d->fops) {
            fprintf(stderr,"%s is visible to devfs at a preemption point without fops\n",object->name);
            abort();
        }
    }
    publications_checked++;
}
static int open_file(struct dfs_fd *f, const char *path) {
    memset(f,0,sizeof(*f)); f->ref_count=1;
    return dfs_file_open(f,path,O_RDWR|O_NOCTTY|O_NONBLOCK);
}
static int open_file_flags(struct dfs_fd *f, const char *path, int flags) {
    memset(f,0,sizeof(*f)); f->ref_count=1;
    return dfs_file_open(f,path,flags);
}
static void close_file(struct dfs_fd *f) { assert(dfs_file_close(f)==0); }
static int number(struct dfs_fd *m) { int n=-1; assert(dfs_file_ioctl(m,TIOCGPTN,&n)==0); return n; }
static void slave_path(struct dfs_fd *m, char *path) { snprintf(path,32,"/dev/pts%d",number(m)); }
static void unlock(struct dfs_fd *m) { int v=0; assert(dfs_file_ioctl(m,TIOCSPTLCK,&v)==0); }
static void raw(struct dfs_fd *f) {
    struct termios t; assert(dfs_file_ioctl(f,TCGETS,&t)==0);
    t.c_iflag=t.c_oflag=t.c_lflag=0;
    assert(dfs_file_ioctl(f,TCSETS,&t)==0);
}
static int (*saved_receive_buf)(struct tty_struct *, char *, int);
static int partial_then_hangup(struct tty_struct *tty, char *buf, int size) {
    int accepted=size < 3 ? size : 3;
    tty->other_struct->pty_hungup=1;
    return accepted;
}
static void roundtrip(struct dfs_fd *m, struct dfs_fd *s, char ch) {
    char got=0;
    assert(dfs_file_write(m,&ch,1)==1);
    assert(dfs_file_read(s,&got,1)==1 && got==ch);
    ch++; assert(dfs_file_write(s,&ch,1)==1);
    assert(dfs_file_read(m,&got,1)==1 && got==ch);
}
static void read_exact(struct dfs_fd *f, char *buf, size_t count) {
    size_t done=0;
    while(done<count) { int n=dfs_file_read(f,buf+done,count-done); assert(n>0); done+=n; }
}
static void no_pairs(void) {
    for(int i=0;i<10;i++) {
        char name[12]; snprintf(name,sizeof(name),"pts%d",i); assert(!rt_device_find(name));
        snprintf(name,sizeof(name),"ptm%d",i); assert(!rt_device_find(name));
    }
}
static void sequential(void) {
    for(int i=0;i<1000;i++) { struct dfs_fd m; assert(open_file(&m,"/dev/ptmx")==0); close_file(&m); }
    no_pairs(); puts("PASS: 1000 sequential master opens/closes reclaim all slots");
}
static void routing(void) {
    struct dfs_fd m[10],s[10],extra,again; char paths[10][32],got;
    for(int i=0;i<10;i++) {
        assert(open_file(&m[i],"/dev/ptmx")==0);
        assert(number(&m[i])==i); slave_path(&m[i],paths[i]);
        assert(open_file(&extra,paths[i])==-EIO);
        unlock(&m[i]); assert(open_file(&s[i],paths[i])==0); raw(&s[i]);
    }
    assert(open_file(&extra,"/dev/ptmx")==-ENOSPC);
    for(int i=0;i<10;i++) roundtrip(&m[i],&s[i],'A'+i*2);
    assert(open_file(&again,paths[0])==0);
    close_file(&s[0]); roundtrip(&m[0],&again,'x');
    /* DFS aliases retain the same file, and only the final put closes it. */
    m[0].ref_count=2; close_file(&m[0]); assert(m[0].data); m[0].ref_count=1;
    roundtrip(&m[0],&again,'q'); close_file(&m[0]);
    assert(dfs_file_read(&again,&got,1)==0);
    assert(dfs_file_write(&again,"x",1)==-EIO);
    assert(again.fnode->fops->poll(&again,NULL)&POLLHUP);
    assert(open_file(&extra,paths[0])==-EIO);
    assert(open_file(&extra,"/dev/ptmx")==-ENOSPC);
    close_file(&again); assert(open_file(&extra,"/dev/ptmx")==0); close_file(&extra);
    for(int i=1;i<10;i++) {
        close_file(&s[i]); assert(dfs_file_read(&m[i],&got,1)==0);
        assert(open_file(&again,paths[i])==0); roundtrip(&m[i],&again,'0');
        close_file(&m[i]); close_file(&again);
    }
    no_pairs(); puts("PASS: isolated pairs, slave lock, capacity, aliases, slave reopen and hangup");
}
static void canonical(void) {
    struct dfs_fd m,s; char path[32],line[201],got[201];
    memset(line,'x',sizeof(line)); line[200]='\n';
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0);
    struct termios term; assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_iflag=0; assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    for(int i=0;i<12;i++) {
        assert(dfs_file_write(&m,line,sizeof(line))==sizeof(line));
        int n=dfs_file_read(&s,got,sizeof(got));
        if(n!=sizeof(line) || memcmp(line,got,n)) fprintf(stderr,"canonical iteration=%d read=%d expected=%zu\n",i,n,sizeof(line));
        assert(n==sizeof(line) && !memcmp(line,got,n));
    }
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: canonical lines cross bitmap words and ring-buffer wrap");
}
static void hungup_output(void) {
    struct dfs_fd m,s; char path[32],fill[RT_TTY_BUF-1],drain[RT_TTY_BUF-1]; struct termios term;
    memset(fill,'f',sizeof(fill));
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); raw(&s);
    assert(dfs_file_write(&s,fill,sizeof(fill))==sizeof(fill));
    assert(dfs_file_write(&s,"z",1)==-EAGAIN);
    assert(dfs_file_read(&m,drain,sizeof(drain))==sizeof(drain));
    assert(dfs_file_write(&s,"z",1)==1);
    close_file(&m); close_file(&s); no_pairs();

    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_iflag=term.c_lflag=0; term.c_oflag=OPOST|ONLCR|TAB3;
    assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    assert(dfs_file_write(&s,fill,sizeof(fill))==sizeof(fill));
    assert(!(s.fnode->fops->poll(&s,NULL)&POLLOUT));
    assert(dfs_file_write(&s,"z",1)==-EAGAIN);
    assert(dfs_file_read(&m,drain,1)==1);
    assert(s.fnode->fops->poll(&s,NULL)&POLLOUT);
    read_exact(&m,drain,sizeof(drain)-1);
    assert(dfs_file_write(&s,"\t",1)==1);
    assert(dfs_file_read(&m,drain,sizeof(drain))>0);
    assert(dfs_file_write(&s,"\r",1)==1);
    assert(dfs_file_read(&m,drain,1)==1 && drain[0]=='\r');
    assert(dfs_file_write(&s,fill,sizeof(fill)-1)==sizeof(fill)-1);
    s.flags&=~O_NONBLOCK;
    assert(dfs_file_write(&s,"\n",1)==-EINTR);
    s.flags|=O_NONBLOCK;
    read_exact(&m,drain,sizeof(drain)-1);
    assert(dfs_file_write(&s,fill,sizeof(fill))==sizeof(fill));
    s.flags&=~O_NONBLOCK;
    assert(dfs_file_write(&s,"z",1)==-EINTR);
    s.flags|=O_NONBLOCK;
    read_exact(&m,drain,sizeof(drain));
    close_file(&m); close_file(&s); no_pairs();

    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_iflag=term.c_oflag=0; term.c_lflag=ECHO;
    assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    assert(dfs_file_write(&s,fill,sizeof(fill)-1)==sizeof(fill)-1);
    assert(dfs_file_write(&m,"ab",2)==2);
    assert(dfs_file_write(&s,"x",1)==-EAGAIN);
    assert(dfs_file_read(&m,drain,sizeof(drain))==sizeof(drain));
    assert(drain[sizeof(drain)-1]=='a');
    assert(dfs_file_read(&m,drain,1)==1 && drain[0]=='b');
    assert(dfs_file_write(&s,"x",1)==1);
    assert(dfs_file_read(&m,drain,1)==1 && drain[0]=='x');
    close_file(&m); close_file(&s); no_pairs();

    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_iflag=term.c_lflag=0; term.c_oflag=OPOST;
    assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    saved_receive_buf=((struct tty_struct *)m.data)->ldisc->ops->receive_buf;
    ((struct tty_struct *)m.data)->ldisc->ops->receive_buf=partial_then_hangup;
    assert(dfs_file_write(&s,"abcdef",6)==3);
    ((struct tty_struct *)m.data)->ldisc->ops->receive_buf=saved_receive_buf;
    close_file(&m); close_file(&s); no_pairs();

    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_oflag=OPOST|TAB3; assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    close_file(&m);
    assert(rt_device_write((rt_device_t)s.data,-1,"x",1)==0);
    assert(dfs_file_write(&s,"\t",1)==-EIO);
    assert(dfs_file_write(&s,"plain",5)==-EIO);
    close_file(&s); no_pairs();

    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_iflag=term.c_oflag=0; term.c_lflag=ECHO;
    assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    assert(dfs_file_write(&m,"e",1)==1); close_file(&m);
    assert(dfs_file_write(&s,"x",1)==-EIO);
    close_file(&s); no_pairs();
    puts("PASS: live-full raw/OPOST/echo return EAGAIN without duplicate echo; blocking write waits; poll tracks room; partial hangup and EIO propagate");
}
static void rollback(void) {
    for(int n=0;n<12;n++) {
        struct dfs_fd m; fail_after=n;
        int r=open_file(&m,"/dev/ptmx"); fail_after=-1;
        assert(r==0 || r==-ENOMEM);
        if(r==0) close_file(&m);
        no_pairs(); assert(open_file(&m,"/dev/ptmx")==0); close_file(&m);
    }
    struct rt_device conflict={0}; struct dfs_fd m;
    /* A devfs lookup of slot 0's slave from before its release. */
    assert(open_file(&m,"/dev/ptmx")==0); rt_device_t stale=rt_device_find("pts0"); assert(stale); close_file(&m);
    check_publication=0;
    assert(rt_device_register(&conflict,"pts0",RT_DEVICE_FLAG_RDWR)==0);
    check_publication=1;
    assert(open_file(&m,"/dev/ptmx")==-EIO);
    /* The failed slave registration must not expose a NULL-fops endpoint to it. */
    assert(stale!=&conflict && stale->fops && stale->fops->open);
    assert(!rt_device_find("ptm0")); rt_device_unregister(&conflict);
    assert(open_file(&m,"/dev/ptmx")==0); close_file(&m); no_pairs();
    puts("PASS: allocation failures and device-name collision roll back without leaks");
}
static void session(void) {
    struct dfs_fd m,s; char path[32]; struct rt_lwp process={.leader=1,.session=7,.__pgrp=7};
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m); assert(open_file(&s,path)==0);
    test_pids[0].data=&process; test_current=&process;
    assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0); assert(process.tty);
    assert(tty_push(&process.tty->head,&process)==0);
    close_file(&m); close_file(&s); assert(!process.tty);
    test_current=NULL; test_pids[0].data=NULL; no_pairs();
    puts("PASS: controlling-process pointers and foreground stack detached before slot reuse");
}
int pty_test_setsid(void);
static void session_reset(void) {
    struct dfs_fd m,s,claimed; char path[32];
    struct rt_lwp leader={.leader=1,.session=7,.__pgrp=7};
    struct rt_lwp member={.session=7,.__pgrp=8};
    struct rt_lwp reused={.leader=1,.session=9,.__pgrp=9};
    struct rt_lwp next={.leader=1,.session=11,.__pgrp=11};
    struct rt_lwp race_leader={.leader=1,.session=13,.__pgrp=13};
    struct rt_lwp race_child={.session=13,.__pgrp=14};
    struct rt_lwp setsid_leader={.pid=15,.leader=1,.session=15,.__pgrp=15};
    struct rt_lwp setsid_member={.session=15,.__pgrp=16};
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); test_pids[0].data=&leader; test_pids[1].data=&member;
    test_current=&leader; assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0); test_current=NULL;
    struct tty_struct *slave=leader.tty,*master=slave->other_struct;
    member.tty=slave; assert(tty_push(&slave->head,&leader)==0);
    assert(tty_push(&master->head,&member)==0);
    slave->foreground=master->foreground=&member;
    master->session=7; master->pgrp=8;
    tty_release_lwp(&leader);
    assert(!leader.tty && !member.tty && !slave->foreground && !master->foreground);
    assert(!slave->head && !master->head && slave->session==-1 && master->session==-1);
    assert(slave->pgrp==-1 && master->pgrp==-1);

    /* A recycled session id must not make TIOCSCTTY return success without
     * attaching the caller. */
    slave->session=reused.session;
    test_pids[2].data=&reused; test_current=&reused;
    assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0 && reused.tty==slave);
    test_current=NULL; tty_release_lwp(&reused); assert(slave->session==-1);

    test_pids[3].data=&next; test_current=&next;
    assert(open_file_flags(&claimed,path,O_RDWR|O_NONBLOCK)==0);
    assert(next.tty==slave && slave->session==11 && slave->foreground==&next);
    test_current=NULL; tty_release_lwp(&next); close_file(&claimed);

    test_pids[4].data=&race_leader; test_pids[5].data=&race_child;
    test_current=&race_leader; assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0); test_current=NULL;
    detach_on_alloc=slave;
    assert(tty_push_foreground(&race_leader,&race_child)==0);
    assert(!detach_on_alloc && !race_leader.tty && !race_child.tty);
    assert(slave->session==-1 && !slave->foreground && !master->foreground);

    test_pids[6].data=&setsid_leader; test_pids[7].data=&setsid_member;
    test_current=&setsid_leader; assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0); test_current=NULL;
    setsid_member.tty=slave; test_thread.lwp=&setsid_leader; test_thread.tid=99;
    assert(pty_test_setsid()==-EPERM && setsid_leader.tty==slave);
    test_thread.lwp=NULL;
    tty_release_lwp(&setsid_leader);
    assert(!setsid_leader.tty && !setsid_member.tty && slave->session==-1 && master->session==-1);
    assert(setsid_leader.leader && setsid_leader.session==15 && setsid_leader.__pgrp==15);
    for(int i=0;i<8;i++) test_pids[i].data=NULL;
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: PTY leader exit detaches both endpoints and every member; a new session reclaims the slave");
}
static void foreground(void) {
    struct tty_struct tty={0}; struct tty_node sentinel;
    struct rt_lwp parent={.tty=&tty}, first={.tty=&tty}, second={.tty=&tty};
    tty_initstack(&sentinel); tty.head=&sentinel;
    assert(tty_push(&tty.head,&parent)==0);
    assert(tty_push(&tty.head,&first)==0); tty.foreground=&second;
    tty_release_lwp(&first); assert(!first.tty && tty.foreground==&second);
    tty_release_lwp(&second); assert(!second.tty && tty.foreground==&parent);
    tty_release_lwp(&parent); assert(!parent.tty && !tty.foreground);
    assert(tty.head==&sentinel && tty_pop(&tty.head,&first)==NULL);
    parent.tty=first.tty=&tty; first.session=7;
    assert(tty_push(&tty.head,&parent)==0); tty.foreground=&first;
    first.pid=29; first.__pgrp=7;
    test_thread.lwp=&first;test_thread.tid=99;
    assert(pty_test_setsid()==29); assert(!first.tty && tty.foreground==&parent);
    assert(first.leader && first.session==29 && first.__pgrp==29);
    tty_release_lwp(&parent);test_thread.lwp=NULL;
    assert(!tty.foreground && tty.head==&sentinel);
    struct tty_struct console={0}; struct rt_lwp old={0},child={0};
    console.foreground=&old; fail_after=0;
    assert(tty_push_foreground_tty(&console,&child)==-ENOMEM);
    assert(!child.tty && console.foreground==&old && !console.head);
    fail_after=-1; assert(tty_push_foreground_tty(&console,&child)==0);
    assert(child.tty==&console && console.foreground==&child);
    tty_release_lwp(&child); assert(console.foreground==&old && !console.head);
    puts("PASS: real setsid restores console owner; out-of-order exit removes only its own stack entry");
}
static struct rt_lwp *spawn(int slot, int session, int pgrp, int leader) {
    struct rt_lwp *lwp=calloc(1,sizeof(*lwp)); assert(lwp && !test_pids[slot].data);
    lwp->pid=100+slot;
    lwp->session=session; lwp->__pgrp=pgrp; lwp->leader=leader; test_pids[slot].data=lwp;
    return lwp;
}
/* lwp_free(): release the terminal, leave the PID table, free the process. */
static void reap(int slot) {
    struct rt_lwp *lwp=test_pids[slot].data;
    if (lwp->tty) tty_release_lwp(lwp);
    test_pids[slot].data=NULL; free(lwp);
}
static void pipeline(void) {
    struct dfs_fd m,s; char path[32], intr=3;
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m); assert(open_file(&s,path)==0);
    struct rt_lwp *shell=spawn(0,7,7,1);
    test_current=shell; assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0); test_current=NULL;
    struct tty_struct *tty=shell->tty, *master=tty->other_struct;
    assert(tty && tty->foreground==shell && master->foreground==shell);
    /* fork() copies the terminal without taking the foreground. */
    struct rt_lwp *member=spawn(3,7,7,0); member->tty=tty;
    /* Two foreground execs from the shell stack it twice: [shell, shell]. */
    struct rt_lwp *c1=spawn(1,7,8,0), *c2=spawn(2,7,9,0), *c3=spawn(4,7,10,0);
    assert(tty_push_foreground(shell,c1)==0 && c1->tty==tty && tty->foreground==c1 && tty->pgrp==8);
    assert(tty_push_foreground(shell,c2)==0 && c2->tty==tty && tty->foreground==c2 && tty->pgrp==9);
    fail_after=0;
    assert(tty_push_foreground(shell,c3)==-ENOMEM && c3->tty==tty && tty->foreground==c2);
    fail_after=-1;
    reap(4); reap(2); assert(tty->foreground==shell);
    reap(1); assert(tty->foreground==shell);
    reap(0); assert(!tty->foreground && !tty->head && !master->foreground);
    /* ^C through the master reaches the slave's ISIG handling. */
    assert(dfs_file_write(&m,&intr,1)==1);
    /* Pair release clears the surviving member's terminal; it can still exec. */
    close_file(&m); close_file(&s); no_pairs(); assert(!member->tty);
    struct rt_lwp *orphan=spawn(1,7,11,0); orphan->tty=(void *)1;
    assert(tty_push_foreground(member,orphan)==0 && !orphan->tty);
    reap(1); reap(3);
    /* A setsid() daemon without a terminal starts a foreground child. */
    struct rt_lwp *daemon=spawn(0,7,7,0), *child=spawn(1,-1,0,0);
    test_thread.lwp=daemon; test_thread.tid=42;
    assert(pty_test_setsid()==daemon->pid && daemon->session==daemon->pid && !daemon->tty); test_thread.lwp=NULL;
    child->session=daemon->session; child->tty=(void *)1;
    assert(tty_push_foreground(daemon,child)==0 && !child->tty);
    assert(tty_push_foreground(NULL,child)==0 && !child->tty);
    reap(1); reap(0);
    puts("PASS: session pipeline exits hand foreground back; members without a terminal exec safely");
}
/* A blocked n_tty_read (__wqueue_default_wake) and a sleeping poll()
 * (__wqueue_pollwake) on one endpoint. */
static int default_wake(struct rt_wqueue_node *w, void *key) { return 0; }
static int poll_sleeping, poll_triggered;
static int poll_wake(struct rt_wqueue_node *w, void *key) {
    if (key && !((rt_ubase_t)key & w->key)) return -1;
    poll_triggered++;
    if (!poll_sleeping) return -1;
    poll_sleeping = 0; return 0;
}
static void shared_queue(void) {
    struct dfs_fd m,s; char path[32], got[2];
    for (int hangup=0; hangup<2; hangup++) {
        assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
        assert(open_file(&s,path)==0); raw(&s);
        struct tty_struct *slave=s.data;
        struct rt_thread reader_thread={.stat=RT_THREAD_SUSPEND_MASK}, poll_thread={.stat=RT_THREAD_SUSPEND_MASK};
        struct rt_wqueue_node reader={.polling_thread=&reader_thread,.wakeup=default_wake};
        struct rt_wqueue_node poller={.polling_thread=&poll_thread,.wakeup=poll_wake,.key=POLLIN|POLLHUP};
        poll_sleeping=1; poll_triggered=0; resume_failures=0;
        rt_list_insert_before(&slave->wait_queue.waiting_list,&reader.list);
        rt_list_insert_before(&slave->wait_queue.waiting_list,&poller.list);
        /* Neither woken thread runs before the second event arrives. */
        assert(dfs_file_write(&m,"z",1)==1);
        assert(!reader_thread.stat && !poll_thread.stat && poll_triggered==1);
        if (hangup) close_file(&m); else assert(dfs_file_write(&m,"y",1)==1);
        assert(resume_failures==0 && poll_triggered==2);
        /* Each thread runs and leaves the queue (_rt_wqueue_wait/poll_teardown). */
        rt_list_remove(&reader.list); rt_list_remove(&poller.list);
        if (hangup) { assert(dfs_file_read(&s,got,2)==1 && got[0]=='z'); }
        else { assert(dfs_file_read(&s,got,2)==2 && got[0]=='z' && got[1]=='y'); close_file(&m); }
        close_file(&s); no_pairs();
    }
    {
        struct dfs_fd m,s; char path[32],fill[RT_TTY_BUF-1],got;
        struct rt_thread writer_thread={.stat=RT_THREAD_SUSPEND_MASK};
        struct rt_wqueue_node writer={.polling_thread=&writer_thread,.wakeup=poll_wake,.key=POLLOUT};
        memset(fill,'w',sizeof(fill));
        assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
        assert(open_file(&s,path)==0); raw(&s);
        assert(dfs_file_write(&s,fill,sizeof(fill))==sizeof(fill));
        poll_sleeping=1; poll_triggered=0;
        rt_list_insert_before(&((struct tty_struct *)s.data)->write_wait_queue.waiting_list,&writer.list);
        assert(dfs_file_read(&m,&got,1)==1 && got=='w');
        assert(!writer_thread.stat && poll_triggered==1);
        rt_list_remove(&writer.list);
        close_file(&m); close_file(&s); no_pairs();
    }
    puts("PASS: input/hangup wake readers and pollers once; peer reads wake blocked PTY writers");
}
static void *worker(void *p) {
    for(int i=0;i<250;i++) {
        struct dfs_fd m,s; char path[32];
        assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
        assert(open_file(&s,path)==0); raw(&s); roundtrip(&m,&s,'a'+(intptr_t)p);
        if(i&1) {close_file(&m);close_file(&s);} else {close_file(&s);close_file(&m);}
    }
    return NULL;
}
/* --- Review fixes, 2026-11-17 ------------------------------------------ */
extern struct termios tty_std_termios;
_Thread_local int (*test_wait_hook)(struct rt_wqueue *q);
static struct dfs_fd *drain_fd; static size_t drained;
static int drain_peer(struct rt_wqueue *q) {
    char buf[RT_TTY_BUF]; int n;
    while((n=dfs_file_read(drain_fd,buf,sizeof(buf)))>0) drained+=n;
    return 0;
}
/* A blocking write returns only when all of it is written; a signal ends it
 * with the count so far, or -EINTR (not RT-Thread's -RT_EINTR, EBADF's
 * number) when nothing was written. */
static void blocking_write(void) {
    struct dfs_fd m,s; char path[32],buf[RT_TTY_BUF]; static char big[3*RT_TTY_BUF]; int n;
    memset(big,'b',sizeof(big));
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); raw(&s);
    s.flags&=~O_NONBLOCK;
    drain_fd=&m; drained=0; test_wait_hook=drain_peer;
    assert(dfs_file_write(&s,big,sizeof(big))==sizeof(big));
    test_wait_hook=NULL;
    while((n=dfs_file_read(&m,buf,sizeof(buf)))>0) drained+=n;
    assert(drained==sizeof(big));
    assert(dfs_file_write(&s,big,sizeof(big))==RT_TTY_BUF-1);
    assert(dfs_file_write(&s,big,1)==-EINTR);
    s.flags|=O_NONBLOCK;
    read_exact(&m,buf,RT_TTY_BUF-1);
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: blocking PTY write completes; a signal returns the partial count or EINTR");
}
/* Echoes that cannot be sent (peer full) are trimmed at the discard
 * watermark instead of letting echo_head lap echo_tail. */
static void echo_stall(void) {
    struct dfs_fd m,s; char path[32],fill[RT_TTY_BUF-1],chunk[64],buf[RT_TTY_BUF]; struct termios term;
    size_t echoed=0; int n;
    memset(fill,'f',sizeof(fill));
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_iflag=term.c_oflag=0; term.c_lflag=ECHO;
    assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    assert(dfs_file_write(&s,fill,sizeof(fill))==sizeof(fill));
    for(int i=0;i<64;i++) {
        for(int j=0;j<64;j++) chunk[j]='a'+(i*64+j)%26;
        assert(dfs_file_write(&m,chunk,sizeof(chunk))==sizeof(chunk));
        assert(dfs_file_read(&s,buf,sizeof(chunk))==sizeof(chunk));
    }
    read_exact(&m,buf,sizeof(fill));
    for(size_t k=0;k<sizeof(fill);k++) assert(buf[k]=='f');
    while((n=dfs_file_read(&m,buf,sizeof(buf)))>0) {
        for(int k=0;k<n;k++) assert(buf[k]>='a' && buf[k]<='z');
        echoed+=n;
    }
    if(!echoed || echoed>RT_TTY_BUF) fprintf(stderr,"echoed=%zu after 4096 stalled echoes\n",echoed);
    assert(echoed>0 && echoed<=RT_TTY_BUF);
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: stalled echoes are trimmed, not overrun");
}
/* A canonical reader whose buffer is full without a newline: the newline
 * (and ^C) the master sends next used to be dropped with the rest, so no
 * line could ever complete. As in Linux, it now overwrites the last slot.
 * (Termios ioctls on the master act on the slave, so the master stays raw
 * and never needs more than one byte of room.) */
static void canonical_overflow(void) {
    struct dfs_fd m,s; char path[32],fill[RT_TTY_BUF+100],line[RT_TTY_BUF]; struct termios term; int n;
    memset(fill,'x',sizeof(fill));
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0);
    assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_iflag=0; term.c_oflag=0; term.c_lflag=ICANON;
    assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    assert(dfs_file_write(&m,fill,sizeof(fill))==sizeof(fill));
    assert(dfs_file_read(&s,line,sizeof(line))==-EAGAIN);
    m.flags&=~O_NONBLOCK;
    assert(dfs_file_write(&m,"\n",1)==1);
    m.flags|=O_NONBLOCK;
    n=dfs_file_read(&s,line,sizeof(line));
    assert(n==RT_TTY_BUF && line[n-1]=='\n' && line[0]=='x');
    assert(dfs_file_read(&s,line,sizeof(line))==-EAGAIN);
    /* The next line is normal again. */
    assert(dfs_file_write(&m,"ok\n",3)==3);
    assert(dfs_file_read(&s,line,sizeof(line))==3 && !memcmp(line,"ok\n",3));
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: canonical overflow keeps the newline and completes the line");
}
/* fork() from a background member: the child shares the terminal but does
 * not take the foreground (it used to, taking ^C from the foreground job).
 * fork() from the foreground stacks it and hands it back on exit
 * (lwp_syscall.c passes tty->foreground as previous and self as parent;
 * main() checks the source). */
static void fork_from_background(void) {
    struct dfs_fd m,s; char path[32]; rt_base_t level;
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m); assert(open_file(&s,path)==0);
    struct rt_lwp *shell=spawn(0,7,7,1);
    test_current=shell; assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0); test_current=NULL;
    struct tty_struct *tty=shell->tty;
    struct rt_lwp *fg=spawn(1,7,8,0), *bg=spawn(2,7,9,0), *child=spawn(3,7,9,0);
    assert(tty_push_foreground(shell,fg)==0 && tty->foreground==fg);
    bg->tty=tty;
    struct tty_node *node=tty_foreground_alloc(); assert(node);
    level=rt_hw_interrupt_disable();
    assert(!tty_foreground_commit_locked(bg->tty,bg->tty->foreground,child,node,bg));
    rt_hw_interrupt_enable(level);
    tty_foreground_free(node);
    assert(child->tty==tty && tty->foreground==fg && tty->pgrp==8);
    reap(3); assert(tty->foreground==fg && tty->pgrp==8);
    /* The foreground job's own child does take it, and gives it back. */
    struct rt_lwp *fgchild=spawn(3,7,8,0);
    node=tty_foreground_alloc(); assert(node);
    level=rt_hw_interrupt_disable();
    assert(tty_foreground_commit_locked(fg->tty,fg->tty->foreground,fgchild,node,fg));
    rt_hw_interrupt_enable(level);
    assert(tty->foreground==fgchild);
    reap(3); assert(tty->foreground==fg && tty->pgrp==8);
    /* A forked member of the foreground group may exec another job. Its
     * exit must restore the actual owner, not the forked member. */
    struct rt_lwp *member=spawn(3,7,8,0), *job=spawn(4,7,8,0);
    member->tty=tty;
    node=tty_foreground_alloc(); assert(node);
    level=rt_hw_interrupt_disable();
    assert(tty_foreground_commit_locked(tty,tty->foreground,job,node,member));
    rt_hw_interrupt_enable(level);
    assert(tty->foreground==job);
    reap(4); assert(tty->foreground==fg && tty->pgrp==8);
    reap(3); assert(tty->foreground==fg && tty->pgrp==8);
    reap(2); assert(tty->foreground==fg && tty->pgrp==8);
    reap(1); assert(tty->foreground==shell);
    reap(0);
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: a background member's forked child stays in the background; the foreground's child takes and returns it");
}
/* tty_open() (serial/console ttys): a failed line-discipline open fails the
 * open, and only the first open of the path sets the discipline up. */
static struct tty_struct fake_tty;
static struct dfs_file_ops fake_fops;
static const struct rt_device_ops fake_ops; /* no callbacks: opens succeed */
static void serial_open(void) {
    struct dfs_fd a,b; int r, failures=0;
    memset(&fake_tty,0,sizeof(fake_tty));
    fake_tty.type=TTY_DRIVER_TYPE_CONSOLE; fake_tty.pgrp=fake_tty.session=-1;
    rt_spin_lock_init(&fake_tty.spinlock); rt_wqueue_init(&fake_tty.wait_queue);
    fake_tty.init_termios=tty_std_termios;
    assert(tty_ldisc_init(&fake_tty)==0);
    fake_tty.parent.ops=&fake_ops;
    assert(rt_device_register(&fake_tty.parent,"ttyT",RT_DEVICE_FLAG_RDWR)==RT_EOK);
    tty_set_fops(&fake_fops); fake_tty.parent.fops=&fake_fops;
    for(int k=0;;k++) {
        fail_after=k; r=open_file(&a,"/dev/ttyT"); fail_after=-1;
        if(r==0) break;
        assert(r==-ENOMEM && !fake_tty.disc_data && !dfs_file_is_open("/dev/ttyT"));
        failures++; assert(k<64);
    }
    assert(failures>0 && fake_tty.disc_data);
    void *ldata=fake_tty.disc_data;
    assert(open_file(&b,"/dev/ttyT")==0 && fake_tty.disc_data==ldata);
    close_file(&b); close_file(&a);
    /* A first open after a full close starts the same discipline afresh
     * (tty_close() keeps it) instead of leaking it. */
    assert(open_file(&a,"/dev/ttyT")==0 && fake_tty.disc_data==ldata);
    close_file(&a);
    assert(rt_device_unregister(&fake_tty.parent)==RT_EOK);
    puts("PASS: tty_open fails on ldisc allocation failure; later opens and reopens keep the discipline");
}
/* OPOST output (pty_write_exact() per expanded character) in a blocking
 * write larger than the peer's buffer: every "\n" arrives as "\r\n". */
static void blocking_opost_write(void) {
    struct dfs_fd m,s; char path[32],buf[RT_TTY_BUF]; static char big[3*RT_TTY_BUF]; struct termios term;
    size_t lines=0; int n;
    for(size_t i=0;i<sizeof(big);i++) { big[i]=(i%16==15)?'\n':'a'+i%16; lines+=big[i]=='\n'; }
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); assert(dfs_file_ioctl(&s,TCGETS,&term)==0);
    term.c_iflag=term.c_lflag=0; term.c_oflag=OPOST|ONLCR;
    assert(dfs_file_ioctl(&s,TCSETS,&term)==0);
    s.flags&=~O_NONBLOCK;
    drain_fd=&m; drained=0; test_wait_hook=drain_peer;
    assert(dfs_file_write(&s,big,sizeof(big))==sizeof(big));
    test_wait_hook=NULL;
    while((n=dfs_file_read(&m,buf,sizeof(buf)))>0) drained+=n;
    assert(drained==sizeof(big)+lines);
    s.flags|=O_NONBLOCK;
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: blocking OPOST write expands every newline and completes");
}
/* The terminal ioctls musl's isatty(), tcgetpgrp(), tcsetpgrp(), tcgetsid(),
 * tcflush(), tcdrain(), tcflow() and openpty(ws) issue. */
static void terminal_ioctls(void) {
    struct dfs_fd m,s; char path[32],buf[64]; struct winsize ws={24,80,0,0},got; int v; struct termios t;
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m);
    assert(open_file(&s,path)==0); raw(&s);
    memset(&got,0xff,sizeof(got));
    assert(dfs_file_ioctl(&s,TIOCGWINSZ,&got)==0 && got.ws_row==0 && got.ws_col==0);
    assert(dfs_file_ioctl(&m,TIOCSWINSZ,&ws)==0);
    assert(dfs_file_ioctl(&s,TIOCGWINSZ,&got)==0 && got.ws_row==24 && got.ws_col==80);
    assert(dfs_file_ioctl(&m,TIOCGWINSZ,&got)==0 && got.ws_col==80);
    /* Unknown terminal ioctls answer ENOTTY; NULL pointers EFAULT. */
    assert(dfs_file_ioctl(&s,TIOCSTI,buf)==-ENOTTY);
    assert(dfs_file_ioctl(&s,TCGETS,NULL)==-EFAULT && dfs_file_ioctl(&s,TCSETS,NULL)==-EFAULT);
    assert(dfs_file_ioctl(&s,TIOCGWINSZ,NULL)==-EFAULT);
    /* FIONREAD and tcflush(). */
    assert(dfs_file_write(&m,"abc",3)==3);
    assert(dfs_file_ioctl(&s,FIONREAD,&v)==0 && v==3);
    assert(dfs_file_ioctl(&s,TCFLSH,(void *)TCIFLUSH)==0);
    assert(dfs_file_ioctl(&s,FIONREAD,&v)==0 && v==0 && dfs_file_read(&s,buf,sizeof(buf))==-EAGAIN);
    assert(dfs_file_write(&s,"xy",2)==2);
    assert(dfs_file_ioctl(&m,FIONREAD,&v)==0 && v==2);
    assert(dfs_file_ioctl(&s,TCFLSH,(void *)TCOFLUSH)==0);
    assert(dfs_file_ioctl(&m,FIONREAD,&v)==0 && v==0);
    assert(dfs_file_ioctl(&s,TCFLSH,(void *)7)==-EINVAL);
    /* tcsetattr(TCSAFLUSH) discards pending input. */
    assert(dfs_file_write(&m,"zz",2)==2);
    assert(dfs_file_ioctl(&s,TCGETS,&t)==0 && dfs_file_ioctl(&s,TCSETSF,&t)==0);
    assert(dfs_file_ioctl(&s,FIONREAD,&v)==0 && v==0);
    /* FIONBIO, tcdrain(), tcflow(). */
    v=0; assert(dfs_file_ioctl(&s,FIONBIO,&v)==0 && !(s.flags&O_NONBLOCK));
    v=1; assert(dfs_file_ioctl(&s,FIONBIO,&v)==0 && (s.flags&O_NONBLOCK));
    assert(dfs_file_ioctl(&s,TCSBRK,(void *)1)==0);
    assert(dfs_file_ioctl(&s,TCXONC,(void *)TCOOFF)==-EOPNOTSUPP);
    assert(dfs_file_ioctl(&s,TCXONC,(void *)9)==-EINVAL);
    assert(dfs_file_ioctl(&s,TCXONC,(void *)TCIOFF)==0);
    assert(dfs_file_read(&m,buf,1)==1 && (unsigned char)buf[0]==t.c_cc[VSTOP]);
    /* Job control on the controlling terminal. */
    struct rt_lwp *shell=spawn(0,7,7,1);
    test_current=shell; assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0);
    struct tty_struct *tty=shell->tty;
    assert(dfs_file_ioctl(&s,TIOCGPGRP,&v)==0 && v==7);
    assert(dfs_file_ioctl(&s,TIOCGSID,&v)==0 && v==7);
    struct rt_lwp *job=spawn(1,7,9,0); job->tty=tty;
    v=9; assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==0 && tty->pgrp==9 && tty->foreground==job);
    v=42; assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==-EPERM && tty->pgrp==9);
    /* Not to an exited (zombie) member, nor one without this terminal: the
     * foreground would outlive it. */
    struct rt_lwp *zombie=spawn(5,7,11,0); zombie->tty=tty; zombie->finish=1;
    v=11; assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==-EPERM && tty->foreground==job);
    zombie->finish=0; zombie->tty=NULL;
    assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==-EPERM && tty->foreground==job);
    zombie->tty=tty;
    assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==0 && tty->foreground==zombie);
    v=9; assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==0 && tty->foreground==job);
    reap(5);
    v=-1; assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==-EINVAL);
    struct rt_lwp *stranger=spawn(2,8,8,1);
    test_current=stranger;
    assert(dfs_file_ioctl(&s,TIOCGPGRP,&v)==-ENOTTY && dfs_file_ioctl(&s,TIOCGSID,&v)==-ENOTTY);
    v=8; assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==-ENOTTY);
    test_current=NULL;
    assert(dfs_file_ioctl(&m,TIOCGPGRP,&v)==0 && v==9);
    reap(1); assert(tty->foreground!=job);
    test_current=shell;
    v=7; assert(dfs_file_ioctl(&s,TIOCSPGRP,&v)==0 && tty->foreground==shell && tty->pgrp==7);
    test_current=NULL;
    reap(2); reap(0);
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: winsize, ENOTTY/EFAULT, FIONREAD, tcflush, TCSAFLUSH, FIONBIO, tcflow, tcgetpgrp/tcsetpgrp/tcgetsid");
}
int test_kills[16], test_kill_count;
static int killed(int pid) {
    for (int i=0;i<test_kill_count && i<16;i++) if (test_kills[i]==pid) return 1;
    return 0;
}
/* ^C reaches every live process of the foreground group on this terminal:
 * the shell and the child it forked (fork() keeps the group and leaves the
 * foreground alone), not a process of another group, an exited member, or a
 * group member on another terminal. */
static void isig_group(void) {
    struct dfs_fd m,s; char path[32], intr; struct termios t;
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m); assert(open_file(&s,path)==0);
    assert(dfs_file_ioctl(&s,TCGETS,&t)==0); t.c_lflag|=ISIG; assert(dfs_file_ioctl(&s,TCSETS,&t)==0);
    intr=t.c_cc[VINTR];
    struct rt_lwp *shell=spawn(0,7,7,1);
    test_current=shell; assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0); test_current=NULL;
    struct tty_struct *tty=shell->tty;
    struct rt_lwp *child=spawn(1,7,7,0), *other=spawn(2,7,9,0), *dead=spawn(3,7,7,0), *away=spawn(4,7,7,0);
    child->tty=tty; other->tty=tty; dead->tty=tty; dead->finish=1;
    assert(tty->foreground==shell && tty->pgrp==7);
    test_kill_count=0;
    assert(dfs_file_write(&m,&intr,1)==1);
    assert(killed(100) && killed(101) && !killed(102) && !killed(103) && !killed(104));
    assert(test_kill_count==2);
    reap(4); reap(3); reap(2); reap(1); reap(0);
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: ^C signals the foreground process group (shell and forked child), nothing else");
}
static void background_access(void) {
    struct dfs_fd m,s; char path[32]; struct termios t;
    assert(open_file(&m,"/dev/ptmx")==0); slave_path(&m,path); unlock(&m); assert(open_file(&s,path)==0);
    struct rt_lwp *fg=spawn(0,7,7,1), *bg=spawn(1,7,9,0), *member=spawn(2,7,9,0);
    test_current=fg; assert(dfs_file_ioctl(&s,TIOCSCTTY,NULL)==0);
    bg->tty=fg->tty; member->tty=fg->tty;
    assert(dfs_file_ioctl(&s,TCGETS,&t)==0);
    t.c_lflag|=TOSTOP; assert(dfs_file_ioctl(&s,TCSETS,&t)==0);
    test_current=bg; test_kill_count=0;
    assert(dfs_file_write(&s,"x",1)==-EINTR);
    assert(killed(bg->pid) && killed(member->pid) && !killed(fg->pid));
    test_current=fg; t.c_lflag&=~TOSTOP; assert(dfs_file_ioctl(&s,TCSETS,&t)==0);
    test_current=bg; test_kill_count=0;
    assert(dfs_file_write(&s,"x",1)==1 && test_kill_count==0);
    test_current=NULL; reap(2); reap(1); reap(0);
    close_file(&m); close_file(&s); no_pairs();
    puts("PASS: background writes signal their own group with TOSTOP; without TOSTOP they write normally");
}
int main(void) {
    pthread_mutexattr_t attr; pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&irq_lock,&attr);
    pthread_mutexattr_destroy(&attr);
    dfs_fnode_mgr_init(); devfs_init(); n_tty_init(); assert(test_ptmx_register()==0);
    check_publication=1;
    sequential(); routing(); canonical(); hungup_output(); rollback(); session(); session_reset(); foreground(); pipeline(); shared_queue();
    blocking_write(); echo_stall(); canonical_overflow(); fork_from_background(); serial_open(); background_access();
    blocking_opost_write(); terminal_ioctls(); isig_group();
    pthread_t threads[6];
    for(int i=0;i<6;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(intptr_t)i));
    for(int i=0;i<6;i++) pthread_join(threads[i],NULL);
    no_pairs(); puts("PASS: 1500 concurrent pairs transfer data and reclaim both close orders");
    assert(publications_checked >= 2*(1000+1500));
    printf("PASS: %d PTY endpoint publications were unpreemptible until fops was set\n",(int)publications_checked);
}
'''


def termios_defines(text):
    values = {}
    for name, value in re.findall(r'^#define\s+(\w+)\s+(0[0-7]*|0x[0-9a-fA-F]+|[1-9]\d*)\b', text, re.M):
        values[name] = int(value, 16) if value.startswith('0x') else int(value, 8 if value.startswith('0') else 10)
    return values


def check_termios_abi():
    """posix_termios.h must use the values userapps compile with."""
    sysroot = Path(os.environ.get('K230_MUSL_INCLUDE', Path.home() /
                   '.kendryte/k230_toolchains/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/riscv64-unknown-linux-musl/include'))
    headers = [sysroot / 'bits/termios.h', sysroot / 'termios.h']
    if not all(h.exists() for h in headers):
        print(f'SKIP: termios ABI check, no musl headers under {sysroot}')
        return
    musl = {}
    for h in headers:
        musl.update(termios_defines(h.read_text()))
    kernel = termios_defines((TTY / 'include/posix_termios.h').read_text())
    common = sorted(set(kernel) & set(musl))
    differ = [(n, kernel[n], musl[n]) for n in common if kernel[n] != musl[n]]
    assert len(common) > 100, len(common)
    assert not differ, differ
    print(f'PASS: {len(common)} termios constants match the toolchain musl headers')


def main():
    lwp_source=(base.RTT/'components/lwp/lwp.c').read_text()
    exec_body=lwp_source[lwp_source.index('pid_t lwp_execve('):]
    assert exec_body.index('foreground_node = tty_foreground_alloc()') < exec_body.index('lwp_tid_set_thread(tid, thread)')
    assert exec_body.index('rt_list_insert_after(&lwp->t_grp, &thread->sibling)') < exec_body.index('tty_foreground_commit_locked(')
    assert 'thread->cleanup = RT_NULL;' in exec_body and 'rt_thread_delete(thread);' in exec_body
    syscall_source=(base.RTT/'components/lwp/lwp_syscall.c').read_text()
    fork_body=syscall_source[syscall_source.index('int _sys_fork(void)'):]
    fork_end=fork_body.index('\n}\n')
    # fork() shares the terminal after the child has a thread, and never
    # takes the foreground (isig_group() runs ^C to the whole group).
    assert fork_body.index('rt_list_insert_after(&lwp->t_grp, &thread->sibling)') < fork_body.index('lwp->tty = self_lwp->tty;')
    assert 'tty_foreground_commit_locked(' not in fork_body[:fork_end]
    assert 'tty_foreground_alloc(' not in fork_body[:fork_end]
    assert 'dst->tty = RT_NULL;' in syscall_source and 'thread->lwp = RT_NULL;' in fork_body
    # fork must not share a slot another thread's fd_new() has not set up.
    copy_files=syscall_source[syscall_source.index('static int lwp_copy_files('):syscall_source.index('int _sys_fork(void)')]
    assert 'if (d_s && d_s->fnode)' in copy_files
    print('PASS: exec allocates the terminal stack before linking children; fork leaves the foreground alone; unstarted threads are deleted on failure')
    check_termios_abi()
    with tempfile.TemporaryDirectory(prefix='k230-pty-test-') as directory:
        build = Path(directory)
        (build/'rtconfig.h').write_text(base.RTCONFIG)
        (build/'rtdef_values.h').write_text(base.rtdef_defines())
        stubs=base.STUBS.replace('#include <assert.h>', '#include <pthread.h>\n#include <semaphore.h>\n#include <assert.h>')
        start=stubs.index('struct rt_mutex {')
        end=stubs.index('extern rt_err_t test_errno;',start)
        stubs=stubs[:start]+SYNC+stubs[end:]+EXTRA
        no_op_wakeup='static inline void rt_wqueue_wakeup(rt_wqueue_t *q, void *key) {}'
        assert no_op_wakeup in stubs
        stubs=stubs.replace(no_op_wakeup,'void rt_wqueue_wakeup(rt_wqueue_t *q, void *key);')
        mask=re.search(r'^#define\s+(RT_THREAD_SUSPEND_MASK)\s+(0x[0-9a-fA-F]+)',(base.RTT/'include/rtdef.h').read_text(),re.M)
        stubs+=f'#define {mask[1]} {mask[2]}\n'
        (build/'stubs.h').write_text(stubs)
        for name in ('rtthread.h','rthw.h','rtdbg.h','rtdevice.h','dfs_poll.h','lwp.h','lwp_pid.h'):
            (build/name).write_text('#include "stubs.h"\n')
        (build/'dfs_poll.h').write_text('#include \"stubs.h\"\n#include <dfs_file.h>\n')
        (build/'dfs_posix.h').write_text('#include <dfs_file.h>\n')
        env=base.ENV.replace('void dfs_fd_lock(void) {}','').replace('void dfs_fd_unlock(void) {}','')
        env=env.replace('    strncpy(object->name, name, RT_NAME_MAX);','    rt_enter_critical();\n    strncpy(object->name, name, RT_NAME_MAX);')
        env=env.replace('    rt_list_insert_after(&devices.object_list, &object->list);','    rt_list_insert_after(&devices.object_list, &object->list);\n    rt_exit_critical();\n    test_object_published(object);')
        env=env.replace('{ rt_list_remove(&object->list); }','{ rt_enter_critical(); rt_list_remove(&object->list); rt_exit_critical(); }')
        (build/'env.c').write_text(env)
        (build/'test.c').write_text(CASES)
        sources=[base.RTT/'src/device.c',base.DFS/'src/dfs_file.c',base.DFS/'filesystems/devfs/devfs.c']
        sources += [TTY/name for name in ('pty.c','tty.c','tty_ldisc.c','n_tty.c','tty_ioctl.c')]
        sources += [base.RTT/'components/lwp/lwp_setsid.c', build/'env.c',build/'test.c']
        cc=[os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Dsetsid=pty_test_setsid','-Wno-unused-function','-Wno-unused-variable',
            '-Wno-unused-but-set-variable','-fsanitize=address,undefined','-fno-sanitize-recover=all','-pthread',
            '-I',str(build),'-I',str(base.DFS/'include'),'-I',str(base.DFS/'filesystems/devfs'),'-I',str(TTY/'include')]
        binary=build/'test'
        subprocess.run([*cc,*map(str,sources),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=45)


if __name__ == '__main__':
    main()
