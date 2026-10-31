#define _XOPEN_SOURCE 600
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_line(int fd,char *line,size_t size) {
  size_t used=0;
  while (used+1<size) {
    char byte;ssize_t n=read(fd,&byte,1);
    if (n<0 && (errno==EINTR || errno==EIO)) { usleep(1000);continue; }
    if (n<=0) return -1;
    if (byte=='\n') {line[used]=0;return 0;}
    if (byte!='\r') line[used++]=byte;
  }
  return -1;
}
static void write_all(int fd,const char *text) {
  size_t n=strlen(text),used=0;
  while (used<n) {ssize_t wrote=write(fd,text+used,n-used);assert(wrote>0);used+=(size_t)wrote;}
}
static void protocol(int master) {
  char line[128];assert(read_line(master,line,sizeof(line))==0 && !strcmp(line,"tv protocol"));
  write_all(master,"noise\r\nCECCTRL/2 OK command=protocol firmware=test session=0000000000000001 on_ms=20000 standby_ms=10000 status_ms=3000 gpio=1 ready=1 busy=0\r\n");
}
static unsigned long long request(int master,const char *command) {
  char line[128],name[16],extra;unsigned long long id=0;
  assert(read_line(master,line,sizeof(line))==0);
  if (!strcmp(command,"on")) {
    unsigned physical=0;assert(sscanf(line,"tv %15s %4x %16llx%c",name,&physical,&id,&extra)==3);assert(physical==0x1000);
  } else assert(sscanf(line,"tv %15s %16llx%c",name,&id,&extra)==2);
  assert(!strcmp(command,name) && id);return id;
}
static void result(int master,const char *command,unsigned long long id,const char *session,bool partial) {
  char reply[512];
  snprintf(reply,sizeof(reply),"CECCTRL/2 %s command=%s id=%016llx session=%s code=%s power=on value=0 verified=1 source=%s routing=%s attempts=1 elapsed_ms=700 phase=activate\r\n",
    partial?"ERR":"OK",command,id,session,partial?"input_unconfirmed":"ok",
    !strcmp(command,"on")?"announced":"none",!strcmp(command,"on")&&!partial?"confirmed":"unconfirmed");
  write_all(master,reply);
}
static pid_t client(const char *program,const char *slave,int master,const char *command) {
  pid_t pid=fork();assert(pid>=0);
  if (!pid) {
    close(master);
    execl(program,program,"--config","/nonexistent","--device",slave,"--physical-address","1.0.0.0",command,(char *)NULL);
    _exit(90);
  }
  return pid;
}
static void exited(pid_t pid,int expected) {
  int status;assert(waitpid(pid,&status,0)==pid);
  assert(WIFEXITED(status) && WEXITSTATUS(status)==expected);
}
static void scenario(const char *program,int mode) {
  int master=posix_openpt(O_RDWR|O_NOCTTY);assert(master>=0);
  assert(!grantpt(master) && !unlockpt(master));char slave[128];snprintf(slave,sizeof(slave),"%s",ptsname(master));
  int ready[2];assert(!pipe(ready));
  pid_t server=fork();assert(server>=0);
  if (!server) {
    alarm(10);close(ready[0]);
    if (mode==1) {
      char line[128];assert(!read_line(master,line,sizeof(line)) && !strcmp(line,"tv protocol"));
      write_all(master,"CECCTRL/1 OK command=protocol firmware=old\r\n");_exit(0);
    }
    protocol(master);unsigned long long id=request(master,"on");
    if (mode==0) {
      /* Different request IDs and an oversized line with a valid-looking tail cannot complete this request. */
      result(master,"on",id+1,"0000000000000001",false);
      char oversized[700];memset(oversized,'x',sizeof(oversized)-1);oversized[sizeof(oversized)-1]=0;write_all(master,oversized);
      result(master,"on",id,"0000000000000001",false);
      assert(write(ready[1],"x",1)==1);
      struct pollfd descriptor={.fd=master,.events=POLLIN};
      assert(poll(&descriptor,1,250)==0); /* second client must not probe or flush during ownership */
      result(master,"on",id,"0000000000000001",false);
      protocol(master);id=request(master,"status");result(master,"status",id,"0000000000000001",false);
    } else result(master,"on",id,mode==2?"0000000000000002":"0000000000000001",mode==3);
    _exit(0);
  }
  close(ready[1]);pid_t first=client(program,slave,master,"on"),second=-1;
  if (!mode) {char byte;assert(read(ready[0],&byte,1)==1);second=client(program,slave,master,"status");}
  close(ready[0]);
  exited(first,mode==1 || mode==2 ? 4: mode==3 ? 6:0);
  if (second>0) exited(second,0);
  exited(server,0);close(master);
}
int main(int argc,char **argv) {
  assert(argc==2);alarm(35);
  for (int mode=0;mode<4;mode++) scenario(argv[1],mode);
  puts("Serial ownership, identity, framing, version, and partial-result regressions passed");return 0;
}
