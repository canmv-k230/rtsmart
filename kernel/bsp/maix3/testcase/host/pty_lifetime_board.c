/* Auxiliary RT-Smart userspace regression; build/run instructions are recorded
 * in docs/plans/k230-pty-lifetime-fix-20261110.md in the SDK root. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "hal_syscall.h"
#define CHECK(x) do { if (!(x)) { printf("PTY_FAIL %s:%d %s errno=%d\n",__func__,__LINE__,#x,errno); return -1; } } while (0)

static int master(void) { return open("/dev/ptmx",O_RDWR|O_NOCTTY|O_NONBLOCK); }
static int index_of(int m) { int n=-1; CHECK(ioctl(m,TIOCGPTN,&n)==0); return n; }
static int slave_of(int m) {
    char path[32]; int unlocked=0;
    CHECK(ioctl(m,TIOCSPTLCK,&unlocked)==0);
    int n=index_of(m); CHECK(n>=0); snprintf(path,sizeof(path),"/dev/pts%d",n);
    int s=open(path,O_RDWR|O_NOCTTY|O_NONBLOCK); CHECK(s>=0);
    struct termios t; CHECK(ioctl(s,TCGETS,&t)==0);
    t.c_iflag=t.c_oflag=t.c_lflag=0; t.c_cc[VMIN]=1; t.c_cc[VTIME]=0;
    CHECK(ioctl(s,TCSETS,&t)==0); return s;
}
static int transfer(int m,int s,char ch) {
    char got=0; CHECK(write(m,&ch,1)==1); CHECK(read(s,&got,1)==1 && got==ch);
    ch++; CHECK(write(s,&ch,1)==1); CHECK(read(m,&got,1)==1 && got==ch); return 0;
}
static int read_exact_fd(int fd,char *buf,size_t count) {
    size_t done=0;
    while(done<count) {
        ssize_t n=read(fd,buf+done,count-done);
        if(n<0&&errno==EINTR)continue;
        CHECK(n>0);done+=(size_t)n;
    }
    return 0;
}
static int reuse(int cycles) {
    for(int i=0;i<cycles;i++) { int m=master(); CHECK(m>=0); CHECK(close(m)==0); }
    return 0;
}
static int capacity(void) {
    int m[10],s[10],ids[10]; char path[32],c;
    for(int i=0;i<10;i++) {
        m[i]=master(); CHECK(m[i]>=0); ids[i]=index_of(m[i]); CHECK(ids[i]>=0);
        for(int j=0;j<i;j++) CHECK(ids[j]!=ids[i]);
        snprintf(path,sizeof(path),"/dev/pts%d",ids[i]);
        errno=0; CHECK(open(path,O_RDWR|O_NOCTTY)==-1 && errno==EIO);
        s[i]=slave_of(m[i]); CHECK(s[i]>=0);
    }
    errno=0; CHECK(master()==-1 && errno==ENOSPC);
    for(int i=0;i<10;i++) CHECK(transfer(m[i],s[i],'A'+i*2)==0);
    int alias=dup(m[0]); CHECK(alias>=0); CHECK(close(m[0])==0);
    CHECK(transfer(alias,s[0],'q')==0); CHECK(close(alias)==0);
    CHECK(read(s[0],&c,1)==0); errno=0; CHECK(write(s[0],"x",1)==-1 && errno==EIO);
    struct pollfd p={s[0],POLLIN|POLLOUT,0}; CHECK(poll(&p,1,0)==1 && (p.revents&POLLHUP));
    errno=0; CHECK(master()==-1 && errno==ENOSPC);
    CHECK(close(s[0])==0); int next=master(); CHECK(next>=0); CHECK(close(next)==0);
    for(int i=1;i<10;i++) {
        CHECK(close(s[i])==0); CHECK(read(m[i],&c,1)==0);
        s[i]=slave_of(m[i]); CHECK(s[i]>=0); CHECK(transfer(m[i],s[i],'x')==0);
        CHECK(close(m[i])==0); CHECK(close(s[i])==0);
    }
    return 0;
}
static int canonical(void) {
    int m=master(); CHECK(m>=0); int s=slave_of(m); CHECK(s>=0);
    struct termios t; CHECK(ioctl(s,TCGETS,&t)==0); t.c_lflag=ICANON; CHECK(ioctl(s,TCSETS,&t)==0);
    char line[201],got[201]; memset(line,'x',sizeof(line));line[200]='\n';
    for(int i=0;i<20;i++) {
        CHECK(write(m,line,sizeof(line))==sizeof(line));
        CHECK(read(s,got,sizeof(got))==sizeof(got)); CHECK(!memcmp(line,got,sizeof(line)));
    }
    CHECK(close(m)==0);CHECK(close(s)==0);return 0;
}
struct waiter { int fd,is_poll,result,error; short events; atomic_int entered; };
static void *waiter(void *arg) {
    struct waiter *w=arg; atomic_store(&w->entered,1);
    if(w->is_poll) { struct pollfd p={w->fd,POLLIN,0}; w->result=poll(&p,1,3000);w->events=p.revents; }
    else {char ch;w->result=read(w->fd,&ch,1);}
    w->error=errno; return NULL;
}
static int sleeping(int slave_wait,int close_waiting_fd) {
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    int fd=slave_wait?s:m,peer=slave_wait?m:s;
    CHECK(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)&~O_NONBLOCK)==0);
    struct waiter w[2]={{.fd=fd},{.fd=fd,.is_poll=1}};pthread_t t[2];
    for(int i=0;i<2;i++)CHECK(pthread_create(&t[i],NULL,waiter,&w[i])==0);
    while(!atomic_load(&w[0].entered)||!atomic_load(&w[1].entered))usleep(1000);
    usleep(100000);
    if(close_waiting_fd) CHECK(close(fd)==0);
    CHECK(close(peer)==0);
    for(int i=0;i<2;i++)CHECK(pthread_join(t[i],NULL)==0);
    if(w[0].result!=0 || w[1].result!=1 || !(w[1].events&POLLHUP))
        printf("PTY_WAIT read=%d errno=%d poll=%d events=%x errno=%d\n",w[0].result,w[0].error,w[1].result,w[1].events,w[1].error);
    CHECK(w[0].result==0);CHECK(w[1].result==1 && (w[1].events&POLLHUP));
    if(!close_waiting_fd)CHECK(close(fd)==0);
    return 0;
}
static int wait_child(pid_t child,int killed) {
    int status=0;long ret;
    do {ret=syscall(_NRSYS_waitpid,(long)child,(long)&status,0);} while((ret==-1&&errno==EINTR)||ret==-EINTR);
    CHECK(ret==child);
    if(killed) CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    else CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
static int owner_exit(int killed) {
    int sync[2];CHECK(pipe(sync)==0);fflush(NULL);pid_t pid=fork();CHECK(pid>=0);
    if(pid==0) {
        close(sync[0]);
        for(int i=0;i<3;i++) {int m=master(),s=slave_of(m);if(m<0||s<0||dup(m)<0||dup(s)<0)_exit(1);}
        if(write(sync[1],"R",1)!=1)_exit(1);
        if(killed) { for(;;)pause(); }
        _exit(0);
    }
    close(sync[1]);char ready;CHECK(read(sync[0],&ready,1)==1 && ready=='R');close(sync[0]);
    if(killed)CHECK(kill(pid,SIGKILL)==0);
    CHECK(wait_child(pid,killed)==0);CHECK(capacity()==0);return 0;
}
static int fork_alias(void) {
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    int sync[2];CHECK(pipe(sync)==0);fflush(NULL);pid_t pid=fork();CHECK(pid>=0);
    if(pid==0) {
        close(sync[1]);close(s);char start;
        if(read(sync[0],&start,1)!=1 || write(m,"F",1)!=1)_exit(1);
        _exit(0);
    }
    close(sync[0]);CHECK(close(m)==0);CHECK(write(sync[1],"S",1)==1);close(sync[1]);
    CHECK(wait_child(pid,0)==0);char ch;CHECK(read(s,&ch,1)==1 && ch=='F');CHECK(read(s,&ch,1)==0);
    CHECK(close(s)==0);return 0;
}
static int session_child(void) {
    CHECK(setsid()>=0);
    for(int i=0;i<10;i++) {
        int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
        CHECK(ioctl(s,TIOCSCTTY,0)==0);
        CHECK(close(m)==0);CHECK(close(s)==0);
    }
    return 0;
}
static int session_reclaim(void) {
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);int n=index_of(m);CHECK(n>=0);
    char path[32];snprintf(path,sizeof(path),"/dev/pts%d",n);
    fflush(NULL);pid_t first=fork();CHECK(first>=0);
    if(first==0) {
        if(setsid()<0) {printf("PTY_SESSION first setsid errno=%d\n",errno);_exit(1);}
        if(ioctl(s,TIOCSCTTY,0)!=0) {printf("PTY_SESSION first TIOCSCTTY errno=%d\n",errno);_exit(1);}
        _exit(0);
    }
    CHECK(wait_child(first,0)==0);
    fflush(NULL);pid_t next=fork();CHECK(next>=0);
    if(next==0) {
        if(setsid()<0) {printf("PTY_SESSION next setsid errno=%d\n",errno);_exit(1);}
        if(close(s)!=0) {printf("PTY_SESSION next close errno=%d\n",errno);_exit(1);}
        int claimed=open(path,O_RDWR|O_NONBLOCK);
        if(claimed<0) {printf("PTY_SESSION next open errno=%d\n",errno);_exit(1);}
        if(ioctl(claimed,TIOCSCTTY,0)!=0) {printf("PTY_SESSION next TIOCSCTTY errno=%d\n",errno);_exit(1);}
        if(close(claimed)!=0) {printf("PTY_SESSION next close claimed errno=%d\n",errno);_exit(1);}
        _exit(0);
    }
    CHECK(wait_child(next,0)==0);CHECK(close(m)==0);CHECK(close(s)==0);return 0;
}
static int hungup_output(void) {
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    struct termios t;CHECK(ioctl(s,TCGETS,&t)==0);t.c_oflag=OPOST|TAB3;CHECK(ioctl(s,TCSETS,&t)==0);
    CHECK(close(m)==0);errno=0;CHECK(write(s,"\t",1)==-1&&errno==EIO);
    errno=0;CHECK(write(s,"plain",5)==-1&&errno==EIO);CHECK(close(s)==0);return 0;
}
struct output_writer { int fd,result,error; atomic_int entered; };
static void *blocking_output_writer(void *arg) {
    struct output_writer *w=arg;atomic_store(&w->entered,1);
    w->result=write(w->fd,"\n",1);w->error=errno;return NULL;
}
static int live_full_output(void) {
    char fill[1023],drain[1023];memset(fill,'f',sizeof(fill));
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    struct termios t;CHECK(ioctl(s,TCGETS,&t)==0);
    t.c_iflag=t.c_lflag=0;t.c_oflag=OPOST|ONLCR;CHECK(ioctl(s,TCSETS,&t)==0);
    CHECK(write(s,fill,sizeof(fill)-1)==(ssize_t)sizeof(fill)-1);
    CHECK(fcntl(s,F_SETFL,fcntl(s,F_GETFL)&~O_NONBLOCK)==0);
    struct output_writer w={.fd=s};pthread_t thread;
    CHECK(pthread_create(&thread,NULL,blocking_output_writer,&w)==0);
    while(!atomic_load(&w.entered))usleep(1000);
    usleep(100000);
    CHECK(read(m,drain,1)==1);CHECK(pthread_join(thread,NULL)==0);
    CHECK(w.result==1);CHECK(read_exact_fd(m,drain,sizeof(drain))==0);
    CHECK(close(m)==0);CHECK(close(s)==0);

    m=master();CHECK(m>=0);s=slave_of(m);CHECK(s>=0);
    CHECK(ioctl(s,TCGETS,&t)==0);t.c_iflag=t.c_oflag=0;t.c_lflag=ECHO;
    CHECK(ioctl(s,TCSETS,&t)==0);
    CHECK(write(s,fill,sizeof(fill)-1)==(ssize_t)sizeof(fill)-1);
    CHECK(write(m,"ab",2)==2);errno=0;CHECK(write(s,"x",1)==-1&&errno==EAGAIN);
    CHECK(read_exact_fd(m,drain,sizeof(drain))==0);CHECK(drain[sizeof(drain)-1]=='a');
    CHECK(read(m,drain,1)==1&&drain[0]=='b');
    CHECK(close(m)==0);CHECK(close(s)==0);return 0;
}
/* 2026-11-17 review fixes, as user space sees them through musl. */
struct drain_reader { int fd; size_t total; atomic_int stop; };
static void *drain_reader_main(void *arg) {
    struct drain_reader *r=arg; char buf[256];
    while(!atomic_load(&r->stop)) {
        ssize_t n=read(r->fd,buf,sizeof(buf));
        if(n>0)r->total+=(size_t)n; else usleep(1000);
    }
    return NULL;
}
/* A blocking write returns only once all of it is written. */
static int blocking_full_write(void) {
    static char big[3*1024]; memset(big,'b',sizeof(big));
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    CHECK(fcntl(s,F_SETFL,fcntl(s,F_GETFL)&~O_NONBLOCK)==0);
    struct drain_reader r={.fd=m};pthread_t t;
    CHECK(pthread_create(&t,NULL,drain_reader_main,&r)==0);
    ssize_t n=write(s,big,sizeof(big));
    while(r.total<sizeof(big))usleep(1000);
    atomic_store(&r.stop,1);CHECK(pthread_join(t,NULL)==0);
    CHECK(n==(ssize_t)sizeof(big)&&r.total==sizeof(big));
    CHECK(close(m)==0);CHECK(close(s)==0);return 0;
}
/* A canonical slave whose buffer is full without a newline still completes
 * the line when the master sends one. */
