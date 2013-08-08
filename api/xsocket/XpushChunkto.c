/* ts=4 */
/*
** Copyright 2011 Carnegie Mellon University
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**    http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
/*!
** @file XpushChunkto.c
** @brief implements XpushChunkto()
*/
#include "Xsocket.h"
#include "Xinit.h"
#include "Xutil.h"
#include <errno.h>
#include "dagaddr.hpp"

/*!
** @brief Sends a datagram chunk message on an Xsocket
**
** Internally calls XputChunk 
** XpushChunkto sends a datagram style chunk to the specified address. The final intent of
** the address should be a valid SID or HID.
** 
** Unlike a standard socket, XpushChunkto() is only valid on Xsockets of
** type XSOCK_CHUNK. FIXME: Maybe there should be chunk_datagram
**
** If the buffer is too large (bigger than XIA_MAXCHUNK), XpushChunkto() will return an error.
**
** @param ChunkContext Chunk context including socket, ttl, etc
** @param buf the data to send
** @param len lenngth of the data to send. The
** Xsendto api is limited to sending at most XIA_MAXBUF bytes.
** @param flags (This is not currently used but is kept to be compatible
** with the standard sendto socket call.
** @param addr address (SID) to send the datagram to
** @param addrlen length of the DAG
** @param ChunkInfo cid, ttl, len, etc
**
** @returns number of bytes sent on success
** @returns -1 on failure with errno set to an error compatible with those
** returned by the standard sendto call.
**
*/
int XpushChunkto(const ChunkContext *ctx, const char *buf, size_t len, int flags,
		const struct sockaddr *addr, socklen_t addrlen, ChunkInfo *info)
{
	
	int rc;
// 	char buffer[MAXBUFLEN];
	
	if ((rc = XputChunk(ctx, buf, len, info)) < 0)
		return rc;
	

/*	if(ctx == NULL || buf == NULL || info == NULL || !addr)*/
	if(!addr){
			errno = EFAULT;
			LOG("NULL pointer");
		return -1;
	}

// 	if (len == 0)
// 		return 0;


	if (addrlen < sizeof(sockaddr_x)) {
		errno = EINVAL;
		return -1;
	}

	if (flags != 0) {
		LOG("the flags parameter is not currently supported");
		errno = EINVAL;
		return -1;
	}

	if (validateSocket(ctx->sockfd, XSOCK_CHUNK, EOPNOTSUPP) < 0) {
		LOGF("Socket %d must be a chunk socket", ctx->sockfd);
		return -1;
	}

	// if buf is too big, send only what we can
	if (len > XIA_MAXBUF) {
		LOGF("truncating... requested size (%d) is larger than XIA_MAXBUF (%d)\n", 
				len, XIA_MAXBUF);
		len = XIA_MAXBUF;
	}

	xia::XSocketMsg xsm;
	xsm.set_type(xia::XPUSHCHUNKTO);

	xia::X_Pushchunkto_Msg *x_pushchunkto = xsm.mutable_x_pushchunkto();

	// FIXME: validate addr
	Graph g((sockaddr_x*)addr);
	std::string s = g.dag_string();

	x_pushchunkto->set_ddag(s.c_str());
	x_pushchunkto->set_payload((const char*)buf, len);
	
	x_pushchunkto->set_contextid(ctx->contextID);
	x_pushchunkto->set_ttl(ctx->ttl);
	x_pushchunkto->set_cachesize(ctx->cacheSize);
	x_pushchunkto->set_cachepolicy(ctx->cachePolicy);

	if ((rc = click_send(ctx->sockfd, &xsm)) < 0) {
		LOGF("Error talking to Click: %s", strerror(errno));
		return -1;
	}

	// because we don't have queueing or seperate control and data sockets, we 
	// can't get status back reliably on a datagram socket as multiple peers
	// could be talking to it at the same time and the control messages can get
	// mixed up with the data packets. So just assume that all went well and tell
	// the caller we sent the data

	return len;
}