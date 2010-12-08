/*
 * Copyright (c) 2009, Rambler media
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY Rambler media ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Rambler BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Implementation of map files handling
 */
#include "config.h"
#include "map.h"
#include "main.h"
#include "util.h"
#include "mem_pool.h"

static memory_pool_t           *map_pool = NULL;

static GList                   *maps = NULL;
static gchar                    *hash_fill = "1";

/* Http reply */
struct http_reply {
	gint                            code;
	GHashTable                     *headers;
	gchar                           *cur_header;

	gint                            parser_state;
};

struct http_callback_data {
	struct event                    ev;
	struct timeval                  tv;
	struct rspamd_map              *map;
	struct http_map_data           *data;
	struct http_reply              *reply;
	struct map_cb_data              cbdata;

	gint                            state;
	gint                            fd;
};

/* Value in seconds after whitch we would try to do stat on list file */
#define MON_TIMEOUT 10
/* HTTP timeouts */
#define HTTP_CONNECT_TIMEOUT 2
#define HTTP_READ_TIMEOUT 10

/**
 * Helper for HTTP connection establishment
 */
static gint
connect_http (struct rspamd_map *map, struct http_map_data *data, gboolean is_async)
{
	gint                            sock;

	if ((sock = make_tcp_socket (&data->addr, data->port, FALSE, is_async)) == -1) {
		msg_info ("cannot connect to http server %s: %d, %s", data->host, errno, strerror (errno));
		return -1;
	}

	return sock;
}

/**
 * Write HTTP request
 */
static void
write_http_request (struct rspamd_map *map, struct http_map_data *data, gint sock)
{
	gchar                           outbuf[BUFSIZ];
	gint                            r;

	r = rspamd_snprintf (outbuf, sizeof (outbuf), "GET %s%s HTTP/1.1" CRLF "Connection: close" CRLF "Host: %s" CRLF, (*data->path == '/') ? "" : "/", data->path, data->host);
	if (data->last_checked != 0) {
		r += rspamd_snprintf (outbuf + r, sizeof (outbuf) - r, "If-Modified-Since: %s" CRLF, asctime (gmtime (&data->last_checked)));
	}

	r += rspamd_snprintf (outbuf + r, sizeof (outbuf) - r, CRLF);

	if (write (sock, outbuf, r) == -1) {
		msg_err ("failed to write request: %d, %s", errno, strerror (errno));
	}
}

/**
 * FSM for parsing HTTP reply
 */
static u_char                  *
parse_http_reply (u_char * chunk, size_t len, struct http_reply *reply)
{
	u_char                         *s, *p, *err_str, *tmp;
	p = chunk;
	s = chunk;

	while (p - chunk < len) {
		switch (reply->parser_state) {
			/* Search status code */
		case 0:
			/* Search for status code */
			if (*p != ' ') {
				p++;
			}
			else {
				/* Try to parse HTTP reply code */
				reply->code = strtoul (++p, (gchar **)&err_str, 10);
				if (*err_str != ' ') {
					msg_info ("error while reading HTTP status code: %s", p);
					return NULL;
				}
				/* Now skip to end of status string */
				reply->parser_state = 1;
				continue;
			}
			break;
			/* Skip to end of line */
		case 1:
			if (*p == '\n') {
				/* Switch to read header state */
				reply->parser_state = 2;
			}
			/* Each skipped symbol is proceeded */
			s = ++p;
			break;
			/* Read header value */
		case 2:
			if (*p == ':') {
				reply->cur_header = g_malloc (p - s + 1);
				rspamd_strlcpy (reply->cur_header, s, p - s + 1);
				reply->parser_state = 3;
			}
			else if (*p == '\r' && *(p + 1) == '\n') {
				/* Last empty line */
				reply->parser_state = 5;
			}
			p++;
			break;
			/* Skip spaces after header name */
		case 3:
			if (*p != ' ') {
				s = p;
				reply->parser_state = 4;
			}
			else {
				p++;
			}
			break;
			/* Read header value */
		case 4:
			if (*p == '\r') {
				if (reply->cur_header != NULL) {
					tmp = g_malloc (p - s + 1);
					rspamd_strlcpy (tmp, s, p - s + 1);
					g_hash_table_insert (reply->headers, reply->cur_header, tmp);
					reply->cur_header = NULL;
				}
				reply->parser_state = 1;
			}
			p++;
			break;
		case 5:
			/* Set pointer to begining of HTTP body */
			p++;
			s = p;
			reply->parser_state = 6;
			break;
		case 6:
			/* Headers parsed, just return */
			return p;
			break;
		}
	}

	return s;
}

