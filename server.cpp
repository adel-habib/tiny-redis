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

const size_t k_max_msg = 4096;

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

/*
 * This function sets a file descriptor to non-blocking mode.
 *
 * In non-blocking mode, I/O system calls like read() and write() do not block if
 * they cannot be completed immediately. Instead, they return -1 and set errno to EAGAIN
 * or EWOULDBLOCK.
 *
 * For read():
 * If no data is available, read() won't block waiting for data to arrive.
 * Instead, it fails with the error EAGAIN or EWOULDBLOCK.
 *
 * For write():
 * If the output buffer is full and the data can't be written immediately, write()
 * won't block waiting for buffer space to be available. Instead, it fails with
 * the error EAGAIN or EWOULDBLOCK.
 *
 * This behaviour allows a program to handle multiple I/O operations concurrently
 * without the need for multi-threading or asynchronous I/O, by checking the return
 * values and errno, and retrying the operation later if necessary.
 */
static void fd_set_nb(int fd)
{
   errno = 0;
   // fnctl stands for file control, and it performs various operations on file descriptors
   // `F_GETFL` retrieves the file access mode and the file status flags
   int flags = fcntl(fd, F_GETFL, 0);
   if (errno)
   {
      die("fcntl error");
      return;
   }
   // `O_NONBLOCK` flag is bitwise-OR'd with the existing flags to make the fd non-blocking
   flags |= O_NONBLOCK;
   errno = 0;
   // set the fd as non blocking with file control with F_SETFL and flags
   (void)fcntl(fd, F_SETFL, flags);
   if (errno)
   {
      die("fcntl error");
   }
}

static int32_t read_full(int fd, char *buf, size_t n)
{
   while (n > 0)
   {
      ssize_t rv = read(fd, buf, n);
      if (rv <= 0)
      {
         return -1;
         // error, or unexpected EOF
      }
      assert((size_t)rv <= n);
      n -= (size_t)rv;
      // change pointer position
      buf += rv;
   }
   return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n)
{
   while (n > 0)
   {
      ssize_t rv = write(fd, buf, n);
      if (rv <= 0)
      {
         return -1;
      }
      assert((size_t)rv <= n);
      n -= (size_t)rv;
      buf += rv;
   }
   return 0;
}

static int32_t one_request(int connfd)
{
   // 4 bytes header
   char rbuf[4 + k_max_msg + 1];
   errno = 0;
   int32_t err = read_full(connfd, rbuf, 4);
   if (err)
   {
      if (errno == 0)
      {
         msg("EOF");
      }
      else
      {
         msg("read() error");
      }
      return err;
   }
   uint32_t len = 0;
   memcpy(&len, rbuf, 4);
   // assume little endian
   if (len > k_max_msg)
   {
      msg("too long");
      return -1;
   }
   // request body
   err = read_full(connfd, &rbuf[4], len);
   if (err)
   {
      msg("read() error");
      return err;
   }
   // do something
   rbuf[4 + len] = '\0';
   printf("client says: %s\n", &rbuf[4]);
   // reply using the same protocol
   const char reply[] = "world";
   char wbuf[4 + sizeof(reply)];
   len = (uint32_t)strlen(reply);
   memcpy(wbuf, &len, 4);
   memcpy(&wbuf[4], reply, len);
   return write_all(connfd, wbuf, 4 + len);
}

enum
{
   STATE_REQ = 0,
   STATE_RES = 1,
   STATE_END = 2,
};

struct Conn
{
   int fd = -1;
   // either STATE_REQ or STATE_RES
   uint32_t state = 0;
   // buffer for reading
   size_t rbuf_size = 0;
   uint8_t rbuf[4 + k_max_msg];
   // buffer for writing
   size_t wbuf_size = 0;
   size_t wbuf_sent = 0;
   uint8_t wbuf[4 + k_max_msg];
};

// the fd2conn vector acts as map of connections where the key is the fd number itself
static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn)
{
   if (fd2conn.size() <= (size_t)conn->fd)
   {
      fd2conn.resize(conn->fd + 1);
   }
   fd2conn[conn->fd] = conn;
}
static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd)
{
   // accept
   struct sockaddr_in client_addr = {};
   socklen_t socklen = sizeof(client_addr);
   int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
   if (connfd < 0)
   {
      msg("accept() error");
      return -1;
      // error
   }
   // set the new connection fd to nonblocking mode
   fd_set_nb(connfd);
   // creating the struct Conn
   struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
   if (!conn)
   {
      close(connfd);
      return -1;
   }
   conn->fd = connfd;
   conn->state = STATE_REQ;
   conn->rbuf_size = 0;
   conn->wbuf_size = 0;
   conn->wbuf_sent = 0;
   conn_put(fd2conn, conn);
   return 0;
}


