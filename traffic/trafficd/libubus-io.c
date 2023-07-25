/*
 * Copyright (C) 2011-2014 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Changes:
 * yubo    <yubo@xiaomi.com>
 */

#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>

#include <unistd.h>
#include <fcntl.h>

#ifdef __ECOS
#include <sys/select.h>
#else
#include <poll.h>
#endif

#include <libubox/blob.h>
#include <libubox/blobmsg.h>

#include "traffic/trafficd.h"
#include "traffic/libubus.h"
#include "traffic/libubus-internal.h"

#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_PKGSIZ  10240


#define STATIC_IOV(_var) { .iov_base = (char *) &(_var), .iov_len = sizeof(_var) }

#define UBUS_MSGBUF_REDUCTION_INTERVAL	16

static const struct blob_attr_info ubus_policy[UBUS_ATTR_MAX] = {
	[UBUS_ATTR_STATUS] = { .type = BLOB_ATTR_INT32 },
	[UBUS_ATTR_OBJID] = { .type = BLOB_ATTR_INT32 },
	[UBUS_ATTR_OBJPATH] = { .type = BLOB_ATTR_STRING },
	[UBUS_ATTR_METHOD] = { .type = BLOB_ATTR_STRING },
	[UBUS_ATTR_ACTIVE] = { .type = BLOB_ATTR_INT8 },
	[UBUS_ATTR_NO_REPLY] = { .type = BLOB_ATTR_INT8 },
	[UBUS_ATTR_SUBSCRIBERS] = { .type = BLOB_ATTR_NESTED },
};

static struct blob_attr *attrbuf[UBUS_ATTR_MAX];

__hidden struct blob_attr **ubus_parse_msg(struct blob_attr *msg)
{
	blob_parse(msg, attrbuf, ubus_policy, UBUS_ATTR_MAX);
	return attrbuf;
}


static int wait_data(int fd, bool write)
{
	fd_set fds;
	struct timeval tv = {.tv_sec = 0, .tv_usec = 10};
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	if(write){
		select(fd + 1, NULL, &fds, NULL, &tv);
	}else{
		select(fd + 1, &fds, NULL, NULL, &tv);
	}
	if(FD_ISSET(fd, &fds)){
		dlog("fd_isset \n");
		return 0;
	}else{
		dlog("fd is not set \n");
		return -1;
	}
}




static int writev_retry(struct ubus_context * ctx, struct iovec *iov, int iov_len, int sock_fd)
{
	static struct {
		struct cmsghdr h;
		int fd;
	} fd_buf = {
		.h = {
			.cmsg_len = sizeof(fd_buf),
			.cmsg_level = SOL_SOCKET,
			.cmsg_type = SCM_RIGHTS,
		}
	};
	struct msghdr msghdr = {
		.msg_iov = iov,
		.msg_iovlen = iov_len,
		.msg_control = &fd_buf,
		.msg_controllen = sizeof(fd_buf),
	};
	int len = 0;

	do {
		int cur_len;

		if (sock_fd < 0) {
			msghdr.msg_control = NULL;
			msghdr.msg_controllen = 0;
		} else {
			fd_buf.fd = sock_fd;
		}

		cur_len = sendmsg(ctx->sock.fd, &msghdr, 0);
		if (cur_len < 0) {
			ctx->retry++;
			switch(errno) {
			case EAGAIN:
				wait_data(ctx->sock.fd, true);
				break;
			case EINTR:
				break;
			default:
				return -1;
			}
			continue;
		}else{
			//D(BUS, "EAGAIN retry %d sendmsg %d(bytes) success\n", ctx->retry, cur_len);
			ctx->retry = 0;
		}


		if (len > 0)
			sock_fd = -1;

		len += cur_len;
		while (cur_len >= iov->iov_len) {
			cur_len -= iov->iov_len;
			iov_len--;
			iov++;
			if (!iov_len)
				return len;
		}
		iov->iov_base = (char *)iov->iov_base + cur_len;
		iov->iov_len -= cur_len;
		msghdr.msg_iov = iov;
		msghdr.msg_iovlen = iov_len;
	} while (1);

	/* Should never reach here */
	return -1;
}

int __hidden ubus_send_msg(struct ubus_context *ctx, uint32_t seq,
			   struct blob_attr *msg, int cmd, uint32_t peer, int fd)
{
	struct ubus_msghdr hdr;
	struct iovec iov[2] = {
		STATIC_IOV(hdr)
	};
	int ret;