/**
 * Read and parse chunked header
 */
static gint
read_chunk_header (u_char * buf, size_t len, struct http_map_data *data)
{
	u_char                          chunkbuf[32], *p, *c;
	gint                            skip = 0;

	p = chunkbuf;
	c = buf;
	/* Find hex digits */
	while (g_ascii_isxdigit (*c) && p - chunkbuf < sizeof (chunkbuf) - 1) {
		*p++ = *c++;
		skip++;
	}
	*p = '\0';
	data->chunk = strtoul (chunkbuf, NULL, 16);
	/* Now skip to CRLF */
	while (*c != '\n' && c - buf < len) {
		c++;
		skip++;
	}
	if (*c == '\n') {
		skip++;
		c++;
	}
	data->chunk_read = 0;

	return skip;
}

/**
 * Helper callback for reading chunked reply
 */
static                          gboolean
read_http_chunked (u_char * buf, size_t len, struct rspamd_map *map, struct http_map_data *data, struct map_cb_data *cbdata)
{
	u_char                         *p = buf, *remain;
	guint32                         skip = 0;

	if (data->chunk == 0) {
		/* Read first chunk data */
		skip = read_chunk_header (buf, len, data);
		p += skip;
	}

	len -= skip;
	data->chunk_read += len;
	if (data->chunk_read >= data->chunk) {
		/* Read next chunk and feed callback with remaining buffer */
		remain = map->read_callback (map->pool, p, len - (data->chunk_read - data->chunk), cbdata);
		if (remain != NULL && remain != p) {
			/* copy remaining buffer to start of buffer */
			data->rlen = len - (remain - p);
			memmove (p, remain, data->rlen);
		}

		p = buf + (len - (data->chunk_read - data->chunk));
		if (*p != '\r') {
			if (*p == '0') {
				return TRUE;
			}
			else {
				msg_info ("invalid chunked reply: %*s", (gint)len, buf);
				return FALSE;
			}
		}
		p += 2;
		if (len == p - buf) {
			/* Next chunk data is not available */
			data->chunk = 0;
			return TRUE;
		}

		len -= p - buf;
		skip = read_chunk_header (p, len, data);
		p += skip;
		len -= skip;
		if (data->chunk == 0) {
			return FALSE;
		}
	}

	remain = map->read_callback (map->pool, p, len, cbdata);
	if (remain != NULL && remain != p + len) {
		/* copy remaining buffer to start of buffer */
		data->rlen = len - (remain - p);
		memmove (p, remain, data->rlen);
	}

	return TRUE;
}

/**
 * Callback for reading HTTP reply
 */
static                          gboolean
read_http_common (struct rspamd_map *map, struct http_map_data *data, struct http_reply *reply, struct map_cb_data *cbdata, gint fd)
{
	u_char                         *remain, *pos;
	ssize_t                         r;
	gchar                           *te;
	
	if ((r = read (fd, data->read_buf + data->rlen, sizeof (data->read_buf) - data->rlen)) > 0) {
		r += data->rlen;
		data->rlen = 0;
		remain = parse_http_reply (data->read_buf, r, reply);
		if (remain != NULL && remain != data->read_buf) {
			/* copy remaining data->read_buffer to start of data->read_buffer */
			data->rlen = r - (remain - data->read_buf);
			memmove (data->read_buf, remain, data->rlen);
			r = data->rlen;
			data->rlen = 0;
		}
		if (r <= 0) {
			return TRUE;
		}
		if (reply->parser_state == 6) {
			/* If reply header is parsed successfully, try to read further data */
			if (reply->code != 200 && reply->code != 304) {
				msg_err ("got error reply from server %s, %d", data->host, reply->code);
				return FALSE;
			}
			else if (reply->code == 304) {
				/* Do not read anything */
				return FALSE;
			}
			pos = data->read_buf;
			/* Check for chunked */
			if (!data->chunked && (te = g_hash_table_lookup (reply->headers, "Transfer-Encoding")) != NULL) {
				if (g_ascii_strcasecmp (te, "chunked") == 0) {
					data->chunked = TRUE;
				}
			}
			if (data->chunked) {
				return read_http_chunked (data->read_buf, r, map, data, cbdata);
			}
			/* Read more data */
			remain = map->read_callback (map->pool, pos, r, cbdata);
			if (remain != NULL && remain != pos + r) {
				/* copy remaining data->read_buffer to start of data->read_buffer */
				data->rlen = r - (remain - pos);
				memmove (pos, remain, data->rlen);
			}
		}
	}
	else {
		return FALSE;
	}

	return TRUE;
}