/**
 * Tries to send all remaining data in the write buffer of a connection.
 *
 * This function continuously sends data from the write buffer over the network
 * as long as the write call is successful and there's more data to send.
 * If the write call would block, or if an error occurs, the function returns immediately.
 * After all the data in the buffer has been sent, the function resets the buffer
 * and changes the connection state back to STATE_REQ.
 *
 * @param conn A pointer to the connection structure.
 * @return true if the function should be called again to continue sending data;
 *         false if the function should not be called again immediately because either
 *         all the data has been sent, the write call would block, or an error occurred.
 */
static bool try_flush_buffer(Conn *conn)
{
   ssize_t rv = 0;

   // Loop to continuously send remaining data from the buffer as long as the write call is successful.
   do
   {
      size_t remain = conn->wbuf_size - conn->wbuf_sent;
      rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
   } while (rv < 0 && errno == EINTR); // Retry if the write call was interrupted.

   // If the write call would block, return false.
   if (rv < 0 && errno == EAGAIN)
   {
      // Got EAGAIN, stop.
      return false;
   }

   // If a different write error occurred, report it, change the state to STATE_END and return false.
   if (rv < 0)
   {
      msg("write() error");
      conn->state = STATE_END;
      return false;
   }

   // If the write call was successful, update the amount of data sent.
   conn->wbuf_sent += (size_t)rv;
   assert(conn->wbuf_sent <= conn->wbuf_size);

   // If all the data has been sent, reset the buffer, change the state back to STATE_REQ and return false.
   if (conn->wbuf_sent == conn->wbuf_size)
   {
      // Response was fully sent, change state back.
      conn->state = STATE_REQ;
      conn->wbuf_sent = 0;
      conn->wbuf_size = 0;
      return false;
   }

   // If there's still more data in the buffer to send, return true.
   return true;
}

static void state_res(Conn *conn)
{
   while (try_flush_buffer(conn))
   {
   }
}

/**
 * Tries to parse and process a single request from the read buffer of a connection.
 *
 * This function assumes that requests are in the format of a 4-byte length followed by a message.
 * If the read buffer contains at least one complete request, the function processes it, prepares
 * a response, and returns true to indicate that it's ready to process another request.
 * If the read buffer doesn't contain a complete request, or if an error occurs (such as the message
 * length exceeding the maximum), the function returns false.
 *
 * @param conn A pointer to the connection structure.
 * @return true if the function successfully processed a request and is ready to process another;
 *         false if the function should not be called again immediately because either the read buffer
 *         does not contain a complete request, an error occurred, or the connection is ready to send a response.
 */
