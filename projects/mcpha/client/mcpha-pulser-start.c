#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <windows.h>
#define INVSOC INVALID_SOCKET
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifndef SOCKET
#define SOCKET int
#define INVSOC (-1)
#endif
#endif

#if defined(__APPLE__) || defined(__MACH__)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL SO_NOSIGPIPE
#endif
#endif

void usage()
{
  #if defined(_WIN32)
  fprintf(stderr, "Usage: mcpha-pulser-start.exe addr rate dist rise fall file\n");
  #else
  fprintf(stderr, "Usage: mcpha-pulser-start addr rate dist rise fall file\n");
  #endif
  fprintf(stderr, " addr - IP address of the Red Pitaya board,\n");
  fprintf(stderr, " rate - pulse rate expressed in counts per second (from 1 to 100000),\n");
  fprintf(stderr, " dist - pulse distribution (0 for uniform, 1 for poisson),\n");
  fprintf(stderr, " rise - pulse rise time expressed in nanoseconds (from 0 to 100),\n");
  fprintf(stderr, " fall - pulse fall time expressed in microseconds (from 0 to 100),\n");
  fprintf(stderr, " file - text file containing the spectrum.\n");
}

int main(int argc, char *argv[])
{
  FILE *fp;
  SOCKET sock_server;
  struct sockaddr_in addr;
  fd_set writefds;
  struct timeval timeout;
  int i, result;
  char *end;
  long value;
  char string[128];
  int32_t scale, fall, rise, rate, dist;
  uint32_t hist[4096];
  float r, f, a, b, t;
  uint64_t command[4102];

  if(argc != 7)
  {
    usage();
    return EXIT_FAILURE;
  }

  errno = 0;
  value = strtol(argv[2], &end, 10);
  if(errno != 0 || end == argv[2] || value < 1 || value > 100000)
  {
    usage();
    return EXIT_FAILURE;
  }
  rate = value;

  value = strtol(argv[3], &end, 10);
  if(errno != 0 || end == argv[3] || value < 0 || value > 1)
  {
    usage();
    return EXIT_FAILURE;
  }
  dist = value;

  value = strtol(argv[4], &end, 10);
  if(errno != 0 || end == argv[4] || value < 0 || value > 100)
  {
    usage();
    return EXIT_FAILURE;
  }
  rise = value;

  value = strtol(argv[5], &end, 10);
  if(errno != 0 || end == argv[5] || value < 0 || value > 100)
  {
    usage();
    return EXIT_FAILURE;
  }
  fall = value;

  if((fp = fopen(argv[6], "r")) == NULL)
  {
    fprintf(stderr, "** ERROR: could not open %s\n", argv[6]);
    return EXIT_FAILURE;
  }

  memset(hist, 0, 16384);

  for(i = 0; i < 4096; ++i)
  {
    if(fgets(string, 128, fp) == NULL) break;
    value = strtol(string, NULL, 10);
    if(errno == ERANGE) continue;
    if(value < 0) continue;
    hist[i] = value;
  }

  #if defined(_WIN32)
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
  #endif

  sock_server = socket(AF_INET, SOCK_STREAM, 0);
  if(sock_server == INVSOC)
  {
    fprintf(stderr, "** ERROR: could not create TCP socket\n");
    return EXIT_FAILURE;
  }

  #if defined(_WIN32)
  u_long mode = 1;
  ioctlsocket(sock_server, FIONBIO, &mode);
  #else
  int flags = fcntl(sock_server, F_GETFL, 0);
  fcntl(sock_server, F_SETFL, flags | O_NONBLOCK);
  #endif

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(argv[1]);
  addr.sin_port = htons(1001);

  connect(sock_server, (struct sockaddr *)&addr, sizeof(addr));

  timeout.tv_sec = 5;
  timeout.tv_usec = 0;

  FD_ZERO(&writefds);
  FD_SET(sock_server, &writefds);

  #if defined(_WIN32)
  result = select(0, 0, &writefds, 0, &timeout);
  #else
  result = select(sock_server + 1, 0, &writefds, 0, &timeout);
  #endif

  if(result <= 0)
  {
    fprintf(stderr, "** ERROR: could not connect to %s\n", argv[1]);
    return EXIT_FAILURE;
  }

  if(rise < 10 || fall == 0)
  {
    scale = 65535;
  }
  else
  {
    r = floor(exp(-log(2.0) / 125.0 / rise * 1.0e3) * 65535.0 + 0.5);
    f = floor(exp(-log(2.0) / 125.0 / fall) * 65535.0 + 0.5);
    a = -logf(r / 65536.0);
    b = -logf(f / 65536.0);
    t = logf(b / a) / (b - a);
    scale = (int32_t)floor((b - a) / (expf(-a * t) - expf(-b * t)) * 65535.0 + 0.5);
  }
  command[0] = (24ULL << 56) + scale;
  command[1] = (25ULL << 56) + (int32_t)floor(expf(-logf(2.0) / 125.0 / fall) * 65536.0 + 0.5);
  command[2] = (26ULL << 56) + (int32_t)floor(expf(-logf(2.0) / 125.0 / rise * 1.0e3) * 65536.0 + 0.5);
  command[3] = (29ULL << 56) + rate;
  command[4] = (30ULL << 56) + dist;

  for(i = 0; i < 4096; ++i)
  {
    command[5 + i] = (32ULL << 56) + ((uint64_t) i << 32) + hist[i];
  }

  command[4101] = (33ULL << 56) + 1;

  #if defined(_WIN32)
  int total = sizeof(command);
  int size;
  size = send(sock_server, (char *)command, total, 0);
  #else
  ssize_t total = sizeof(command);
  ssize_t size;
  size = send(sock_server, command, total, MSG_NOSIGNAL);
  #endif

  #if defined(_WIN32)
  closesocket(sock_server);
  #else
  close(sock_server);
  #endif

  if(size < total)
  {
    fprintf(stderr, "** ERROR: could not send command\n");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}