/**
 * Sync read of HTTP reply
 */
static void
read_http_sync (struct rspamd_map *map, struct http_map_data *data)
{
	struct map_cb_data              cbdata;
	gint                            fd;
	struct http_reply              *repl;

	if (map->read_callback == NULL || map->fin_callback == NULL) {
		msg_err ("bad callback for reading map file");
		return;
	}

	/* Connect synced */
	if ((fd = connect_http (map, data, FALSE)) == -1) {
		return;
	}
	write_http_request (map, data, fd);

	cbdata.state = 0;
	cbdata.prev_data = *map->user_data;
	cbdata.cur_data = NULL;

	repl = g_malloc (sizeof (struct http_reply));
	repl->parser_state = 0;
	repl->code = 404;
	repl->headers = g_hash_table_new_full (rspamd_strcase_hash, rspamd_strcase_equal, g_free, g_free);

	while (read_http_common (map, data, repl, &cbdata, fd));

	close (fd);

	map->fin_callback (map->pool, &cbdata);
	*map->user_data = cbdata.cur_data;
	data->last_checked = time (NULL);

	g_hash_table_destroy (repl->headers);
	g_free (repl);
}

/**
 * Callback for reading data from file
 */
static void
read_map_file (struct rspamd_map *map, struct file_map_data *data)
{
	struct map_cb_data              cbdata;
	u_char                          buf[BUFSIZ], *remain;
	ssize_t                         r;
	gint                            fd, rlen;

	if (map->read_callback == NULL || map->fin_callback == NULL) {
		msg_err ("bad callback for reading map file");
		return;
	}

	if ((fd = open (data->filename, O_RDONLY)) == -1) {
		msg_warn ("cannot open file '%s': %s", data->filename, strerror (errno));
		return;
	}

	cbdata.state = 0;
	cbdata.prev_data = *map->user_data;
	cbdata.cur_data = NULL;

	rlen = 0;
	while ((r = read (fd, buf + rlen, sizeof (buf) - rlen - 1)) > 0) {
		r += rlen;
		buf[r] = '\0';
		remain = map->read_callback (map->pool, buf, r, &cbdata);
		if (remain != NULL) {
			/* copy remaining buffer to start of buffer */
			rlen = r - (remain - buf);
			memmove (buf, remain, rlen);
		}
	}

	close (fd);

	map->fin_callback (map->pool, &cbdata);
	*map->user_data = cbdata.cur_data;
}

gboolean
check_map_proto (const gchar *map_line, gint *res, const gchar **pos)
{
	if (g_ascii_strncasecmp (map_line, "http://", sizeof ("http://") - 1) == 0) {
		if (res && pos) {
			*res = PROTO_HTTP;
			*pos = map_line + sizeof ("http://") - 1;
		}
	}
	else if (g_ascii_strncasecmp (map_line, "file://", sizeof ("file://") - 1) == 0) {
		if (res && pos) {
			*res = PROTO_FILE;
			*pos = map_line + sizeof ("file://") - 1;
		}
	}
	else {
		msg_warn ("invalid map fetching protocol: %s", map_line);
		return FALSE;
	}

	return TRUE;
}

