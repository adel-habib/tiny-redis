#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>

static void die(const char *msg)
{
   int err = errno;
   fprintf(stderr, "[%d] %s\n", err, msg);
   abort();
}

static void msg(const char *msg)
{
   fprintf(stderr, "%s\n", msg);
}

static void do_something(int connfd)
{
   char rbuf[64] = {};
   // sizeof(rbuf) - 1 to reserve space for a null terminator ('\0') to make it a proper C-style string.
   ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
   if (n < 0)
   {
      msg("read() error");
      return;
   }
   printf("client says: %s\n", rbuf);
   char wbuf[] = "world\n";
   write(connfd, wbuf, strlen(wbuf));
}

int main()
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   int val = 1;
   // SOL_SOCKET sets the level of these options to the socket itself
   // SO_REUSEADDR allows us to reuse the same socket (quick restart and dev)
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

   struct sockaddr_in addr = {};
   addr.sin_family = AF_INET;
   // set the port number (in host encoding nthos is `network to host short`)
   addr.sin_port = ntohs(1234);
   addr.sin_addr.s_addr = ntohl(0);

   // bind the socket to the address
   int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
   if (rv)
   {
      die("bind()");
   }

   // Start listening
   rv = listen(fd, SOMAXCONN);
   if (rv)
   {
      die("listen()");
   }

   while (true)
   {
      struct sockaddr_in client_addr = {};
      socklen_t client_addr_len = sizeof(client_addr);
      int connfd = accept(fd, (struct sockaddr *)&client_addr, &client_addr_len);
      if (connfd < 0)
      {
         continue;
      }
      do_something(connfd);
      close(connfd);
   }
}