static int canonical_overflow_line(void) {
    char fill[1100],line[1024];memset(fill,'x',sizeof(fill));
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    struct termios t;CHECK(ioctl(s,TCGETS,&t)==0);t.c_lflag=ICANON;CHECK(ioctl(s,TCSETS,&t)==0);
    CHECK(write(m,fill,sizeof(fill))==(ssize_t)sizeof(fill));
    errno=0;CHECK(read(s,line,sizeof(line))==-1&&errno==EAGAIN);
    CHECK(write(m,"\n",1)==1);
    ssize_t n=read(s,line,sizeof(line));CHECK(n==(ssize_t)sizeof(line)&&line[n-1]=='\n');
    CHECK(write(m,"ok\n",3)==3);CHECK(read(s,line,sizeof(line))==3);
    CHECK(close(m)==0);CHECK(close(s)==0);return 0;
}
/* A signal interrupting a blocking PTY write reaches user space as EINTR
 * (RT-Thread's -RT_EINTR has EBADF's number). */
static void on_usr1(int sig) { (void)sig; }
static int interrupted_write(void) {
    char fill[1023];memset(fill,'f',sizeof(fill));
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    CHECK(write(s,fill,sizeof(fill))==(ssize_t)sizeof(fill));
    fflush(NULL);pid_t child=fork();CHECK(child>=0);
    if(child==0) {
        struct sigaction sa;memset(&sa,0,sizeof(sa));sa.sa_handler=on_usr1;
        if(sigaction(SIGUSR1,&sa,NULL)!=0)_exit(2);
        if(fcntl(s,F_SETFL,fcntl(s,F_GETFL)&~O_NONBLOCK)!=0)_exit(3);
        errno=0;ssize_t n=write(s,"z",1);
        _exit(n==-1&&errno==EINTR?0:(n==-1&&errno==EBADF?4:5));
    }
    usleep(200000);CHECK(kill(child,SIGUSR1)==0);
    int status=0;CHECK(waitpid(child,&status,0)==child);
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)
        printf("PTY_FAIL interrupted_write status=%#x (exit 4: EBADF)\n",status);
    CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    CHECK(close(m)==0);CHECK(close(s)==0);return 0;
}
/* musl's own terminal functions, as userapps call them. */
static int musl_terminal_api(void) {
    char buf[64],*name;struct winsize ws={.ws_row=24,.ws_col=80},got;struct termios t;int n;
    int m=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK);CHECK(m>=0);
    CHECK(grantpt(m)==0&&unlockpt(m)==0);CHECK((name=ptsname(m))!=NULL);
    int s=open(name,O_RDWR|O_NOCTTY|O_NONBLOCK);CHECK(s>=0);
    CHECK(isatty(m)&&isatty(s));                 /* TIOCGWINSZ */
    CHECK(ioctl(m,TIOCSWINSZ,&ws)==0&&ioctl(s,TIOCGWINSZ,&got)==0&&got.ws_col==80&&got.ws_row==24);
    errno=0;CHECK(ioctl(s,TIOCSTI,buf)==-1&&errno==ENOTTY);
    CHECK(tcgetattr(s,&t)==0);
    t.c_iflag=0;t.c_lflag=0;t.c_oflag=OPOST|ONLCR;t.c_cc[VMIN]=1;t.c_cc[VTIME]=0;
    CHECK(tcsetattr(s,TCSANOW,&t)==0);
    CHECK(write(s,"ab\n",3)==3);                 /* ONLCR as musl defines it */
    CHECK(read_exact_fd(m,buf,4)==0&&!memcmp(buf,"ab\r\n",4));
    CHECK(write(m,"xyz",3)==3);CHECK(ioctl(s,FIONREAD,&n)==0&&n==3);
    CHECK(tcflush(s,TCIFLUSH)==0);CHECK(ioctl(s,FIONREAD,&n)==0&&n==0);
    CHECK(write(m,"q",1)==1);CHECK(tcsetattr(s,TCSAFLUSH,&t)==0);
    errno=0;CHECK(read(s,buf,1)==-1&&errno==EAGAIN);
    CHECK(tcdrain(s)==0);CHECK(tcflow(s,TCIOFF)==0);
    CHECK(read(m,buf,1)==1&&(unsigned char)buf[0]==t.c_cc[VSTOP]);
    errno=0;CHECK(tcflow(s,TCOOFF)==-1&&errno==EOPNOTSUPP);
    /* Job control from a session leader whose controlling terminal it is. */
    fflush(NULL);pid_t child=fork();CHECK(child>=0);
    if(child==0) {
        if(setsid()<0||ioctl(s,TIOCSCTTY,0)!=0)_exit(1);
        if(tcgetsid(s)!=getpid()||tcgetpgrp(s)!=getpgrp())_exit(2);
        if(tcsetpgrp(s,getpgrp())!=0)_exit(3);
        errno=0;if(tcsetpgrp(s,999999)!=-1||errno!=EPERM)_exit(4);
        _exit(0);
    }
    int status=0;CHECK(waitpid(child,&status,0)==child);
    if(!WIFEXITED(status)||WEXITSTATUS(status))printf("PTY_FAIL job_control status=%#x\n",status);
    CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    errno=0;CHECK(tcgetpgrp(s)==-1&&errno==ENOTTY); /* not our controlling terminal */
    CHECK(close(m)==0);CHECK(close(s)==0);return 0;
}
/* lwp_execve() (sys_exec) starts a child of this process; without a trailing
 * "&" a session member hands it the terminal foreground. */