gboolean
add_map (const gchar *map_line, map_cb_t read_callback, map_fin_cb_t fin_callback, void **user_data)
{
	struct rspamd_map              *new_map;
	enum fetch_proto                proto;
	const gchar                     *def, *p, *hostend;
	struct file_map_data           *fdata;
	struct http_map_data           *hdata;
	gchar                           portbuf[6];
	gint                            i, s, fd;
	struct hostent                 *hent;

	/* First of all detect protocol line */
	if (!check_map_proto (map_line, (int *)&proto, &def)) {
		return FALSE;
	}
	/* Constant pool */
	if (map_pool == NULL) {
		map_pool = memory_pool_new (memory_pool_get_size ());
	}
	new_map = memory_pool_alloc0 (map_pool, sizeof (struct rspamd_map));
	new_map->read_callback = read_callback;
	new_map->fin_callback = fin_callback;
	new_map->user_data = user_data;
	new_map->protocol = proto;

	/* Now check for each proto separately */
	if (proto == PROTO_FILE) {
		if ((fd = open (def, O_RDONLY)) == -1) {
			msg_warn ("cannot open file '%s': %s", def, strerror (errno));
			return FALSE;
		}
		fdata = memory_pool_alloc0 (map_pool, sizeof (struct file_map_data));
		fdata->filename = memory_pool_strdup (map_pool, def);
		fstat (fd, &fdata->st);
		new_map->map_data = fdata;
	}
	else if (proto == PROTO_HTTP) {
		hdata = memory_pool_alloc0 (map_pool, sizeof (struct http_map_data));
		/* Try to search port */
		if ((p = strchr (def, ':')) != NULL) {
			hostend = p;
			i = 0;
			p++;
			while (g_ascii_isdigit (*p) && i < sizeof (portbuf) - 1) {
				portbuf[i++] = *p++;
			}
			if (*p != '/') {
				msg_info ("bad http map definition: %s", def);
				return FALSE;
			}
			portbuf[i] = '\0';
			hdata->port = atoi (portbuf);
		}
		else {
			/* Default http port */
			hdata->port = 80;
			/* Now separate host from path */
			if ((p = strchr (def, '/')) == NULL) {
				msg_info ("bad http map definition: %s", def);
				return FALSE;
			}
			hostend = p;
		}
		hdata->host = memory_pool_alloc (map_pool, hostend - def + 1);
		rspamd_strlcpy (hdata->host, def, hostend - def + 1);
		hdata->path = memory_pool_strdup (map_pool, p);
		hdata->rlen = 0;
		/* Now try to resolve */
		if (!inet_aton (hdata->host, &hdata->addr)) {
			/* Resolve using dns */
			hent = gethostbyname (hdata->host);
			if (hent == NULL) {
				msg_info ("cannot resolve: %s", hdata->host);
				return FALSE;
			}
			else {
				memcpy (&hdata->addr, hent->h_addr, sizeof (struct in_addr));
			}
		}
		/* Now try to connect */
		if ((s = make_tcp_socket (&hdata->addr, hdata->port, FALSE, FALSE)) == -1) {
			msg_info ("cannot connect to http server %s: %d, %s", hdata->host, errno, strerror (errno));
			return FALSE;
		}
		close (s);
		new_map->map_data = hdata;
	}
	/* Temp pool */
	new_map->pool = memory_pool_new (memory_pool_get_size ());

	maps = g_list_prepend (maps, new_map);

	return TRUE;
}

/**
 * FSM for parsing lists
 */
u_char                  *
abstract_parse_list (memory_pool_t * pool, u_char * chunk, size_t len, struct map_cb_data *data, insert_func func)
{
	u_char                         *s, *p, *str, *start;

	p = chunk;
	start = p;

	str = g_malloc (len + 1);
	s = str;

	while (p - chunk < len) {
		switch (data->state) {
			/* READ_SYMBOL */
		case 0:
			if (*p == '#') {
				/* Got comment */
				if (s != str) {
					/* Save previous string in lines like: "127.0.0.1 #localhost" */
					*s = '\0';
					s = memory_pool_strdup (pool, g_strstrip (str));
					if (strlen (s) > 0) {
						func (data->cur_data, s, hash_fill);
					}
					s = str;
					start = p;
				}
				data->state = 1;
			}
			else if (*p == '\r' || *p == '\n') {
				/* Got EOL marker, save stored string */
				if (s != str) {
					*s = '\0';
					s = memory_pool_strdup (pool, g_strstrip (str));
					if (strlen (s) > 0) {
						func (data->cur_data, s, hash_fill);
					}
					s = str;
				}
				/* Skip EOL symbols */
				while ((*p == '\r' || *p == '\n') && p - chunk < len) {
					p++;
				}
				start = p;
			}
			else {
				/* Store new string in s */
				*s = *p;
				s++;
				p++;
			}
			break;
			/* SKIP_COMMENT */
		case 1:
			/* Skip comment till end of line */
			if (*p == '\r' || *p == '\n') {
				while ((*p == '\r' || *p == '\n') && p - chunk < len) {
					p++;
				}
				s = str;
				start = p;
				data->state = 0;
			}
			else {
				p++;
			}
			break;
		}
	}

	g_free (str);

	return start;
}

/**
 * Radix tree helper function
 */
