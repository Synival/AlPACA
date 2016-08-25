/* connections.c
 * -------------
 * connection management for servers. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

#include "connections.h"

connection_type *connection_new (server_type *server, int fd,
   struct sockaddr_in *addr, socklen_t addr_size)
{
   connection_type *new;

   /* allocate and assign data. */
   new = malloc (sizeof (connection_type));
   memset (new, 0, sizeof (connection_type));
   new->sock_fd = fd;
   if (addr) {
      new->addr = *addr;
      new->addr_size = addr_size;
   }

   /* link to our server. */
   server_lock (server);
   LLIST_LINK_FRONT (new, server, prev, next, server, connection_list);
   if (server->func[SERVER_FUNC_JOIN])
      if (!server->func[SERVER_FUNC_JOIN] (server, new, NULL, 0)) {
         connection_free (new);
         return NULL;
      }
   server_unlock (server);

   /* return our new connection. */
   return new;
}

int connection_free (connection_type *c)
{
   server_type *server;

   /* boo race conditions! */
   server = c->server;
   server_lock (server);

   /* function for leaving? */
   if (server->func[SERVER_FUNC_LEAVE])
      server->func[SERVER_FUNC_LEAVE] (server, c, NULL, 0);

   /* attempt to send remaining output. */
   connection_fd_write (c);

   /* close our socket. */
   socket_close (c->sock_fd);

   /* unlink. */
   LLIST_UNLINK (c, prev, next, c->server, connection_list);

   /* free remaining data and return success. */
   free (c);
   server_unlock (server);
   return 1;
}

int connection_append_buffer (connection_type *c, unsigned char **buf,
   size_t *size, size_t *len, size_t *pos, unsigned char *input, size_t isize)
{
   size_t new_size;

   /* don't do anything if there's no buffer whatsoever. */
   if (input == NULL || isize == 0)
      return 0;

   /* don't allow reading while we're doing this. */
   server_lock (c->server);

   /* how large should our buffer be? */
   if (*buf == NULL)
      new_size = 1; //256;
   else
      new_size = *size;
   while (new_size < *len + isize + 1)
      new_size *= 2;

   /* make sure our buffer is the proper size. */
   if (*buf == NULL) {
      buf = malloc (sizeof (unsigned char) * new_size);
      *size = new_size;
   }
   else if (new_size != *size) {
      *buf = realloc (*buf, new_size);
      *size = new_size;
   }

   /* copy data into our buffer.  make it NULL-terminated, just in case. */
   memcpy (*buf + *len, input, sizeof (unsigned char) * isize);
   *len += isize;
   *((*buf) + *len) = 0;

   /* we did it, you guise! */
   server_unlock (c->server);
   return 1;
}

int connection_fetch_buffer (connection_type *c, unsigned char **buf,
   size_t *size, size_t *len, size_t *pos, unsigned char *output, size_t osize)
{
   size_t input_len;

   /* if we don't have a buffer, do nothin'. */
   if (output == NULL || osize == 0)
      return 0;

   /* don't allow writing while we're doing this. */
   server_lock (c->server);

   /* ...and don't bother if there's nothing to read. */
   input_len = *len - *pos;
   if (*buf == NULL || input_len <= 0) {
      server_unlock (c->server);
      return 0;
   }
   /* are we only reading a portion? */
   else if (osize < input_len) {
      memcpy (output, *buf + *pos, sizeof (unsigned char) * osize);
      *pos += osize;
      server_unlock (c->server);
      return osize;
   }
   else {
      /* we're reading everything! */
      memcpy (output, *buf + *pos, sizeof (unsigned char) * input_len);

      /* reset our buffer and return the number of bytes we read. */
      *len = 0;
      *pos = 0;
      server_unlock (c->server);
      return input_len;
   }
}

int connection_read (connection_type *c, unsigned char *buf, size_t size)
{
   return connection_fetch_buffer (c, &(c->input), &(c->input_size),
      &(c->input_len), &(c->input_pos), buf, size);
}

int connection_fd_read (connection_type *c)
{
   static unsigned char buf[4096];
   int res;

   /* attempt to read.  if it didn't work, the connection has been closed.
    * return -1 to indicate an error. */
   if ((res = read (c->sock_fd, buf, 4096)) <= 0) {
      unsigned char *ib;
      ib = malloc (c->input_len + 1);
      memcpy (ib, c->input, c->input_len);
      ib[c->input_len] = '\0';
      free (ib);
      return -1;
   }

   /* add to our input buffer. */
   connection_append_buffer (c, &(c->input), &(c->input_size),
      &(c->input_len), &(c->input_pos), buf, res);

   /* return the number of bytes read. */
   return res;
}

int connection_fd_write (connection_type *c)
{
   size_t bytes, max;
   int res;

   /* attempt to write. */
   bytes = c->output_len - c->output_pos;
   max = c->output_max < bytes ? c->output_max : bytes;

   if (max <= 0)
      return 0;
   if ((res = write (c->sock_fd, c->output + c->output_pos, bytes)) <= 0) {
      ERROR ("Couldn't write %ld bytes to client [%d].\n", bytes, c->sock_fd);
      return -1;
   }

   /* if we wrote everything, clear out our buffer. */
   if (res >= bytes) {
      c->output_len = 0;
      c->output_pos = 0;
      c->flags &= ~CONNECTION_WROTE;
   }
   /* or just move forward a little bit. */
   else
      c->output_pos += bytes;

   /* allow SERVER_FUNC_PRE_WRITE to run again once output_max reaches zero. */
   c->output_max -= res;
   if (c->output_max <= 0)
      c->flags &= ~CONNECTION_WRITING;

   return bytes;
}

int connection_write (connection_type *c, unsigned char *buf, size_t size)
{
   int res;
   res = connection_append_buffer (c, &(c->output), &(c->output_size),
      &(c->output_len), &(c->input_pos), buf, size);
   connection_wrote (c);
   return res;
}

int connection_write_all (server_type *server, unsigned char *buf, size_t size)
{
   connection_type *c;
   int count;

   /* connection_write() to everyone! */
   server_lock (server);
   count = 0;
   for (c = server->connection_list; c != NULL; c = c->next)
      count += connection_write (c, buf, size);
   server_unlock (server);

   return count;
}

int connection_wrote (connection_type *c)
{
   c->flags |= CONNECTION_WROTE;
   server_interrupt (c->server);
   return 1;
}