static bool try_one_request(Conn *conn)
{
   // Check if there's enough data in the buffer to read the 4-byte length.
   if (conn->rbuf_size < 4)
   {
      // Not enough data in the buffer. Will retry in the next iteration.
      return false;
   }

   // Read the 4-byte length into 'len'.
   uint32_t len = 0;
   memcpy(&len, &conn->rbuf[0], 4);

   // Check if 'len' exceeds the maximum message size.
   if (len > k_max_msg)
   {
      msg("too long");
      conn->state = STATE_END;
      return false;
   }

   // Check if there's enough data in the buffer for the entire message.
   if (4 + len > conn->rbuf_size)
   {
      // Not enough data in the buffer. Will retry in the next iteration.
      return false;
   }

   // Process the message. In this case, simply print it and prepare an echo response.
   printf("client says: %.*s\n", len, &conn->rbuf[4]);

   // Prepare the echo response.
   memcpy(&conn->wbuf[0], &len, 4);             // Copy the length into the write buffer.
   memcpy(&conn->wbuf[4], &conn->rbuf[4], len); // Copy the message into the write buffer.
   conn->wbuf_size = 4 + len;                   // Update the write buffer size.

   // Remove the processed request from the read buffer.
   // Note: frequent memmove is inefficient and needs better handling for production code.
   size_t remain = conn->rbuf_size - 4 - len;
   if (remain)
   {
      // Remove the processed request from the read buffer by moving all the data
      // after the request to the start of the buffer. 'remain' is the amount of
      // data in the buffer after the processed request. '&conn->rbuf[4 + len]' is
      // the position in the buffer immediately after the end of the processed request.
      // Note that memmove is used instead of memcpy because the source and destination
      // memory areas overlap, and memmove handles this correctly.
      memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
   }
   conn->rbuf_size = remain; // Update the read buffer size.

   // Change state to STATE_RES, indicating that a response is ready to be sent.
   conn->state = STATE_RES;
   state_res(conn);

   // If the connection state is still STATE_REQ, return true to indicate that the function
   // is ready to process another request. Otherwise, return false.
   return (conn->state == STATE_REQ);
}
/**
 * Attempts to fill the read buffer of a connection from a non-blocking file descriptor.
 *
 * This function continuously reads data from a client connection into the read buffer.
 * It continues to read data as long as the read call is successful and the buffer is not full.
 * If the read call would block (because there's no data currently available),
 * or if an error occurs other than an interrupt signal, the function returns immediately.
 * In the event of a successful read, the function attempts to process as many complete
 * requests in the buffer as it can.
 *
 * @param conn A pointer to the connection structure.
 * @return true if the function should be called again to continue reading data;
 *         false if the function should not be called again immediately because
 *         either an error occurred, the other end of the connection has been closed,
 *         or the read call would block.
 */
static bool try_fill_buffer(Conn *conn)
{
   // Assert that we have not exceeded the buffer size
   assert(conn->rbuf_size < sizeof(conn->rbuf));

   ssize_t rv = 0;

   // Attempt to read data from the client connection into the buffer until
   // either an error occurs or the buffer is full.
   // In the case of an interrupt signal (errno == EINTR), the operation is retried.
   do
   {
      size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
      rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
   } while (rv < 0 && errno == EINTR);

   // If the read operation would block (meaning there's no data available), return false.
   if (rv < 0 && errno == EAGAIN)
   {
      // got EAGAIN, stop.
      return false;
   }

   // If a different read error occurred, report it, change the state to STATE_END and return false.
   if (rv < 0)
   {
      msg("read() error");
      conn->state = STATE_END;
      return false;
   }

   // If the other end of the connection has been closed, report it,
   // change the state to STATE_END and return false.
   if (rv == 0)
   {
      if (conn->rbuf_size > 0)
      {
         msg("unexpected EOF");
      }
      else
      {
         msg("EOF");
      }
      conn->state = STATE_END;
      return false;
   }

   // If data was successfully read, update the buffer size and return true.
   conn->rbuf_size += (size_t)rv;

   // This assert ensures that the read buffer size does not exceed the total buffer size.
   assert(conn->rbuf_size <= sizeof(conn->rbuf));

   // This loop is used to process multiple requests from the same connection,
   // if they are available in the buffer. This concept is known as "pipelining".
   //
   // Pipelining is a technique where the client sends multiple requests without
   // waiting for each response, and the server processes these requests in the
   // order they are received. Pipelining can increase protocol efficiency by
   // reducing the impact of network latency.
   //
   // In this context, the client could potentially send multiple requests in rapid
   // succession, faster than the server can process them. These requests would all
   // be read into the server's buffer in one or more `read` calls.
   //
   // The `try_one_request` function is called in a loop until it either processes
   // all complete requests in the buffer, or it encounters a request that's incomplete
   // (because not all of the request data has been received yet).
   //
   // If `try_one_request` returns false, it means it encountered an incomplete request,
   // so we break the loop and return to the event loop, which will call `try_fill_buffer`
   // again when more data is available to read. If `try_one_request` keeps returning
   // true, it means it's successfully processing multiple pipelined requests.
   //
   // Therefore, this loop allows the server to process multiple pipelined requests
   // from the same connection without having to return to the event loop and wait for
   // another `read` event on this connection's file descriptor.
   while (try_one_request(conn))
   {
   }

   // Return true if the connection is still in the STATE_REQ state,
   // indicating that the function should be called again to continue reading data.
   // Otherwise, return false.
   return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn)
{
   while (try_fill_buffer(conn))
   {
   }
}