static void
radix_tree_insert_helper (gpointer st, gconstpointer key, gpointer value)
{
	radix_tree_t                   *tree = st;

	guint32                         mask = 0xFFFFFFFF;
	guint32                         ip;
	gchar                           *token, *ipnet, *err_str, **strv, **cur;
	struct in_addr                  ina;
	gint                            k;

	/* Split string if there are multiple items inside a single string */
	strv = g_strsplit_set ((gchar *)key, " ,;", 0);
	cur = strv;
	while (*cur) {
		if (**cur == '\0') {
			cur++;
			continue;
		}
		/* Extract ipnet */
		ipnet = *cur;
		token = strsep (&ipnet, "/");

		if (ipnet != NULL) {
			errno = 0;
			/* Get mask */
			k = strtoul (ipnet, &err_str, 10);
			if (errno != 0) {
				msg_warn ("invalid netmask, error detected on symbol: %s, erorr: %s", err_str, strerror (errno));
				k = 32;
			}
			else if (k > 32 || k < 0) {
				msg_warn ("invalid netmask value: %d", k);
				k = 32;
			}
			/* Calculate mask based on CIDR presentation */
			mask = mask << (32 - k);
		}

		/* Check IP */
		if (inet_aton (token, &ina) == 0) {
			msg_err ("invalid ip address: %s", token);
			return;
		}

		/* Insert ip in a tree */
		ip = ntohl ((guint32) ina.s_addr);
		k = radix32tree_insert (tree, ip, mask, 1);
		if (k == -1) {
			msg_warn ("cannot insert ip to tree: %s, mask %X", inet_ntoa (ina), mask);
		}
		else if (k == 1) {
			msg_warn ("ip %s, mask %X, value already exists", inet_ntoa (ina), mask);
		}
		cur++;
	}

	g_strfreev (strv);
}

/* Helpers */
u_char                         *
read_host_list (memory_pool_t * pool, u_char * chunk, size_t len, struct map_cb_data *data)
{
	if (data->cur_data == NULL) {
		data->cur_data = g_hash_table_new (rspamd_strcase_hash, rspamd_strcase_equal);
	}
	return abstract_parse_list (pool, chunk, len, data, (insert_func) g_hash_table_insert);
}

void
fin_host_list (memory_pool_t * pool, struct map_cb_data *data)
{
	if (data->prev_data) {
		g_hash_table_destroy (data->prev_data);
	}
}

u_char                         *
read_radix_list (memory_pool_t * pool, u_char * chunk, size_t len, struct map_cb_data *data)
{
	if (data->cur_data == NULL) {
		data->cur_data = radix_tree_create ();
	}
	return abstract_parse_list (pool, chunk, len, data, (insert_func) radix_tree_insert_helper);
}

void
fin_radix_list (memory_pool_t * pool, struct map_cb_data *data)
{
	if (data->prev_data) {
		radix_tree_free (data->prev_data);
	}
}

/**
 * Common file callback
 */
static void
file_callback (gint fd, short what, void *ud)
{
	struct rspamd_map              *map = ud;
	struct file_map_data           *data = map->map_data;
	struct stat                     st;

	/* Plan event again with jitter */
	evtimer_del (&map->ev);
	map->tv.tv_sec = MON_TIMEOUT + MON_TIMEOUT * g_random_double ();
	map->tv.tv_usec = 0;
	evtimer_add (&map->ev, &map->tv);

	if (stat (data->filename, &st) != -1 && st.st_mtime > data->st.st_mtime) {
		/* File was modified since last check */
		memcpy (&data->st, &st, sizeof (struct stat));
	}
	else {
		return;
	}

	msg_info ("rereading map file %s", data->filename);
	read_map_file (map, data);
}

/**
 * Callback for destroying HTTP callback data
 */
static void
free_http_cbdata (struct http_callback_data *cbd)
{
	if (cbd->reply) {
		g_hash_table_destroy (cbd->reply->headers);
		g_free (cbd->reply);
	}
	event_del (&cbd->ev);
	close (cbd->fd);
	g_free (cbd);
}

/**
 * Async HTTP request parser
 */