	hdr.version = 0;
	hdr.type = cmd;
	hdr.seq = seq;
	hdr.peer = peer;

	if (!msg) {
		blob_buf_init(&b, 0);
		msg = b.head;
	}

	iov[1].iov_base = (char *) msg;
	iov[1].iov_len = blob_raw_len(msg);

	ret = writev_retry(ctx, iov, ARRAY_SIZE(iov), fd);
	if (ret < 0)
		ctx->sock.eof = true;

	if (fd >= 0)
		close(fd);

	return ret;
}


static int recv_retry(struct ubus_context * ctx, struct iovec *iov, bool wait, int *recv_fd)
{
	int bytes, total = 0;
	static struct {
		struct cmsghdr h;
		int fd;
	} fd_buf = {
		.h = {
			.cmsg_type = SCM_RIGHTS,
			.cmsg_level = SOL_SOCKET,
			.cmsg_len = sizeof(fd_buf),
		},
	};
	struct msghdr msghdr = {
		.msg_iov = iov,
		.msg_iovlen = 1,
	};


	while (iov->iov_len > 0) {
		if (wait && wait_data(ctx->sock.fd, false))
			return 0;

		if (recv_fd) {
			msghdr.msg_control = &fd_buf;
			msghdr.msg_controllen = sizeof(fd_buf);
		} else {
			msghdr.msg_control = NULL;
			msghdr.msg_controllen = 0;
		}

		fd_buf.fd = -1;
		dlog(" recvmsg ctx->sock.fd[%d] msghdr.msg_control[%d] msghdr.msg_controllen[%d]\n",
				ctx->sock.fd, msghdr.msg_control ? msghdr.msg_control : 0, msghdr.msg_controllen);
		bytes = recvmsg(ctx->sock.fd, &msghdr, 0);
		dlog("recvmsg bytes %d\n", bytes);
		if (!bytes)
			return -1;

		if (bytes < 0) {
			bytes = 0;
			ctx->retry++;
			if (uloop_cancelled)
				return 0;
			if (errno == EINTR)
				continue;

			if (errno != EAGAIN)
				return -1;
		}else{
			//D(BUS, "EAGAIN retry %d recvmsg %d(bytes) success\n", ctx->retry, bytes);
			ctx->retry = 0;
		}

		if (!wait && !bytes){
			return 0;
		}

		if (recv_fd)
			*recv_fd = fd_buf.fd;

		recv_fd = NULL;

		wait = true;
		iov->iov_len -= bytes;
		iov->iov_base = (char *)iov->iov_base + bytes;
		total += bytes;
	}

	return total;
}

static bool ubus_validate_hdr(struct ubus_msghdr *hdr)
{
	struct blob_attr *data = (struct blob_attr *) (hdr + 1);

	if (hdr->version != 0)
		return false;

	if (blob_raw_len(data) < sizeof(*data))
		return false;

	if (blob_pad_len(data) > UBUS_MAX_MSGLEN)
		return false;

	return true;
}

static bool get_next_msg(struct ubus_context *ctx, int *recv_fd)
{
	struct {
		struct ubus_msghdr hdr;
		struct blob_attr data;
	} hdrbuf;
	struct iovec iov = STATIC_IOV(hdrbuf);
	int len;
	int r;
	void *p;

	/* receive header + start attribute */
	dlog("recv_retry\n");
	r = recv_retry(ctx, &iov, false, recv_fd);
	if (r <= 0) {
		if (r < 0)
			ctx->sock.eof = true;

		return false;
	}

	if (!ubus_validate_hdr(&hdrbuf.hdr)){
		return false;
	}


	len = blob_raw_len(&hdrbuf.data);
	if (len > ctx->msgbuf_data_len) {
		ctx->msgbuf_reduction_counter = UBUS_MSGBUF_REDUCTION_INTERVAL;
	} else if (ctx->msgbuf_data_len > UBUS_MSG_CHUNK_SIZE) {
		if (ctx->msgbuf_reduction_counter > 0) {
			len = -1;
			--ctx->msgbuf_reduction_counter;
		} else
			len = UBUS_MSG_CHUNK_SIZE;
	} else
		len = -1;

	if (len > -1) {
		p = ctx->msgbuf.data;
		ctx->msgbuf.data = realloc(ctx->msgbuf.data, len * sizeof(char));
		if (ctx->msgbuf.data)
			ctx->msgbuf_data_len = len;
		else
			free(p);
	}
	if (!ctx->msgbuf.data)
		return false;

	memcpy(&ctx->msgbuf.hdr, &hdrbuf.hdr, sizeof(hdrbuf.hdr));
	memcpy(ctx->msgbuf.data, &hdrbuf.data, sizeof(hdrbuf.data));


	iov.iov_base = (char *)ctx->msgbuf.data + sizeof(hdrbuf.data);
	iov.iov_len = blob_len(ctx->msgbuf.data);
	dlog("recv_retry wait true iov.iov_len[%d]\n", iov.iov_len);
	if (iov.iov_len > 0 && !recv_retry(ctx, &iov, true, NULL))
		return false;

	return true;
}


