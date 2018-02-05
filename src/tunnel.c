#include "myssh.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

#ifdef _WIN32
#define NONBLOCK_OK (WSAGetLastError() == WSAEWOULDBLOCK)
void set_nonblocking(int sockfd){
  u_long nonblocking = 1;
  ioctlsocket(sockfd, FIONBIO, &nonblocking);
}
#else
#define NONBLOCK_OK (errno == EAGAIN || errno == EWOULDBLOCK)
void set_nonblocking(int sockfd){
  long arg = fcntl(sockfd, F_GETFL, NULL);
  arg |= O_NONBLOCK;
  fcntl(sockfd, F_SETFL, arg);
}
#endif

/* Check for interrupt without long jumping */
void check_interrupt_fn(void *dummy) {
  R_CheckUserInterrupt();
}

int pending_interrupt() {
  return !(R_ToplevelExec(check_interrupt_fn, NULL));
}

/* check for system errors */
void syserror_if(int err, const char * what){
  if(err && !NONBLOCK_OK)
    Rf_errorcall(R_NilValue, "System failure for: %s (%s)", what, strerror(errno));
}

char spinner(){
  static int x;
  x = (x + 1) % 4;
  switch(x){
  case 0: return '|';
  case 1: return '/';
  case 2: return '-';
  case 3: return '\\';
  }
  return '?';
}

static void print_progress(int add){
  static int total;
  if(add < 0)
    total = 0;
  Rprintf("\r%c Tunneled %d bytes...", spinner(), total += add);
}

/* Wait for descriptor */
int wait_for_fd(int fd, int port){
  int waitms = 200;
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = waitms * 1000;
  fd_set rfds;
  int active = 0;
  while(active == 0){
    Rprintf("\r%c Waiting for connetion on port %d... ", spinner(), port);
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    active = select(fd+1, &rfds, NULL, NULL, &tv);
    syserror_if(active < 0, "select()");
    if(active || pending_interrupt())
      break;
  }
  return active;
}

int open_port(int port){

  // define server socket
  struct sockaddr_in serv_addr;
  memset(&serv_addr, '0', sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);

  //creates the listening socket
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  syserror_if(listenfd < 0, "socket()");
  syserror_if(bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0, "bind()");
  syserror_if(listen(listenfd, 10) < 0, "listen()");
  return listenfd;
}

void host_tunnel(ssh_channel tunnel, int connfd){
  char buf[16384];

  //assume connfd is non-blocking
  int avail = 1;
  fd_set rfds;
  struct timeval tv = {0, 100000}; //100ms
  print_progress(-1);
  while(!pending_interrupt() && ssh_channel_is_open(tunnel) && !ssh_channel_is_eof(tunnel)){
    FD_ZERO(&rfds);
    FD_SET(connfd, &rfds);
    ssh_channel channels[2] = {tunnel, 0};
    ssh_channel out[2];
    ssh_select(channels, out, connfd+1, &rfds, &tv);

    /* Pipe local socket data to ssh channel */
    while((avail = recv(connfd, buf, sizeof(buf), 0)) > 0){
      ssh_channel_write(tunnel, buf, avail);
      print_progress(avail);
    }
    syserror_if(avail == -1, "recv() from user");
    if(avail == 0)
      break;

    /* Pipe ssh stdout data to local socket */
    while((avail = ssh_channel_read_nonblocking(tunnel, buf, sizeof(buf), 0)) > 0){
      syserror_if(send(connfd, buf, avail, 0) < avail, "send() to user");
      print_progress(avail);
    }
    syserror_if(avail == -1, "ssh_channel_read_nonblocking()");

    /* Print ssh stderr data to console (not sure if this is needed) */
    while((avail = ssh_channel_read_nonblocking(tunnel, buf, sizeof(buf), 1)) > 0)
      REprintf("%.*s", avail, buf);
    syserror_if(avail == -1, "ssh_channel_read_nonblocking()");
    print_progress(0); //spinner only
  }
  close(connfd);
  ssh_channel_send_eof(tunnel);
  ssh_channel_close(tunnel);
  ssh_channel_free(tunnel);
}

void open_tunnel(ssh_session ssh, int port, const char * outhost, int outport){
  int listenfd = open_port(port);
  if(wait_for_fd(listenfd, port) == 0)
    return;
  int connfd = accept(listenfd, NULL, NULL);
  syserror_if(connfd < 0, "accept()");
  Rprintf("client connnected!\n");
  set_nonblocking(connfd);
  ssh_channel tunnel = ssh_channel_new(ssh);
  bail_if(tunnel == NULL, "ssh_channel_new", ssh);
  bail_if(ssh_channel_open_forward(tunnel, outhost, outport, "localhost", port), "channel_open_forward", ssh);
  host_tunnel(tunnel, connfd);
  Rprintf("tunnel closed!\n");
  close(listenfd);
}

/* Set up tunnel to the target host */
SEXP C_blocking_tunnel(SEXP ptr, SEXP port, SEXP target_host, SEXP target_port){
  open_tunnel(ssh_ptr_get(ptr), Rf_asInteger(port), CHAR(STRING_ELT(target_host, 0)), Rf_asInteger(target_port));
  return R_NilValue;
}