static const char *self_path;
static pid_t spawn_foreground(int delay_ms) {
    extern char **environ; char delay[16]; snprintf(delay,sizeof(delay),"%d",delay_ms);
    char *argv[]={(char *)self_path,"child",delay,NULL};
    long pid=syscall(_NRSYS_exec,(long)self_path,3L,(long)argv,(long)environ);
    return pid>0?(pid_t)pid:-1;
}
/* The leader runs two foreground children at once; they exit C2, C1, leader.
 * The leader is on the foreground stack twice, so its exit must not leave it as
 * the foreground: ^C through the master would then signal a freed process. */
static int session_pipeline(void) {
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    struct termios t;CHECK(ioctl(s,TCGETS,&t)==0);t.c_lflag=ISIG;CHECK(ioctl(s,TCSETS,&t)==0);
    fflush(NULL);pid_t leader=fork();CHECK(leader>=0);
    if(leader==0) {
        if(setsid()<0||ioctl(s,TIOCSCTTY,0)!=0)_exit(1);
        pid_t c1=spawn_foreground(300),c2=spawn_foreground(100);
        if(c1<0||c2<0||wait_child(c2,0)!=0||wait_child(c1,0)!=0)_exit(1);
        _exit(0);
    }
    CHECK(wait_child(leader,0)==0);
    char intr=t.c_cc[VINTR];CHECK(write(m,&intr,1)==1);
    CHECK(transfer(m,s,'p')==0);CHECK(close(m)==0);CHECK(close(s)==0);return 0;
}
/* Foreground execs from processes without a terminal: after setsid(), and after
 * every other holder closed the session's PTY pair, which detaches the member. */