void __hidden ubus_handle_data(struct uloop_fd *u, unsigned int events)
{
	struct ubus_context *ctx = container_of(u, struct ubus_context, sock);
	struct ubus_msghdr *hdr = &ctx->msgbuf.hdr;
	int left, offset, len, again;
	ssize_t bytes;
	char buff[MAX_PKGSIZ];
	void *p;
	struct iovec iov = STATIC_IOV(buff);
	struct msghdr msghdr = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = NULL,
		.msg_controllen = 0,
	};
	struct hdrbuf{
		struct ubus_msghdr hdr;
		struct blob_attr data;
	} *hdrbuf;

again:
	bytes = recvmsg(ctx->sock.fd, &msghdr, 0);

	if(bytes < sizeof(buff))
		again = 0;
	else
		again = 1;

	//dlog("recvmsg[%d/%d] sizeof(*hdrbuf)[%d], sizeof(hdrbuf->data)[%d]\n", bytes, sizeof(buff), sizeof(*hdrbuf), sizeof(struct blob_attr));
	if(bytes == 0){
		ctx->sock.eof = true;
	}
	offset = 0;

	while(bytes > 0){
		//dlog("offset[%d], bytes[%d], msgbuf_data_offset[%d]\n", offset, bytes, ctx->msgbuf_data_offset);

		if (uloop_cancelled){
			goto reset;
		}

		if(ctx->msgbuf_data_offset >= sizeof(struct blob_attr)){
			if(bytes >= ctx->msg_data_len - ctx->msgbuf_data_offset){
				memcpy((char *)ctx->msgbuf.data + ctx->msgbuf_data_offset,
						(char *)buff + offset, ctx->msg_data_len - ctx->msgbuf_data_offset);
				offset += ctx->msg_data_len - ctx->msgbuf_data_offset;
				bytes -= ctx->msg_data_len - ctx->msgbuf_data_offset;
				ctx->msgbuf_data_offset = 0;
				dlog("offset[%d], bytes[%d], msgbuf_data_offset[%d], ctx->msg_data_len[%d]\n",
					offset, bytes, ctx->msgbuf_data_offset, ctx->msg_data_len);
				ubus_process_msg(ctx, hdr, -1);
			}else{
				memcpy((char *)ctx->msgbuf.data + ctx->msgbuf_data_offset,
						(char *)buff + offset, bytes);
				offset += bytes;
				ctx->msgbuf_data_offset += bytes;
				bytes = 0;
				//dlog("offset[%d], bytes[%d], msgbuf_data_offset[%d], ctx->msg_data_len[%d]\n",
				//	offset, bytes, ctx->msgbuf_data_offset, ctx->msg_data_len);
			}
		}else{  /* msgbuff_data_offset == 0 */
			hdrbuf = (struct hdrbuf *)buff;
			if(bytes < sizeof(*hdrbuf))
				goto reset;

			if (!ubus_validate_hdr(&hdrbuf->hdr)){
				goto reset;
			}

			len = blob_raw_len(&hdrbuf->data);
			if (len > ctx->msgbuf_data_len) {
				ctx->msgbuf_reduction_counter = UBUS_MSGBUF_REDUCTION_INTERVAL;
			} else if (ctx->msgbuf_data_len > UBUS_MSG_CHUNK_SIZE) {
				if (ctx->msgbuf_reduction_counter > 0) {
					len = -1;
					--ctx->msgbuf_reduction_counter;
				} else
					len = UBUS_MSG_CHUNK_SIZE;
			} else
				len = -1;

			if (len > -1) {
				p = ctx->msgbuf.data;
				ctx->msgbuf.data = realloc(ctx->msgbuf.data, len * sizeof(char));
				if (ctx->msgbuf.data)
					ctx->msgbuf_data_len = len;
				else
					free(p);
			}
			if (!ctx->msgbuf.data)
				goto reset;

			memcpy(&ctx->msgbuf.hdr, &hdrbuf->hdr, sizeof(hdrbuf->hdr));
			memcpy(ctx->msgbuf.data, &hdrbuf->data, sizeof(hdrbuf->data));
			bytes -= sizeof(*hdrbuf);
			offset += sizeof(*hdrbuf);
			ctx->msgbuf_data_offset = sizeof(hdrbuf->data);
			ctx->msg_data_len = blob_raw_len(&hdrbuf->data);
			//dlog("offset[%d], bytes[%d], msgbuf_data_offset[%d] ctx->msg_data_len[%d]\n",
			//	offset, bytes, ctx->msgbuf_data_offset, ctx->msg_data_len);
		}
	}

	//if(again)
	//	goto again;

	if (u->eof){
		ctx->connection_lost(ctx);
	}
	//dlog("leavefunction\n");
	return;