static void
http_async_callback (gint fd, short what, void *ud)
{
	struct http_callback_data      *cbd = ud;

	/* Begin of connection */
	if (what == EV_WRITE) {
		if (cbd->state == 0) {
			/* Can write request */
			write_http_request (cbd->map, cbd->data, fd);
			/* Plan reading */
			event_set (&cbd->ev, cbd->fd, EV_READ | EV_PERSIST, http_async_callback, cbd);
			cbd->tv.tv_sec = HTTP_READ_TIMEOUT;
			cbd->tv.tv_usec = 0;
			cbd->state = 1;
			/* Allocate reply structure */
			cbd->reply = g_malloc (sizeof (struct http_reply));
			cbd->reply->parser_state = 0;
			cbd->reply->code = 404;
			cbd->reply->headers = g_hash_table_new_full (rspamd_strcase_hash, rspamd_strcase_equal, g_free, g_free);
			cbd->cbdata.state = 0;
			cbd->cbdata.prev_data = *cbd->map->user_data;
			cbd->cbdata.cur_data = NULL;

			event_add (&cbd->ev, &cbd->tv);
		}
		else {
			msg_err ("bad state when got write readiness");
			free_http_cbdata (cbd);
			return;
		}
	}
	/* Got reply, parse it */
	else if (what == EV_READ) {
		if (cbd->state >= 1) {
			if (!read_http_common (cbd->map, cbd->data, cbd->reply, &cbd->cbdata, cbd->fd)) {
				/* Handle Not-Modified in a special way */
				if (cbd->reply->code == 304) {
					cbd->data->last_checked = time (NULL);
					msg_info ("data is not modified for server %s", cbd->data->host);
				}
				else if (cbd->cbdata.cur_data != NULL) {
					/* Destroy old data and start reading request data */
					cbd->map->fin_callback (cbd->map->pool, &cbd->cbdata);
					*cbd->map->user_data = cbd->cbdata.cur_data;
					cbd->data->last_checked = time (NULL);
				}
				if (cbd->state == 1 && cbd->reply->code == 200) {
					/* Write to log that data is modified */
					msg_info ("rereading map data from %s", cbd->data->host);
				}

				free_http_cbdata (cbd);
				return;
			}
			else if (cbd->state == 1) {
				/* Write to log that data is modified */
				msg_info ("rereading map data from %s", cbd->data->host);
			}
			cbd->state = 2;
		}
	}
	else {
		msg_err ("connection with http server terminated incorrectly");
		free_http_cbdata (cbd);
	}
}

/**
 * Async HTTP callback
 */
static void
http_callback (gint fd, short what, void *ud)
{
	struct rspamd_map              *map = ud;
	struct http_map_data           *data = map->map_data;
	gint                            sock;
	struct http_callback_data      *cbd;

	/* Plan event again with jitter */
	evtimer_del (&map->ev);
	map->tv.tv_sec = MON_TIMEOUT + MON_TIMEOUT * g_random_double ();
	map->tv.tv_usec = 0;
	evtimer_add (&map->ev, &map->tv);

	/* Connect asynced */
	if ((sock = connect_http (map, data, TRUE)) == -1) {
		return;
	}
	else {
		/* Plan event */
		cbd = g_malloc (sizeof (struct http_callback_data));
		event_set (&cbd->ev, sock, EV_WRITE, http_async_callback, cbd);
		cbd->tv.tv_sec = HTTP_CONNECT_TIMEOUT;
		cbd->tv.tv_usec = 0;
		cbd->map = map;
		cbd->data = data;
		cbd->state = 0;
		cbd->fd = sock;
		cbd->reply = NULL;
		event_add (&cbd->ev, &cbd->tv);
	}
}

/* Start watching event for all maps */
void
start_map_watch (void)
{
	GList                          *cur = maps;
	struct rspamd_map              *map;

	/* First of all do synced read of data */
	while (cur) {
		map = cur->data;
		if (map->protocol == PROTO_FILE) {
			evtimer_set (&map->ev, file_callback, map);
			/* Read initial data */
			read_map_file (map, map->map_data);
			/* Plan event with jitter */
			map->tv.tv_sec = MON_TIMEOUT + MON_TIMEOUT * g_random_double ();
			map->tv.tv_usec = 0;
			evtimer_add (&map->ev, &map->tv);
		}
		else if (map->protocol == PROTO_HTTP) {
			evtimer_set (&map->ev, http_callback, map);
			/* Read initial data */
			read_http_sync (map, map->map_data);
			/* Plan event with jitter */
			map->tv.tv_sec = MON_TIMEOUT + MON_TIMEOUT * g_random_double ();
			map->tv.tv_usec = 0;
			evtimer_add (&map->ev, &map->tv);
		}
		cur = g_list_next (cur);
	}
}

void 
remove_all_maps (void)
{
	GList                          *cur = maps;
	struct rspamd_map              *map;

	/* First of all do synced read of data */
	while (cur) {
		map = cur->data;
		event_del (&map->ev);
		cur = g_list_next (cur);
	}
	g_list_free (maps);
	maps = NULL;
	memory_pool_delete (map_pool);
	map_pool = NULL;
}