static int detached_exec(void) {
    fflush(NULL);pid_t daemon=fork();CHECK(daemon>=0);
    if(daemon==0) {
        pid_t c;if(setsid()<0||(c=spawn_foreground(0))<0||wait_child(c,0)!=0)_exit(1);
        _exit(0);
    }
    CHECK(wait_child(daemon,0)==0);
    int m=master();CHECK(m>=0);int s=slave_of(m);CHECK(s>=0);
    int ready[2],go[2];CHECK(pipe(ready)==0);CHECK(pipe(go)==0);
    fflush(NULL);pid_t member=fork();CHECK(member>=0);
    if(member==0) {
        char ch;pid_t c;close(ready[0]);close(go[1]);
        if(setsid()<0||ioctl(s,TIOCSCTTY,0)!=0||close(m)!=0||close(s)!=0)_exit(1);
        if(write(ready[1],"R",1)!=1||read(go[0],&ch,1)!=1)_exit(1);
        if((c=spawn_foreground(0))<0||wait_child(c,0)!=0)_exit(1);
        _exit(0);
    }
    close(ready[1]);close(go[0]);char ch;CHECK(read(ready[0],&ch,1)==1 && ch=='R');
    CHECK(close(m)==0);CHECK(close(s)==0);CHECK(write(go[1],"G",1)==1);
    close(ready[0]);close(go[1]);CHECK(wait_child(member,0)==0);return 0;
}
static void *worker(void *arg) {
    for(int i=0;i<200;i++) {
        int m=master();if(m<0)return (void *)1;
        int s=slave_of(m);if(s<0)return (void *)1;
        int alias=dup(m);if(alias<0||close(m)<0||transfer(alias,s,'a'+(intptr_t)arg)<0)return (void *)1;
        if(i&1) {if(close(alias)<0||close(s)<0)return (void *)1;}
        else {if(close(s)<0||close(alias)<0)return (void *)1;}
    }
    return NULL;
}
static int threads(int n) {
    pthread_t t[6];for(int i=0;i<n;i++)CHECK(pthread_create(&t[i],NULL,worker,(void *)(intptr_t)i)==0);
    for(int i=0;i<n;i++) {void *ret;CHECK(pthread_join(t[i],&ret)==0);CHECK(ret==NULL);}
    return 0;
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"child")) {usleep(atoi(argv[2])*1000);return 0;}
    self_path=argv[0];CHECK(self_path && self_path[0]=='/');
    setvbuf(stdout,NULL,_IONBF,0);
    CHECK(reuse(1000)==0);puts("PTY_PASS sequential=1000");
    CHECK(capacity()==0);CHECK(canonical()==0);puts("PTY_PASS capacity isolation dup reopen canonical");
    for(int i=0;i<5;i++)for(int side=0;side<2;side++)for(int closefd=0;closefd<2;closefd++)CHECK(sleeping(side,closefd)==0);
    puts("PTY_PASS sleeping_read_poll=20 both_peer_close_orders with_close_during_io");
    CHECK(fork_alias()==0);puts("PTY_PASS fork_alias buffered_data_then_eof");
    for(int i=0;i<10;i++) {CHECK(owner_exit(0)==0);CHECK(owner_exit(1)==0);}
    puts("PTY_PASS owner_exit=10 killed_owner=10 each_3_pairs_12_descriptors");
    pid_t child=fork();CHECK(child>=0);if(!child)_exit(session_child()!=0);CHECK(wait_child(child,0)==0);
    puts("PTY_PASS controlling_terminal_recycle=10");
    for(int i=0;i<10;i++) {CHECK(session_reclaim()==0);CHECK(hungup_output()==0);}
    puts("PTY_PASS session_reclaim=10 hungup_plain_tab_eio=10");
    for(int i=0;i<10;i++)CHECK(live_full_output()==0);
    puts("PTY_PASS live_full_blocking=10 echo_autoflush=10");
    for(int i=0;i<5;i++) {CHECK(blocking_full_write()==0);CHECK(canonical_overflow_line()==0);}
    puts("PTY_PASS blocking_full_write=5 canonical_overflow_line=5");
    for(int i=0;i<3;i++)CHECK(interrupted_write()==0);
    puts("PTY_PASS interrupted_write_eintr=3");
    for(int i=0;i<3;i++)CHECK(musl_terminal_api()==0);
    puts("PTY_PASS musl_terminal_api=3 isatty winsize onlcr fionread tcflush tcflow tcgetpgrp tcsetpgrp tcgetsid");
    for(int i=0;i<10;i++) {CHECK(session_pipeline()==0);CHECK(detached_exec()==0);}
    puts("PTY_PASS session_pipeline=10 detached_exec=20");
    CHECK(threads(6)==0);puts("PTY_PASS threaded_pairs=1200");
    pid_t children[2];
    for(int i=0;i<2;i++) {children[i]=fork();CHECK(children[i]>=0);if(!children[i])_exit(threads(3)!=0);}
    CHECK(threads(3)==0);for(int i=0;i<2;i++)CHECK(wait_child(children[i],0)==0);
    puts("PTY_PASS multiprocess_threaded_pairs=1800 peak_pairs=9");
    CHECK(capacity()==0);CHECK(reuse(1000)==0);
    puts("PTY_LIFETIME failures=0 sequential=2000 concurrent_pairs=3000");return 0;
}