reset:
	ctx->msgbuf_data_offset = 0;
	if (u->eof){
		ctx->connection_lost(ctx);
	}
	//dlog("leavefunction");
}

void __hidden ubus_poll_data(struct ubus_context *ctx, int timeout)
{
	fd_set fds;
	struct timeval tv;

	if (timeout >= 0) {
		tv.tv_sec = timeout / 1000;
		tv.tv_usec = (timeout % 1000) * 1000;
	}

	FD_ZERO(&fds);
	FD_SET(ctx->sock.fd, &fds);
	select(ctx->sock.fd + 1, &fds, NULL, NULL, timeout >= 0 ? &tv : NULL);
	if(FD_ISSET(ctx->sock.fd, &fds)){
		ubus_handle_data(&ctx->sock, ULOOP_READ);
	}
}

static void
ubus_refresh_state(struct ubus_context *ctx)
{
	struct ubus_object *obj, *tmp;
	struct ubus_object **objs;
	int n, i = 0;

	/* clear all type IDs, they need to be registered again */
	avl_for_each_element(&ctx->objects, obj, avl)
		if (obj->type)
			obj->type->id = 0;

	/* push out all objects again */
	objs = malloc(ctx->objects.count * sizeof(*objs));
	avl_remove_all_elements(&ctx->objects, obj, avl, tmp) {
		objs[i++] = obj;
		obj->id = 0;
	}

	for (n = i, i = 0; i < n; i++)
		ubus_replace_object(ctx, objs[i]);
		//ubus_add_object(ctx, objs[i]);
	free(objs);
	ctx->msgbuf_data_offset = 0;
}

static int
connect_socket(struct sockaddr_in *a)
{
    int s;

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        //perror("socket");
        close(s);
        return -1;
    }

    if (connect(s, (struct sockaddr *)a, sizeof(*a)) == -1) {
        //perror("connect()");
        shutdown(s, SHUT_RDWR);
        close(s);
        return -1;
    }
    return s;
}


int tbus_reconnect(struct ubus_context *ctx, struct sockaddr_in *a)
{
	struct {
		struct ubus_msghdr hdr;
		struct blob_attr data;
	} hdr;
	struct blob_attr *buf;
	int ret = UBUS_STATUS_UNKNOWN_ERROR;

	if (ctx->sock.fd >= 0) {
		if (ctx->sock.registered)
			uloop_fd_delete(&ctx->sock);

		close(ctx->sock.fd);
	}

	ctx->sock.fd = connect_socket(a);
	if (ctx->sock.fd < 0)
		return UBUS_STATUS_CONNECTION_FAILED;

	if (read(ctx->sock.fd, &hdr, sizeof(hdr)) != sizeof(hdr))
		goto out_close;

	if (!ubus_validate_hdr(&hdr.hdr))
		goto out_close;

	if (hdr.hdr.type != UBUS_MSG_HELLO)
		goto out_close;

	buf = calloc(1, blob_raw_len(&hdr.data));
	if (!buf)
		goto out_close;

	memcpy(buf, &hdr.data, sizeof(hdr.data));
	if (read(ctx->sock.fd, blob_data(buf), blob_len(buf)) != blob_len(buf))
		goto out_free;

	ctx->local_id = hdr.hdr.peer;
	if (!ctx->local_id)
		goto out_free;

	ret = UBUS_STATUS_OK;

#ifndef T_NONBLOCK
	fcntl(ctx->sock.fd, F_SETFL, fcntl(ctx->sock.fd, F_GETFL) | O_NONBLOCK);
#endif
	ubus_refresh_state(ctx);

out_free:
	free(buf);
out_close:
	if (ret)
		close(ctx->sock.fd);

	return ret;
}