/**
 * Manages the state of the given connection, based on the current state of the state machine.
 *
 * This function is a core part of the server's state machine. In our state machine, each connection
 * can be in one of three states: STATE_REQ, STATE_RES, or STATE_END. The `connection_io` function checks 
 * the current state of the connection, and calls the corresponding function to handle that state:
 *
 * - If the connection is in STATE_REQ, this function calls state_req(), which attempts to read a client request 
 *   from the connection and process it.
 *
 * - If the connection is in STATE_RES, this function calls state_res(), which attempts to write a server response 
 *   to the connection.
 *
 * - If the connection is in an unexpected state, the function will assert to indicate an unexpected state.
 *
 * After the state-specific function completes, the state of the connection may change, and `connection_io`
 * will be called again the next time through the main server loop to handle the new state.
 *
 * @param conn A pointer to the connection structure representing the connection to be handled.
 */
static void connection_io(Conn *conn)
{

   // +-------------+        a request is fully        +-------------+
   // |             |           read from the          |             |
   // |  STATE_REQ  | <--------------------------->    |  STATE_RES  |
   // |             |         buffer / a response      |             |
   // +-------------+        is fully written to       +-------------+
   //      |                      the network                 |
   //      |                                                  |
   //      +---------------------->                           |
   //      An error occurs, or the connection is closed by the client
   //      |
   //      v
   // +-------------+
   // |  STATE_END  |
   // +-------------+

   if (conn->state == STATE_REQ)
   {
      state_req(conn);
   }
   else if (conn->state == STATE_RES)
   {
      state_res(conn);
   }
   else
   {
      // invalid state
      assert(0);
   }
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

   // List of all existing connections
   std::vector<Conn *> fd2conn;

   // set the socket to be non-blocking
   fd_set_nb(fd);

   // poll_args is a vector of pollfd structures, where each structure represents
   // a file descriptor to be monitored for events. The vector is rebuilt in each
   // iteration of the event loop to reflect the current state of the listening
   // socket and each client connection.
   //
   // pollfd is a Data structure describing a polling request.
   //
   // For each connection, a pollfd is added to the vector with the events field
   // set to POLLIN if the connection is waiting for a request, or POLLOUT if
   // the connection is ready to send a response. The vector always includes the
   // listening socket with the events field set to POLLIN, indicating that the
   // server is always ready to accept new connections.
   //
   // After the vector is built, it is passed to the poll() function, which waits
   // for one of the requested events to occur on one of the file descriptors.
   // When poll() returns, the server processes each connection with pending events
   // accordingly.
   std::vector<struct pollfd> poll_args;

// Main loop:
//   |
//   | (Create a pollfd structure for each active connection)
//   v
// poll()
//   |
//   | (Iterate over each active connection)
//   v
// connection_io()
//   |
//   | (Based on the state of the connection, call the appropriate function)
//   v
// state_req() or state_res()
//   |
//   | (Call try_fill_buffer() or try_flush_buffer() respectively)
//   v
// try_fill_buffer() or try_flush_buffer()
//   |
//   | (Read a request from the buffer and process it, or write a response to the network)
//   v
// Back to main loop
   while (true)
   {
      // prepare the poll args
      poll_args.clear();

      // Add listening socket to the list
      struct pollfd pfd = {fd, POLLIN, 0};
      poll_args.push_back(pfd);

      // Add all connections to the list with events corresponing to their state
      for (Conn *conn : fd2conn)
      {
         if (!conn)
         {
            continue;
         }
         struct pollfd pfd = {};
         pfd.fd = conn->fd;
         pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
         pfd.events = pfd.events | POLLERR;
         poll_args.push_back(pfd);
      }

      // poll for active fds
      // the timeout argument doesn't matter here
      int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), 1000);
      if (rv < 0)
      {
         die("poll");
      }

      // process active connections
      for (size_t i = 1; i < poll_args.size(); ++i)
      {
         if (poll_args[i].revents)
         {
            Conn *conn = fd2conn[poll_args[i].fd];
            connection_io(conn);
            if (conn->state == STATE_END)
            {
               // client closed normally, or something bad happened.
               // destroy this connection
               fd2conn[conn->fd] = NULL;
               (void)close(conn->fd);
               free(conn);
            }
         }
      }
      // try to accept a new connection if the listening fd is active
      if (poll_args[0].revents)
      {
         (void)accept_new_conn(fd2conn, fd);
      }
   }
}
