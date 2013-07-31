#include "../../userlevel/xia.pb.h"
#include <click/config.h>
#include <click/glue.hh>
#include <click/error.hh>
#include <click/confparse.hh>
#include <click/packet_anno.hh>
#include <click/packet.hh>
#include <click/vector.hh>

#include <click/xiacontentheader.hh>
#include "xiatransport.hh"
#include "xtransport.hh"
#include <click/xiatransportheader.hh>

/*
** FIXME:
** - why is xia_socket_msg in the class definition and not a local variable?
** - implement a backoff delay on retransmits so we don't flood the connection
** - fix cid header size issue so we work correctly with the linux version
** - migrate from uisng printf and click_chatter to using the click ErrorHandler class
** - there are still some small memory leaks happening when stream sockets are created/used/closed
**   (problem does not happen if sockets are just opened and closed)
** - fix issue in SYN code with XIDPairToConnectPending (see comment in code for details)
** - what is the constant 22 for near line 850? I can't find a 22 anywhere else in the source tree
*/

#define DEBUG 0

CLICK_DECLS

XTRANSPORT::XTRANSPORT()
	: _timer(this)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	_id = 0;
	isConnected = false;

	_ackdelay_ms = ACK_DELAY;
	_teardown_wait_ms = TEARDOWN_DELAY;

//	pthread_mutexattr_init(&_lock_attr);
//	pthread_mutexattr_settype(&_lock_attr, PTHREAD_MUTEX_RECURSIVE);
//	pthread_mutex_init(&_lock, &_lock_attr);

	cp_xid_type("SID", &_sid_type); 
}


int
XTRANSPORT::configure(Vector<String> &conf, ErrorHandler *errh)
{
	XIAPath local_addr;
	XID local_4id;
	Element* routing_table_elem;
	bool is_dual_stack_router;
	_is_dual_stack_router = false;

	if (cp_va_kparse(conf, this, errh,
					 "LOCAL_ADDR", cpkP + cpkM, cpXIAPath, &local_addr,
					 "LOCAL_4ID", cpkP + cpkM, cpXID, &local_4id,
					 "ROUTETABLENAME", cpkP + cpkM, cpElement, &routing_table_elem,
					 "IS_DUAL_STACK_ROUTER", 0, cpBool, &is_dual_stack_router,
					 cpEnd) < 0)
		return -1;

	_local_addr = local_addr;
	_local_hid = local_addr.xid(local_addr.destination_node());
	_local_4id = local_4id;
	// IP:0.0.0.0 indicates NULL 4ID
	_null_4id.parse("IP:0.0.0.0");

	_is_dual_stack_router = is_dual_stack_router;

	/*
	// If a valid 4ID is given, it is included (as a fallback) in the local_addr
	if(_local_4id != _null_4id) {
		String str_local_addr = _local_addr.unparse();
		size_t AD_found_start = str_local_addr.find_left("AD:");
		size_t AD_found_end = str_local_addr.find_left(" ", AD_found_start);
		String AD_str = str_local_addr.substring(AD_found_start, AD_found_end - AD_found_start);
		String HID_str = _local_hid.unparse();
		String IP4ID_str = _local_4id.unparse();
		String new_local_addr = "RE ( " + IP4ID_str + " ) " + AD_str + " " + HID_str;
		//click_chatter("new address is - %s", new_local_addr.c_str());
		_local_addr.parse(new_local_addr);	
	}
	*/					

#if USERLEVEL
	_routeTable = dynamic_cast<XIAXIDRouteTable*>(routing_table_elem);
#else
	_routeTable = reinterpret_cast<XIAXIDRouteTable*>(routing_table_elem);
#endif

	return 0;
}

XTRANSPORT::~XTRANSPORT()
{
	//Clear all hashtable entries
	XIDtoPort.clear();
	portToSock.clear();
	XIDpairToPort.clear();
	XIDpairToConnectPending.clear();

	hlim.clear();
	xcmp_listeners.clear();
	nxt_xport.clear();

//	pthread_mutex_destroy(&_lock);
//	pthread_mutexattr_destroy(&_lock_attr);
}


int
XTRANSPORT::initialize(ErrorHandler *)
{
	_timer.initialize(this);
	//_timer.schedule_after_msec(1000);
	//_timer.unschedule();
	return 0;
}

char *XTRANSPORT::random_xid(const char *type, char *buf)
{
	// This is a stand-in function until we get certificate based names
	//
	// note: buf must be at least 45 characters long 
	// (longer if the XID type gets longer than 3 characters)
	sprintf(buf, RANDOM_XID_FMT, type, click_random(0, 0xffffffff));

	return buf;
}

void
XTRANSPORT::run_timer(Timer *timer)
{
//	pthread_mutex_lock(&_lock);

	assert(timer == &_timer);

	Timestamp now = Timestamp::now();
	Timestamp earlist_pending_expiry = now;

	WritablePacket *copy;

	bool tear_down;

	for (HashTable<unsigned short, sock>::iterator iter = portToSock.begin(); iter != portToSock.end(); ++iter ) {
		unsigned short _sport = iter->first;
		sock *sk = portToSock.get_pointer(_sport);
		tear_down = false;

		// check if pending
		if (sk->timer_on == true) {
			// check if synack waiting
			if (sk->synack_waiting == true && sk->expiry <= now ) {
				//click_chatter("Timer: synack waiting\n");
		
				if (sk->num_connect_tries <= MAX_CONNECT_TRIES) {

					//click_chatter("Timer: SYN RETRANSMIT! \n");
					copy = copy_packet(sk->syn_pkt, sk);
					// retransmit syn
					XIAHeader xiah(copy);
					// printf("Timer: (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
					output(NETWORK_PORT).push(copy);

					sk->timer_on = true;
					sk->synack_waiting = true;
					sk->expiry = now + Timestamp::make_msec(_ackdelay_ms);
					sk->num_connect_tries++;

				} else {
					// Stop sending the connection request & Report the failure to the application
					sk->timer_on = false;
					sk->synack_waiting = false;

					String str = String("^Connection-failed^");
					WritablePacket *ppp = WritablePacket::make (256, str.c_str(), str.length(), 0);

					if (DEBUG)
						//click_chatter("Timer: Sent packet to socket with port %d", _sport);
                        output(API_PORT).push(UDPIPPrep(ppp, _sport));
				}

			} else if (sk->dataack_waiting == true && sk->expiry <= now ) {

				// adding check to see if anything was retransmitted. We can get in here with
				// no packets in the sk->send_buffer array waiting to go and will stay here forever
				bool retransmit_sent = false;

				if (sk->num_retransmit_tries < MAX_RETRANSMIT_TRIES) {

				//click_chatter("Timer: DATA RETRANSMIT at from (%s) from_port=%d send_base=%d next_seq=%d \n\n", (_local_addr.unparse()).c_str(), _sport, sk->send_base, sk->next_send_seqnum );
		
					// retransmit data
					for (unsigned int i = sk->send_base; i < sk->next_send_seqnum; i++) {
						if (sk->send_buffer[i % sk->send_buffer_size] != NULL) {
							copy = copy_packet(sk->send_buffer[i % sk->send_buffer_size], sk);
							XIAHeader xiah(copy);
							//printf("Timer: (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
							//printf("pusing the retransmit pkt\n");
							output(NETWORK_PORT).push(copy);
							retransmit_sent = true;
						}
					}
				} else {
					//printf("retransmit counter exceeded\n");
					// FIXME what cleanup should happen here?
					// should we do a NAK?
				}

				if (retransmit_sent) {
					//click_chatter("resetting retransmit timer for %d\n", _sport);
					sk->timer_on = true;
					sk->dataack_waiting = true;
					sk-> num_retransmit_tries++;
					sk->expiry = now + Timestamp::make_msec(_ackdelay_ms);
				} else {
					//click_chatter("terminating retransmit timer for %d\n", _sport);
					sk->timer_on = false;
					sk->dataack_waiting = false;
					sk->num_retransmit_tries = 0;
				}

			} else if (sk->teardown_waiting == true && sk->teardown_expiry <= now) {
				tear_down = true;
				sk->timer_on = false;
				portToActive.set(_sport, false);

				//XID source_xid = portToSock.get(_sport).xid;

				// this check for -1 prevents a segfault cause by bad XIDs
				// it may happen in other cases, but opening a XSOCK_STREAM socket, calling
				// XreadLocalHostAddr and then closing the socket without doing anything else will
				// cause the problem
				// TODO: make sure that -1 is the only condition that will cause us to get a bad XID
				if (sk->src_path.destination_node() != -1) {
					XID source_xid = sk->src_path.xid(sk->src_path.destination_node());
					if (!sk->isAcceptSocket) {

						//click_chatter("deleting route %s from port %d\n", source_xid.unparse().c_str(), _sport);
						delRoute(source_xid);
						XIDtoPort.erase(source_xid);
					}
				}

				portToSock.erase(_sport);
				portToActive.erase(_sport);
				hlim.erase(_sport);

				nxt_xport.erase(_sport);
				xcmp_listeners.remove(_sport);
				for (int i = 0; i < sk->send_buffer_size; i++) {
					if (sk->send_buffer[i] != NULL) {
						sk->send_buffer[i]->kill();
						sk->send_buffer[i] = NULL;
					}
				}
			}
		}

		if (tear_down == false) {

			// find the (next) earlist expiry
			if (sk->timer_on == true && sk->expiry > now && ( sk->expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
				earlist_pending_expiry = sk->expiry;
			}
			if (sk->timer_on == true && sk->teardown_expiry > now && ( sk->teardown_expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
				earlist_pending_expiry = sk->teardown_expiry;
			}


			// check for CID request cases
			for (HashTable<XID, bool>::iterator it = sk->XIDtoTimerOn.begin(); it != sk->XIDtoTimerOn.end(); ++it ) {
				XID requested_cid = it->first;
				bool timer_on = it->second;

				HashTable<XID, Timestamp>::iterator it2;
				it2 = sk->XIDtoExpiryTime.find(requested_cid);
				Timestamp cid_req_expiry = it2->second;

				if (timer_on == true && cid_req_expiry <= now) {
					//printf("CID-REQ RETRANSMIT! \n");
					//retransmit cid-request
					HashTable<XID, WritablePacket*>::iterator it3;
					it3 = sk->XIDtoCIDreqPkt.find(requested_cid);
					copy = copy_cid_req_packet(it3->second, sk);
					XIAHeader xiah(copy);
					//printf("\n\n (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
					output(NETWORK_PORT).push(copy);

					cid_req_expiry  = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
					sk->XIDtoExpiryTime.set(requested_cid, cid_req_expiry);
					sk->XIDtoTimerOn.set(requested_cid, true);
				}

				if (timer_on == true && cid_req_expiry > now && ( cid_req_expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
					earlist_pending_expiry = cid_req_expiry;
				}
			}

			portToSock.set(_sport, *sk);
		}
	}

	// Set the next timer
	if (earlist_pending_expiry > now) {
		_timer.reschedule_at(earlist_pending_expiry);
	}

//	pthread_mutex_unlock(&_lock);
}

void
XTRANSPORT::copy_common(sock *sk, XIAHeader &xiahdr, XIAHeaderEncap &xiah) {  

	//Recalculate source path
	XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
	String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse();
	//Make source DAG _local_addr:SID
	String dagstr = sk->src_path.unparse_re();

	//Client Mobility...
	if (dagstr.length() != 0 && dagstr != str_local_addr) {
		//Moved!
		// 1. Update 'sk->src_path'
		sk->src_path.parse_re(str_local_addr);
	}

	xiah.set_nxt(xiahdr.nxt());
	xiah.set_last(xiahdr.last());
	xiah.set_hlim(xiahdr.hlim());
	xiah.set_dst_path(sk->dst_path);
	xiah.set_src_path(sk->src_path);
	xiah.set_plen(xiahdr.plen());
}

WritablePacket *
XTRANSPORT::copy_packet(Packet *p, sock *sk) {  

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(sk, xiahdr, xiah);

	TransportHeader thdr(p);
	TransportHeaderEncap *new_thdr = new TransportHeaderEncap(thdr.type(), thdr.pkt_info(), thdr.seq_num(), thdr.ack_num(), thdr.length());

	WritablePacket *copy = WritablePacket::make(256, thdr.payload(), xiahdr.plen() - thdr.hlen(), 20);

	copy = new_thdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete new_thdr;

	return copy;
}


WritablePacket *
XTRANSPORT::copy_cid_req_packet(Packet *p, sock *sk) {

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(sk, xiahdr, xiah);

	WritablePacket *copy = WritablePacket::make(256, xiahdr.payload(), xiahdr.plen(), 20);

	ContentHeaderEncap *chdr = ContentHeaderEncap::MakeRequestHeader();

	copy = chdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete chdr;
	xiah.set_plen(xiahdr.plen());

	return copy;
}


WritablePacket *
XTRANSPORT::copy_cid_response_packet(Packet *p, sock *sk) {

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(sk, xiahdr, xiah);

	WritablePacket *copy = WritablePacket::make(256, xiahdr.payload(), xiahdr.plen(), 20);

	ContentHeader chdr(p);
	ContentHeaderEncap *new_chdr = new ContentHeaderEncap(chdr.opcode(), chdr.chunk_offset(), chdr.length());

	copy = new_chdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete new_chdr;
	xiah.set_plen(xiahdr.plen());

	return copy;
}

/**
* @brief Checks whether or not a received packet can be buffered.
*
* Checks if we have room to buffer the received packet; that is, is the packet's
* sequence number within our recieve window?
*
* @param p
* @param sk
*
* @return true if packet can be buffered, false otherwise
*/
bool XTRANSPORT::should_buffer_received_packet(WritablePacket *p, sock *sk) {
	
	TransportHeader thdr(p);
	int received_seqnum = thdr.seq_num();

	// check if received_seqnum is within our current recv window
	// TODO: if we switch to a byte-based, buf size, this needs to change
	if (received_seqnum >= sk->next_recv_seqnum &&
		received_seqnum < sk->next_recv_seqnum + sk->recv_buffer_size) {
		return true;
	}
	return false;
}

// TODO: make these two functions check sock_type and act accordingly!!

/**
* @brief Adds a packet to the connection's receive buffer.
*
* Stores the supplied packet pointer, p, in slot seqnum % bufsize.
*
* @param p
* @param sk
*/
void XTRANSPORT::add_packet_to_recv_buf(WritablePacket *p, sock *sk) {

	TransportHeader thdr(p);
	int received_seqnum = thdr.seq_num();
	int index = received_seqnum % sk->recv_buffer_size;

	sk->recv_buffer[index] = p;
}

/**
* @brief Returns the next expected sequence number.
*
* Beginning with sk->recv_base, this function checks consecutive slots
* in the receive buffer and returns the first missing sequence number.
*
* @param sk
*/
uint32_t XTRANSPORT::next_missing_seqnum(sock *sk) {

	uint32_t next_missing = sk->recv_base;
	for (uint32_t i = 0; i < sk->recv_buffer_size; i++) {

		// checking if we have the next consecutive packet
		uint32_t seqnum_to_check = sk->recv_base + i;
		uint32_t index_to_check = seqnum_to_check % sk->recv_buffer_size;

		next_missing = seqnum_to_check;

		if (sk->recv_buffer[index_to_check]) {
			TransportHeader thdr(sk->recv_buffer[index_to_check]);
			if (thdr.seq_num() != seqnum_to_check) {
				break; // found packet, but its seqnum isn't right, so break and return next_missing
			}
		} else {
			break; // no packet here, so break and return next_missing
		}
	}

	return next_missing;
}

void XTRANSPORT::ProcessAPIPacket(WritablePacket *p_in)
{
	//Extract the destination port
	unsigned short _sport = SRC_PORT_ANNO(p_in);

	if (DEBUG)
        click_chatter("\nPush: Got packet from API sport:%d",ntohs(_sport));

	std::string p_buf;
	p_buf.assign((const char*)p_in->data(), (const char*)p_in->end_data());

	//protobuf message parsing
	xia_socket_msg.ParseFromString(p_buf);

	switch(xia_socket_msg.type()) {
	case xia::XSOCKET:
		Xsocket(_sport);
		break;
	case xia::XSETSOCKOPT:
		Xsetsockopt(_sport);
		break;
	case xia::XGETSOCKOPT:
		Xgetsockopt(_sport);
		break;
	case xia::XBIND:
		Xbind(_sport);
		break;
	case xia::XCLOSE:
		Xclose(_sport);
		break;
	case xia::XCONNECT:
		Xconnect(_sport);
		break;
	case xia::XACCEPT:
		Xaccept(_sport);
		break;
	case xia::XCHANGEAD:
		Xchangead(_sport);
		break;
	case xia::XREADLOCALHOSTADDR:
		Xreadlocalhostaddr(_sport);
		break;
	case xia::XUPDATENAMESERVERDAG:
		Xupdatenameserverdag(_sport);
		break;
	case xia::XREADNAMESERVERDAG:
		Xreadnameserverdag(_sport);
		break;
	case xia::XISDUALSTACKROUTER:
		Xisdualstackrouter(_sport);	
		break;
    case xia::XSEND:
		Xsend(_sport, p_in);
		break;
	case xia::XSENDTO:
		Xsendto(_sport, p_in);
		break;
	case xia::XREQUESTCHUNK:
		XrequestChunk(_sport, p_in);
		break;
	case xia::XGETCHUNKSTATUS:
		XgetChunkStatus(_sport);
		break;
	case xia::XREADCHUNK:
		XreadChunk(_sport);
		break;
	case xia::XREMOVECHUNK:
		XremoveChunk(_sport);
		break;
	case xia::XPUTCHUNK:
		XputChunk(_sport);
		break;
	case xia::XGETPEERNAME:
		Xgetpeername(_sport);
		break;
	case xia::XGETSOCKNAME:
		Xgetsockname(_sport);
		break;
	default:
		click_chatter("\n\nERROR: API TRAFFIC !!!\n\n");
		break;
	}

	p_in->kill();
}

void XTRANSPORT::ProcessNetworkPacket(WritablePacket *p_in)
{
	if (DEBUG)
		click_chatter("Got packet from network");

	//Extract the SID/CID
	XIAHeader xiah(p_in->xia_header());
	XIAPath dst_path = xiah.dst_path();
	XID _destination_xid(xiah.hdr()->node[xiah.last()].xid);
	//TODO:In case of stream use source AND destination XID to find port, if not found use source. No TCP like protocol exists though
	//TODO:pass dag back to recvfrom. But what format?

	XIAPath src_path = xiah.src_path();
	XID	_source_xid = src_path.xid(src_path.destination_node());

	unsigned short _dport = XIDtoPort.get(_destination_xid);  // This is to be updated for the XSOCK_STREAM type connections below

	//String pld((char *)xiah.payload(), xiah.plen());
	//printf("\n\n 1. (%s) Received=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), pld.c_str(), xiah.plen());

	TransportHeader thdr(p_in);

	if (xiah.nxt() == CLICK_XIA_NXT_XCMP) {
		// FIXME: This shouldn't strip off the header. raw sockets return raw packets. 
		// (Matt): I've tweaked this to work properly
		// strip off the header and make a writable packet

		String src_path = xiah.src_path().unparse();
		String header((const char*)xiah.hdr(), xiah.hdr_size());
		String payload((const char*)xiah.payload(), xiah.plen());//+xiah.hdr_size());
		//String payload((const char*)thdr.payload(), xiah.plen() - thdr.hlen());
		String str = header + payload;

		xia::XSocketMsg xsm;
		xsm.set_type(xia::XRECV);
		xia::X_Recv_Msg *x_recv_msg = xsm.mutable_x_recv();
		x_recv_msg->set_dag(src_path.c_str());
		x_recv_msg->set_payload(str.c_str(), str.length());

		std::string p_buf;
		xsm.SerializeToString(&p_buf);

		WritablePacket *xcmp_pkt = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

		list<int>::iterator i;

		for (i = xcmp_listeners.begin(); i != xcmp_listeners.end(); i++) {
			int port = *i;
			output(API_PORT).push(UDPIPPrep(xcmp_pkt, port));
		}

		return;

	} else if (thdr.type() == TransportHeader::XSOCK_STREAM) {

	/* =========================================================
	 * TCP input
	 * ========================================================= */
		
		// Obtain sk
		XIDpair xid_pair;
		xid_pair.set_src(_destination_xid);
		xid_pair.set_dst(_source_xid);

		// Get the dst(=receiver) port from XIDpair table
		_dport = XIDpairToPort.get(xid_pair);

		HashTable<unsigned short, bool>::iterator it1;
		it1 = portToActive.find(_dport);

		if(it1 != portToActive.end() ) {
				sock *sk = portToSock.get_pointer(_dport);

				switch(sk->sk_state)
				{
					case TCP_CLOSE:
						goto discard;
				// ....
				}
		}
		
		/* =========================================================
		 * Old XSP's way of handling incoming packet
	 	 * ========================================================= */

		//printf("stream socket dport = %d\n", _dport);
		if (thdr.pkt_info() == TransportHeader::SYN) {
			//printf("syn dport = %d\n", _dport);
			// Connection request from client...

			// First, check if this request is already in the pending queue
			XIDpair xid_pair;
			xid_pair.set_src(_destination_xid);
			xid_pair.set_dst(_source_xid);

			HashTable<XIDpair , bool>::iterator it;
			it = XIDpairToConnectPending.find(xid_pair);

			// FIXME:
			// XIDpairToConnectPending never gets cleared, and will cause problems if matching XIDs
			// were used previously. Commenting out the check for now. Need to look into whether
			// or not we can just get rid of this logic? probably neede for retransmit cases
			// if needed, where should it be cleared???
			if (1) {
//			if (it == XIDpairToConnectPending.end()) {
				// if this is new request, put it in the queue

				// Todo: 1. prepare new sock and store it
				//	 2. send SYNACK to client
				//	   3. Notify the api of SYN reception

				//1. Prepare new sock for this connection
				sock sk;
				sk.port = -1; // just for now. This will be updated via Xaccept call

				sk.sock_type = XSOCKET_STREAM; 

				sk.dst_path = src_path;
				sk.src_path = dst_path;
				sk.isConnected = true;
				sk.initialized = true;
				sk.nxt = LAST_NODE_DEFAULT;
				sk.last = LAST_NODE_DEFAULT;
				sk.hlim = HLIM_DEFAULT;
				sk.seq_num = 0;
				sk.ack_num = 0;

				pending_connection_buf.push(sk);

				// Mark these src & dst XID pair
				XIDpairToConnectPending.set(xid_pair, true);

				//portToSock.set(-1, sk);	// just for now. This will be updated via Xaccept call

			} else {
				// If already in the pending queue, just send back SYNACK to the requester
			
				// if this case is hit, we won't trigger the accept, and the connection will get be left
				// in limbo. see above for whether or not we should even be checking.
				// printf("%06d conn request already pending\n", _dport);
			}


			//2. send SYNACK to client
			//Add XIA headers
			XIAHeaderEncap xiah_new;
			xiah_new.set_nxt(CLICK_XIA_NXT_TRN);
			xiah_new.set_last(LAST_NODE_DEFAULT);
			xiah_new.set_hlim(HLIM_DEFAULT);
			xiah_new.set_dst_path(src_path);
			xiah_new.set_src_path(dst_path);

			const char* dummy = "Connection_granted";
			WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 0);

			WritablePacket *p = NULL;

			xiah_new.set_plen(strlen(dummy));
			//click_chatter("Sent packet to network");

			TransportHeaderEncap *thdr_new = TransportHeaderEncap::MakeSYNACKHeader( 0, 0, 0); // #seq, #ack, length
			p = thdr_new->encap(just_payload_part);

			thdr_new->update();
			xiah_new.set_plen(strlen(dummy) + thdr_new->hlen()); // XIA payload = transport header + transport-layer data

			p = xiah_new.encap(p, false);

			delete thdr_new;
			XIAHeader xiah1(p);
			//String pld((char *)xiah1.payload(), xiah1.plen());
			//printf("\n\n (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), pld.c_str(), xiah1.plen());
			output(NETWORK_PORT).push(p);


			// 3. Notify the api of SYN reception
			//   Done below (via port#5005)

		} else if (thdr.pkt_info() == TransportHeader::SYNACK) {
			XIDpair xid_pair;
			xid_pair.set_src(_destination_xid);
			xid_pair.set_dst(_source_xid);

			// Get the dst port from XIDpair table
			_dport = XIDpairToPort.get(xid_pair);

			sock *sk = portToSock.get_pointer(_dport);

			// Clear timer
			sk->timer_on = false;
			sk->synack_waiting = false;
			//sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

		} else if (thdr.pkt_info() == TransportHeader::DATA) {
			XIDpair xid_pair;
			xid_pair.set_src(_destination_xid);
			xid_pair.set_dst(_source_xid);

			// Get the dst port from XIDpair table
			_dport = XIDpairToPort.get(xid_pair);

			//printf("(%s) my_sport=%d  my_sid=%s  his_sid=%s\n", (_local_addr.unparse()).c_str(),  _dport,  _destination_xid.unparse().c_str(), _source_xid.unparse().c_str());
			HashTable<unsigned short, bool>::iterator it1;
			it1 = portToActive.find(_dport);

			if(it1 != portToActive.end() ) {

				sock *sk = portToSock.get_pointer(_dport);

				// buffer data, if we have room
				if (should_buffer_received_packet(p_in, sk)) {
					add_packet_to_recv_buf(p_in, sk);
					sk->next_recv_seqnum = next_missing_seqnum(sk);
				} 

				portToSock.set(_dport, *sk); 
			
				//In case of Client Mobility...	 Update 'sk->dst_path'
				sk->dst_path = src_path;		

				// send the cumulative ACK to the sender
				//Add XIA headers
				XIAHeaderEncap xiah_new;
				xiah_new.set_nxt(CLICK_XIA_NXT_TRN);
				xiah_new.set_last(LAST_NODE_DEFAULT);
				xiah_new.set_hlim(HLIM_DEFAULT);
				xiah_new.set_dst_path(src_path);
				xiah_new.set_src_path(dst_path);

				const char* dummy = "cumulative_ACK";
				WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 0);

				WritablePacket *p = NULL;

				xiah_new.set_plen(strlen(dummy));

				TransportHeaderEncap *thdr_new = TransportHeaderEncap::MakeACKHeader( 0, sk->next_recv_seqnum, 0); // #seq, #ack, length
				p = thdr_new->encap(just_payload_part);

				thdr_new->update();
				xiah_new.set_plen(strlen(dummy) + thdr_new->hlen()); // XIA payload = transport header + transport-layer data

				p = xiah_new.encap(p, false);
				delete thdr_new;

				XIAHeader xiah1(p);
				String pld((char *)xiah1.payload(), xiah1.plen());
				//printf("\n\n (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), pld.c_str(), xiah1.plen());

				output(NETWORK_PORT).push(p);

			} else {
				printf("destination port not found: %d\n", _dport);
			}

		} else if (thdr.pkt_info() == TransportHeader::ACK) {

			XIDpair xid_pair;
			xid_pair.set_src(_destination_xid);
			xid_pair.set_dst(_source_xid);

			// Get the dst port from XIDpair table
			_dport = XIDpairToPort.get(xid_pair);

			HashTable<unsigned short, bool>::iterator it1;
			it1 = portToActive.find(_dport);

			if(it1 != portToActive.end() ) {
				sock *sk = portToSock.get_pointer(_dport);
			
				//In case of Client Mobility...	 Update 'sk->dst_path'
				sk->dst_path = src_path;					

				int remote_next_seqnum_expected = thdr.ack_num();

				bool resetTimer = false;

				// Clear all Acked packets
				for (int i = sk->send_base; i < remote_next_seqnum_expected; i++) {
					int idx = i % sk->send_buffer_size;
					if (sk->send_buffer[idx]) {
						sk->send_buffer[idx]->kill();
						sk->send_buffer[idx] = NULL;
					}
				
					resetTimer = true;
				}

				// Update the variables
				sk->send_base = remote_next_seqnum_expected;

				// Reset timer
				if (resetTimer) {
					sk->timer_on = true;
					sk->dataack_waiting = true;
					// FIXME: should we reset retransmit_tries here?
					sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

					if (! _timer.scheduled() || _timer.expiry() >= sk->expiry )
						_timer.reschedule_at(sk->expiry);

					if (sk->send_base == sk->next_send_seqnum) {

						// Clear timer
						sk->timer_on = false;
						sk->dataack_waiting = false;
						sk->num_retransmit_tries = 0;
						//sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
					}
				}

				portToSock.set(_dport, *sk);

			} else {
				//printf("port not found\n");
			}

		} else if (thdr.pkt_info() == TransportHeader::FIN) {
			//printf("FIN received, doing nothing\n");
		}
		else {
			printf("UNKNOWN dport = %d hdr=%d\n", _dport, thdr.pkt_info());		
		}

	} else if (thdr.type() == TransportHeader::XSOCK_DGRAM) {

		_dport = XIDtoPort.get(_destination_xid);
		sock *sk = portToSock.get_pointer(_dport);

		// buffer packet if this is a DGRAM socket and we have room
		if (sk->sock_type == XSOCKET_DGRAM &&
			should_buffer_received_packet(p_in, sk)) {
			add_packet_to_recv_buf(p_in, sk);
		}
	
	} else {
		printf("UNKNOWN!!!!! dport = %d\n", _dport);
	}


	/*if(_dport && sendToApplication) {
		//TODO: Refine the way we change DAG in case of migration. Use some control bits. Add verification
		sock sk = portToSock.get(_dport);

		if(sk.initialized == false) {
			sk.dst_path = xiah.src_path();
			sk.initialized = true;
			portToSock.set(_dport, sk);
		}

		// FIXME: what is this? need constant here
		if(xiah.nxt() == 22 && sk.isConnected == true)
		{
			//Verify mobility info
			sk.dst_path = xiah.src_path();
			portToSock.set(_dport, sk);
			click_chatter("Sender moved, update to the new DAG");

		} else {
			//Unparse sock info
			String src_path = xiah.src_path().unparse();
			String payload((const char*)thdr.payload(), xiah.plen() - thdr.hlen());

			xia::XSocketMsg xsm;
			xsm.set_type(xia::XRECV);
			xia::X_Recv_Msg *x_recv_msg = xsm.mutable_x_recv();
			x_recv_msg->set_dag(src_path.c_str());
			x_recv_msg->set_payload(payload.c_str(), payload.length());

			std::string p_buf;
			xsm.SerializeToString(&p_buf);

			WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

			if (DEBUG)
				click_chatter("Sent packet to socket with port %d", _dport);
			output(API_PORT).push(UDPIPPrep(p2, _dport));
		}

	} else {
		if (!_dport) {
			click_chatter("Packet to unknown port %d XID=%s, sendToApp=%d", _dport, _destination_xid.unparse().c_str(), sendToApplication );
		}
	}*/
}

void XTRANSPORT::ProcessCachePacket(WritablePacket *p_in)
{
	if (DEBUG)
		click_chatter("Got packet from cache");
	//Extract the SID/CID
	XIAHeader xiah(p_in->xia_header());
	XIAPath dst_path = xiah.dst_path();
	XIAPath src_path = xiah.src_path();
	XID	destination_sid = dst_path.xid(dst_path.destination_node());
	XID	source_cid = src_path.xid(src_path.destination_node());

	XIDpair xid_pair;
	xid_pair.set_src(destination_sid);
	xid_pair.set_dst(source_cid);

	unsigned short _dport = XIDpairToPort.get(xid_pair);

	if(_dport)
	{
		//TODO: Refine the way we change DAG in case of migration. Use some control bits. Add verification
		//sock sk=portToSock.get(_dport);
		//sk.dst_path=xiah.src_path();
		//portToSock.set(_dport,sk);
		//ENDTODO

		sock *sk = portToSock.get_pointer(_dport);

		// Reset timer or just Remove the corresponding entry in the hash tables (Done below)
		HashTable<XID, WritablePacket*>::iterator it1;
		it1 = sk->XIDtoCIDreqPkt.find(source_cid);

		if(it1 != sk->XIDtoCIDreqPkt.end() ) {
			// Remove the entry
			sk->XIDtoCIDreqPkt.erase(it1);
		}

		HashTable<XID, Timestamp>::iterator it2;
		it2 = sk->XIDtoExpiryTime.find(source_cid);

		if(it2 != sk->XIDtoExpiryTime.end()) {
			// Remove the entry
			sk->XIDtoExpiryTime.erase(it2);
		}

		HashTable<XID, bool>::iterator it3;
		it3 = sk->XIDtoTimerOn.find(source_cid);

		if(it3 != sk->XIDtoTimerOn.end()) {
			// Remove the entry
			sk->XIDtoTimerOn.erase(it3);
		}

		// compute the hash and verify it matches the CID
		String hash = "CID:";
		char hexBuf[3];
		int i = 0;
		SHA1_ctx sha_ctx;
		unsigned char digest[HASH_KEYSIZE];
		SHA1_init(&sha_ctx);
		SHA1_update(&sha_ctx, (unsigned char *)xiah.payload(), xiah.plen());
		SHA1_final(digest, &sha_ctx);
		for(i = 0; i < HASH_KEYSIZE; i++) {
			sprintf(hexBuf, "%02x", digest[i]);
			hash.append(const_cast<char *>(hexBuf), 2);
		}

		int status = READY_TO_READ;
		if (hash != source_cid.unparse()) {
			click_chatter("CID with invalid hash received: %s\n", source_cid.unparse().c_str());
			status = INVALID_HASH;
		}

		// Update the status of CID request
		sk->XIDtoStatus.set(source_cid, status);

		// Check if the ReadCID() was called for this CID
		HashTable<XID, bool>::iterator it4;
		it4 = sk->XIDtoReadReq.find(source_cid);

		if(it4 != sk->XIDtoReadReq.end()) {
			// There is an entry
			bool read_cid_req = it4->second;

			if (read_cid_req == true) {
				// Send pkt up
				sk->XIDtoReadReq.erase(it4);

				portToSock.set(_dport, *sk);

				//Unparse sock info
				String src_path = xiah.src_path().unparse();

				xia::XSocketMsg xia_socket_msg;
				xia_socket_msg.set_type(xia::XREADCHUNK);
				xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg.mutable_x_readchunk();
				x_readchunk_msg->set_dag(src_path.c_str());
				x_readchunk_msg->set_payload((const char*)xiah.payload(), xiah.plen());

				std::string p_buf;
				xia_socket_msg.SerializeToString(&p_buf);

				WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

				//printf("FROM CACHE. data length = %d  \n", str.length());
				if (DEBUG)
					click_chatter("Sent packet to socket: sport %d dport %d", _dport, _dport);

				output(API_PORT).push(UDPIPPrep(p2, _dport));

			} else {
				// Store the packet into temp buffer (until ReadCID() is called for this CID)
				WritablePacket *copy_response_pkt = copy_cid_response_packet(p_in, sk);
				sk->XIDtoCIDresponsePkt.set(source_cid, copy_response_pkt);

				portToSock.set(_dport, *sk);
			}

		} else {
			WritablePacket *copy_response_pkt = copy_cid_response_packet(p_in, sk);
			sk->XIDtoCIDresponsePkt.set(source_cid, copy_response_pkt);
			portToSock.set(_dport, *sk);
		}
	}
	else
	{
		click_chatter("Case 2. Packet to unknown %s", destination_sid.unparse().c_str());
	}
}

void XTRANSPORT::ProcessXhcpPacket(WritablePacket *p_in)
{
	XIAHeader xiah(p_in->xia_header());
	String temp = _local_addr.unparse();
	Vector<String> ids;
	cp_spacevec(temp, ids);;
	if (ids.size() < 3) {
		String new_route((char *)xiah.payload());
		String new_local_addr = new_route + " " + ids[1];
		click_chatter("new address is - %s", new_local_addr.c_str());
		_local_addr.parse(new_local_addr);
	}
}

void XTRANSPORT::push(int port, Packet *p_input)
{
//	pthread_mutex_lock(&_lock);

	WritablePacket *p_in = p_input->uniqueify();
	//Depending on which CLICK-module-port it arrives at it could be control/API traffic/Data traffic

	switch(port) { // This is a "CLICK" port of UDP module.
	case API_PORT:	// control packet from socket API
		ProcessAPIPacket(p_in);
		break;

	case BAD_PORT: //packet from ???
        if (DEBUG)
            click_chatter("\n\nERROR: BAD INPUT PORT TO XTRANSPORT!!!\n\n");
		break;

	case NETWORK_PORT: //Packet from network layer
		ProcessNetworkPacket(p_in);
		p_in->kill();
		break;

	case CACHE_PORT:	//Packet from cache
		ProcessCachePacket(p_in);
		p_in->kill();
		break;

	case XHCP_PORT:		//Packet with DHCP information
		ProcessXhcpPacket(p_in);
		p_in->kill();
		break;

	default:
		click_chatter("packet from unknown port: %d\n", port);
		break;
	}

//	pthread_mutex_unlock(&_lock);
}

void XTRANSPORT::ReturnResult(int sport, xia::XSocketCallType type, int rc, int err)
{
//	click_chatter("sport=%d type=%d rc=%d err=%d\n", sport, type, rc, err);
	xia::XSocketMsg xia_socket_msg_response;
	xia_socket_msg_response.set_type(xia::XRESULT);
	xia::X_Result_Msg *x_result = xia_socket_msg_response.mutable_x_result();
	x_result->set_type(type);
	x_result->set_return_code(rc);
	x_result->set_err_code(err);

	std::string p_buf;
	xia_socket_msg_response.SerializeToString(&p_buf);
	WritablePacket *reply = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, sport));
}

Packet *
XTRANSPORT::UDPIPPrep(Packet *p_in, int dport)
{
    p_in->set_dst_ip_anno(IPAddress("127.0.0.1"));
    SET_DST_PORT_ANNO(p_in, dport);

	return p_in;
}


enum {H_MOVE};

int XTRANSPORT::write_param(const String &conf, Element *e, void *vparam,
							ErrorHandler *errh)
{
	XTRANSPORT *f = static_cast<XTRANSPORT *>(e);
	switch(reinterpret_cast<intptr_t>(vparam)) {
	case H_MOVE:
	{
		XIAPath local_addr;
		if (cp_va_kparse(conf, f, errh,
						 "LOCAL_ADDR", cpkP + cpkM, cpXIAPath, &local_addr,
						 cpEnd) < 0)
			return -1;
		f->_local_addr = local_addr;
		click_chatter("Moved to %s", local_addr.unparse().c_str());
		f->_local_hid = local_addr.xid(local_addr.destination_node());

	}
	break;
	default:
		break;
	}
	return 0;
}

void XTRANSPORT::add_handlers() {
	add_write_handler("local_addr", write_param, (void *)H_MOVE);
}

/*
** Handler for the Xsocket API call
**
** FIXME: why is xia_socket_msg part of the xtransport class and not a local variable?????
*/
void XTRANSPORT::Xsocket(unsigned short _sport) {
:
	/* =========================
	 * Initialize socket variables
	 * ========================= */

	//Set the source port in sock
	sock sk;
	sk.port = _sport;
	sk.timer_on = false;
	sk.synack_waiting = false;
	sk.dataack_waiting = false;
	sk.num_retransmit_tries = 0;
	sk.teardown_waiting = false;
	sk.isAcceptSocket = false;
	sk.num_connect_tries = 0; // number of xconnect tries (Xconnect will fail after MAX_CONNECT_TRIES trials)
	memset(sk.sent_pkt, 0, MAX_WIN_SIZE * sizeof(WritablePacket*));

	//Set the socket_type (reliable or not) in sock
	xia::X_Socket_Msg *x_socket_msg = xia_socket_msg.mutable_x_socket();
	int sock_type = x_socket_msg->type();
	sk.sock_type = sock_type;

	/* =========================================================
	 * Initialize TCP variables
	 * http://lxr.linux.no/linux+v3.10.2/net/ipv4/tcp.c : 372
	 * ========================================================= */
	if(sock_type == XSOCKET_STREAM)
	{
		//tcp_init_xmit_timers(sk);
		
		// TODO: Linux use tp for tcp_sock, sk for sock, icsk for inet_connection_sock.
		// 		 We currently only have sock. Should probably rename tp, icsk to sk later
		sock *tp = &sk;	 
		//sock *icsk = &sk;

		//icsk->icsk_rto = TCP_TIMEOUT_INIT;
		tp->mdev = TCP_TIMEOUT_INIT;

		/* So many TCP implementations out there (incorrectly) count the
		 * initial SYN frame in their delayed-ACK and congestion control
		 * algorithms that we must have the following bandaid to talk
		 * efficiently to them.  -DaveM
		 */
		tp->snd_cwnd = TCP_INIT_CWND;

		/* See draft-stevens-tcpca-spec-01 for discussion of the
		 * initialization of these values.
		 */
		tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
		tp->snd_cwnd_clamp = ~0;
		tp->mss_cache = TCP_MSS_DEFAULT;

		//tp->reordering = sysctl_tcp_reordering;
		//tcp_enable_early_retrans(tp);
		//icsk->icsk_ca_ops = &tcp_init_congestion_ops;

		tp->tsoffset = 0;

		sk.sk_state = TCP_CLOSE;

		//sk->sk_write_space = sk_stream_write_space;
		//sock_set_flag(sk, SOCK_USE_WRITE_QUEUE);

		//icsk->icsk_sync_mss = tcp_sync_mss;

		/* Presumed zeroed, in order of appearance:
		 *	cookie_in_always, cookie_out_never,
		 *	s_data_constant, s_data_in, s_data_out
		 */
		//sk->sk_sndbuf = sysctl_tcp_wmem[1];
		//sk->sk_rcvbuf = sysctl_tcp_rmem[1];

		//local_bh_disable();
		//sock_update_memcg(sk);
		//sk_sockets_allocated_inc(sk);
		//local_bh_enable();
	}

	// Map the source port to DagInfo
	portToSock.set(_sport, sk);
	portToActive.set(_sport, true);

	hlim.set(_sport, HLIM_DEFAULT);
	nxt_xport.set(_sport, CLICK_XIA_NXT_TRN);

	// printf("XSOCKET: sport=%hu\n", _sport);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	ReturnResult(_sport, xia::XSOCKET, 0);
	// output(API_PORT).push(UDPIPPrep(p_in,_sport));
}

/*
** Xsetsockopt API handler
*/
void XTRANSPORT::Xsetsockopt(unsigned short _sport) {

	// click_chatter("\nSet Socket Option\n");
	xia::X_Setsockopt_Msg *x_sso_msg = xia_socket_msg.mutable_x_setsockopt();

	switch (x_sso_msg->opt_type())
	{
		// FIXME: need real opt type for protobufs
	case 1:
	{
		int hl = x_sso_msg->int_opt();
	
		hlim.set(_sport, hl);
		//click_chatter("sso:hlim:%d\n",hl);
	}
	break;

	case 2:
	{
		int nxt = x_sso_msg->int_opt();
		nxt_xport.set(_sport, nxt);
		if (nxt == CLICK_XIA_NXT_XCMP)
			xcmp_listeners.push_back(_sport);
		else
			xcmp_listeners.remove(_sport);
	}
	break;

	default:
		// unsupported option
		break;
	}
	ReturnResult(_sport, xia::XSETSOCKOPT);
}

/*
** Xgetsockopt API handler
*/
void XTRANSPORT::Xgetsockopt(unsigned short _sport) {
	// click_chatter("\nGet Socket Option\n");
	xia::X_Getsockopt_Msg *x_sso_msg = xia_socket_msg.mutable_x_getsockopt();

	// click_chatter("opt = %d\n", x_sso_msg->opt_type());
	switch (x_sso_msg->opt_type())
	{
	// FIXME: need real opt type for protobufs
	case 1:
	{
		x_sso_msg->set_int_opt(hlim.get(_sport));
		//click_chatter("gso:hlim:%d\n", hlim.get(_sport));
	}
	break;

	case 2:
	{
		x_sso_msg->set_int_opt(nxt_xport.get(_sport));
	}
	break;

	default:
		// unsupported option
		break;
	}
	std::string p_buf;
	xia_socket_msg.SerializeToString(&p_buf);

	WritablePacket *reply = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::Xbind(unsigned short _sport) {

	int rc = 0, ec = 0;

	//Bind XID
	//click_chatter("\n\nOK: SOCKET BIND !!!\\n");
	//get source DAG from protobuf message

	xia::X_Bind_Msg *x_bind_msg = xia_socket_msg.mutable_x_bind();

	String sdag_string(x_bind_msg->sdag().c_str(), x_bind_msg->sdag().size());

	//String sdag_string((const char*)p_in->data(),(const char*)p_in->end_data());
//	if (DEBUG)
//		click_chatter("\nbind requested to %s, length=%d\n", sdag_string.c_str(), (int)p_in->length());

	//String str_local_addr=_local_addr.unparse();
	//str_local_addr=str_local_addr+" "+xid_string;//Make source DAG _local_addr:SID

	//Set the source DAG in sock
	sock *sk = portToSock.get_pointer(_sport);
	if (sk->src_path.parse(sdag_string)) {
		sk->nxt = LAST_NODE_DEFAULT;
		sk->last = LAST_NODE_DEFAULT;
		sk->hlim = hlim.get(_sport);
		sk->isConnected = false;
		sk->initialized = true;
		sk->sdag = sdag_string;

		//Check if binding to full DAG or just to SID only
		Vector<XIAPath::handle_t> xids = sk->src_path.next_nodes( sk->src_path.source_node() );		
		XID front_xid = sk->src_path.xid( xids[0] );
		struct click_xia_xid head_xid = front_xid.xid();
		uint32_t head_xid_type = head_xid.type;
		if(head_xid_type == _sid_type) {
			sk->full_src_dag = false; 
		} else {
			sk->full_src_dag = true;
		}

		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
		//XID xid(xid_string);
		//TODO: Add a check to see if XID is already being used

		// Map the source XID to source port (for now, for either type of tranports)
		XIDtoPort.set(source_xid, _sport);
		addRoute(source_xid);

		portToSock.set(_sport, *sk);

		//click_chatter("Bound");
		//click_chatter("set %d %d",_sport, __LINE__);

	} else {
		rc = -1;
		ec = EADDRNOTAVAIL;
	}

	// (for Ack purpose) Reply with a packet with the destination port=source port
	ReturnResult(_sport, xia::XBIND, rc, ec);
}

void XTRANSPORT::Xclose(unsigned short _sport)
{
	// Close port
	//click_chatter("Xclose: closing %d\n", _sport);

	sock *sk = portToSock.get_pointer(_sport);

	// Set timer
	sk->timer_on = true;
	sk->teardown_waiting = true;
	sk->teardown_expiry = Timestamp::now() + Timestamp::make_msec(_teardown_wait_ms);

	if (! _timer.scheduled() || _timer.expiry() >= sk->teardown_expiry )
		_timer.reschedule_at(sk->teardown_expiry);

	portToSock.set(_sport, *sk);

	xcmp_listeners.remove(_sport);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	ReturnResult(_sport, xia::XCLOSE);
}

void XTRANSPORT::Xconnect(unsigned short _sport)
{
	//click_chatter("Xconect: connecting %d\n", _sport);

	//isConnected=true;
	//String dest((const char*)p_in->data(),(const char*)p_in->end_data());
	//click_chatter("\nconnect to %s, length=%d\n",dest.c_str(),(int)p_in->length());

	xia::X_Connect_Msg *x_connect_msg = xia_socket_msg.mutable_x_connect();

	String dest(x_connect_msg->ddag().c_str());

	//String sdag_string((const char*)p_in->data(),(const char*)p_in->end_data());
	//click_chatter("\nconnect requested to %s, length=%d\n",dest.c_str(),(int)p_in->length());

	XIAPath dst_path;
	dst_path.parse(dest);

	sock *sk = portToSock.get_pointer(_sport);
	//click_chatter("connect %d %x",_sport, sk);

	if(!sk) {
		//click_chatter("Create DAGINFO connect %d %x",_sport, sk);
		//No local SID bound yet, so bind ephemeral one
		sk = new sock();
	}

	sk->dst_path = dst_path;
	sk->port = _sport;
	sk->isConnected = true;
	sk->initialized = true;
	sk->ddag = dest;
	sk->seq_num = 0;
	sk->ack_num = 0;
	sk->send_base = 0;
	sk->next_send_seqnum = 0;
	sk->next_recv_seqnum = 0;
	sk->num_connect_tries++; // number of xconnect tries (Xconnect will fail after MAX_CONNECT_TRIES trials)

	String str_local_addr = _local_addr.unparse_re();
	//String dagstr = sk->src_path.unparse_re();

	/* Use src_path set by Xbind() if exists */
	if(sk->sdag.length() == 0) {
		char xid_string[50];
		random_xid("SID", xid_string);

//		String rand(click_random(1000000, 9999999));
//		String xid_string = "SID:20000ff00000000000000000000000000" + rand;
//		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		sk->src_path.parse_re(str_local_addr);
	}

	sk->nxt = LAST_NODE_DEFAULT;
	sk->last = LAST_NODE_DEFAULT;
	sk->hlim = hlim.get(_sport);

	XID source_xid = sk->src_path.xid(sk->src_path.destination_node());
	XID destination_xid = sk->dst_path.xid(sk->dst_path.destination_node());

	XIDpair xid_pair;
	xid_pair.set_src(source_xid);
	xid_pair.set_dst(destination_xid);

	// Map the src & dst XID pair to source port
	//printf("setting pair to port1 %d\n", _sport);

	XIDpairToPort.set(xid_pair, _sport);

	// Map the source XID to source port
	XIDtoPort.set(source_xid, _sport);
	addRoute(source_xid);

	// click_chatter("XCONNECT: set %d %x",_sport, sk);

	// Prepare SYN packet

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_nxt(CLICK_XIA_NXT_TRN);
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(sk->src_path);

	//click_chatter("Sent packet to network");
	const char* dummy = "Connection_request";
	WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 20);

	WritablePacket *p = NULL;

	TransportHeaderEncap *thdr = TransportHeaderEncap::MakeSYNHeader( 0, -1, 0); // #seq, #ack, length

	p = thdr->encap(just_payload_part);

	thdr->update();
	xiah.set_plen(strlen(dummy) + thdr->hlen()); // XIA payload = transport header + transport-layer data

	p = xiah.encap(p, false);

	delete thdr;

	// Set timer
	sk->timer_on = true;
	sk->synack_waiting = true;
	sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

	if (! _timer.scheduled() || _timer.expiry() >= sk->expiry )
		_timer.reschedule_at(sk->expiry);

	// Store the syn packet for potential retransmission
	sk->syn_pkt = copy_packet(p, sk);

	portToSock.set(_sport, *sk);
	XIAHeader xiah1(p);
	//String pld((char *)xiah1.payload(), xiah1.plen());
	// printf("XCONNECT: %d: %s\n", _sport, (_local_addr.unparse()).c_str());
	output(NETWORK_PORT).push(p);

	//sk=portToSock.get_pointer(_sport);
	//click_chatter("\nbound to %s\n",portToSock.get_pointer(_sport)->src_path.unparse().c_str());

	// (for Ack purpose) Reply with a packet with the destination port=source port
	//output(API_PORT).push(UDPIPPrep(p_in,_sport));
}

void XTRANSPORT::Xaccept(unsigned short _sport)
{
	//click_chatter("Xaccept: on %d\n", _sport);
	hlim.set(_sport, HLIM_DEFAULT);
	nxt_xport.set(_sport, CLICK_XIA_NXT_TRN);

	if (!pending_connection_buf.empty()) {

		sock sk = pending_connection_buf.front();
		sk.port = _sport;

		sk.seq_num = 0;
		sk.ack_num = 0;
		sk.send_base = 0;
		sk.hlim = hlim.get(_sport);
		sk.next_send_seqnum = 0;
		sk.next_recv_seqnum = 0;
		sk.isAcceptSocket = true;
		memset(sk.send_buffer, 0, sk.send_buffer_size * sizeof(WritablePacket*));

		portToSock.set(_sport, sk);

		XID source_xid = sk.src_path.xid(sk.src_path.destination_node());
		XID destination_xid = sk.dst_path.xid(sk.dst_path.destination_node());

		XIDpair xid_pair;
		xid_pair.set_src(source_xid);
		xid_pair.set_dst(destination_xid);

		// Map the src & dst XID pair to source port
		XIDpairToPort.set(xid_pair, _sport);

		portToActive.set(_sport, true);

		// printf("XACCEPT: (%s) my_sport=%d  my_sid=%s  his_sid=%s \n\n", (_local_addr.unparse()).c_str(), _sport, source_xid.unparse().c_str(), destination_xid.unparse().c_str());

		pending_connection_buf.pop();

		ReturnResult(_sport, xia::XACCEPT);

	} else {
		// FIXME: what error code should be returned?
		ReturnResult(_sport, xia::XACCEPT, -1, ECONNABORTED);
		click_chatter("\n Xaccept: error\n");
	}
}

void XTRANSPORT::Xchangead(unsigned short _sport)
{
	UNUSED(_sport);

	xia::X_Changead_Msg *x_changead_msg = xia_socket_msg.mutable_x_changead();
	//String tmp = _local_addr.unparse();
	//Vector<String> ids;
	//cp_spacevec(tmp, ids);
	String AD_str(x_changead_msg->ad().c_str());
	String HID_str = _local_hid.unparse();
	String IP4ID_str(x_changead_msg->ip4id().c_str());
	_local_4id.parse(IP4ID_str);
	String new_local_addr;
	// If a valid 4ID is given, it is included (as a fallback) in the local_addr
	if(_local_4id != _null_4id) {		
		new_local_addr = "RE ( " + IP4ID_str + " ) " + AD_str + " " + HID_str;
	} else {
		new_local_addr = "RE " + AD_str + " " + HID_str;
	}
	click_chatter("new address is - %s", new_local_addr.c_str());
	_local_addr.parse(new_local_addr);		
}

void XTRANSPORT::Xreadlocalhostaddr(unsigned short _sport)
{
	// read the localhost AD and HID
	String local_addr = _local_addr.unparse();
	size_t AD_found_start = local_addr.find_left("AD:");
	size_t AD_found_end = local_addr.find_left(" ", AD_found_start);
	String AD_str = local_addr.substring(AD_found_start, AD_found_end - AD_found_start);
	String HID_str = _local_hid.unparse();
	String IP4ID_str = _local_4id.unparse();
	// return a packet containing localhost AD and HID
	xia::XSocketMsg _Response;
	_Response.set_type(xia::XREADLOCALHOSTADDR);
	xia::X_ReadLocalHostAddr_Msg *_msg = _Response.mutable_x_readlocalhostaddr();
	_msg->set_ad(AD_str.c_str());
	_msg->set_hid(HID_str.c_str());
	_msg->set_ip4id(IP4ID_str.c_str());
	std::string p_buf1;
	_Response.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::Xupdatenameserverdag(unsigned short _sport)
{
	UNUSED(_sport);

	xia::X_Updatenameserverdag_Msg *x_updatenameserverdag_msg = xia_socket_msg.mutable_x_updatenameserverdag();
	String ns_dag(x_updatenameserverdag_msg->dag().c_str());
	//click_chatter("new nameserver address is - %s", ns_dag.c_str());
	_nameserver_addr.parse(ns_dag);
}

void XTRANSPORT::Xreadnameserverdag(unsigned short _sport)
{
	// read the nameserver DAG
	String ns_addr = _nameserver_addr.unparse();
	// return a packet containing the nameserver DAG
	xia::XSocketMsg _Response;
	_Response.set_type(xia::XREADNAMESERVERDAG);
	xia::X_ReadNameServerDag_Msg *_msg = _Response.mutable_x_readnameserverdag();
	_msg->set_dag(ns_addr.c_str());
	std::string p_buf1;
	_Response.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::Xisdualstackrouter(unsigned short _sport)
{
	// return a packet indicating whether this node is an XIA-IPv4 dual-stack router
	xia::XSocketMsg _Response;
	_Response.set_type(xia::XISDUALSTACKROUTER);
	xia::X_IsDualStackRouter_Msg *_msg = _Response.mutable_x_isdualstackrouter();
	_msg->set_flag(_is_dual_stack_router);
	std::string p_buf1;
	_Response.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::Xgetpeername(unsigned short _sport)
{
	xia::XSocketMsg _xsm;
	_xsm.set_type(xia::XGETPEERNAME);
	xia::X_GetPeername_Msg *_msg = _xsm.mutable_x_getpeername();

	sock *sk = portToSock.get_pointer(_sport);

	_msg->set_dag(sk->dst_path.unparse().c_str());

	std::string p_buf1;
	_xsm.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}
					

void XTRANSPORT::Xgetsockname(unsigned short _sport)
{
	xia::XSocketMsg _xsm;
	_xsm.set_type(xia::XGETSOCKNAME);
	xia::X_GetSockname_Msg *_msg = _xsm.mutable_x_getsockname();

	sock *sk = portToSock.get_pointer(_sport);

	_msg->set_dag(sk->src_path.unparse().c_str());

	std::string p_buf1;
	_xsm.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}


void XTRANSPORT::Xsend(unsigned short _sport, WritablePacket *p_in)
{
	//click_chatter("Xsend on %d\n", _sport);

	xia::X_Send_Msg *x_send_msg = xia_socket_msg.mutable_x_send();

	String pktPayload(x_send_msg->payload().c_str(), x_send_msg->payload().size());

	int pktPayloadSize = pktPayload.length();
	//click_chatter("pkt %s port %d", pktPayload.c_str(), _sport);
	//printf("XSEND: %d bytes from (%d)\n", pktPayloadSize, _sport);

	//Find sock info for that stream
	sock *sk = portToSock.get_pointer(_sport);
	if (sk && sk->isConnected) {

		//Recalculate source path
		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
		String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse();
		//Make source DAG _local_addr:SID
		String dagstr = sk->src_path.unparse_re();

		//Client Mobility...
		if (dagstr.length() != 0 && dagstr != str_local_addr) {
			//Moved!
			// 1. Update 'sk->src_path'
			sk->src_path.parse_re(str_local_addr);
		}
	
		// Case of initial binding to only SID
		if(sk->full_src_dag == false) {
			sk->full_src_dag = true;
			String str_local_addr = _local_addr.unparse_re();
			XID front_xid = sk->src_path.xid(sk->src_path.destination_node());
			String xid_string = front_xid.unparse();
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
			sk->src_path.parse_re(str_local_addr);
		}

		//Add XIA headers
		XIAHeaderEncap xiah;
		xiah.set_nxt(CLICK_XIA_NXT_TRN);
		xiah.set_last(LAST_NODE_DEFAULT);
		xiah.set_hlim(hlim.get(_sport));
		xiah.set_dst_path(sk->dst_path);
		xiah.set_src_path(sk->src_path);
		xiah.set_plen(pktPayloadSize);

		if (DEBUG)
			click_chatter("XSEND: (%d) sent packet to %s, from %s\n", _sport, sk->dst_path.unparse_re().c_str(), sk->src_path.unparse_re().c_str());

		WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_send_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

		WritablePacket *p = NULL;

		//Add XIA Transport headers
		TransportHeaderEncap *thdr = TransportHeaderEncap::MakeDATAHeader(sk->next_send_seqnum, sk->ack_num, 0 ); // #seq, #ack, length
		p = thdr->encap(just_payload_part);

		thdr->update();
		xiah.set_plen(pktPayloadSize + thdr->hlen()); // XIA payload = transport header + transport-layer data

		p = xiah.encap(p, false);

		delete thdr;

		// Store the packet into buffer
		WritablePacket *tmp = sk->send_buffer[sk->seq_num % sk->send_buffer_size];
		sk->send_buffer[sk->seq_num % sk->send_buffer_size] = copy_packet(p, sk);
		if (tmp)
			tmp->kill();

		// printf("XSEND: SENT DATA at (%s) seq=%d \n\n", dagstr.c_str(), sk->seq_num%sk->send_buffer_size);

		sk->seq_num++;
		sk->next_send_seqnum++;

		// Set timer
		sk->timer_on = true;
		sk->dataack_waiting = true;
		sk->num_retransmit_tries = 0;
		sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

		if (! _timer.scheduled() || _timer.expiry() >= sk->expiry )
			_timer.reschedule_at(sk->expiry);

		portToSock.set(_sport, *sk);

		//click_chatter("Sent packet to network");
		XIAHeader xiah1(p);
		String pld((char *)xiah1.payload(), xiah1.plen());
		//printf("\n\n (%s) send (timer set at %f) =%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (sk->expiry).doubleval(), pld.c_str(), xiah1.plen());
		output(NETWORK_PORT).push(p);

		// REMOVED STATUS RETURNS AS WE RAN INTO SEQUENCING ERRORS
		// WHERE IT INTERLEAVED WITH RECEIVE PACKETS
		// (for Ack purpose) Reply with a packet with the destination port=source port
//				ReturnResult(_sport, xia::XSEND);

	} else {
	
//				ReturnResult(_sport, xia::XSEND, -1, ENOTCONN);
		click_chatter("Not 'connect'ed: you may need to use 'sendto()'");
	}
}

void XTRANSPORT::Xsendto(unsigned short _sport, WritablePacket *p_in)
{
	xia::X_Sendto_Msg *x_sendto_msg = xia_socket_msg.mutable_x_sendto();

	String dest(x_sendto_msg->ddag().c_str());
	String pktPayload(x_sendto_msg->payload().c_str(), x_sendto_msg->payload().size());
	int pktPayloadSize = pktPayload.length();
	//click_chatter("\n SENDTO ddag:%s, payload:%s, length=%d\n",xia_socket_msg.ddag().c_str(), xia_socket_msg.payload().c_str(), pktPayloadSize);

	XIAPath dst_path;
	dst_path.parse(dest);

	//Find sock info for this DGRAM
	sock *sk = portToSock.get_pointer(_sport);

	if(!sk) {
		//No local SID bound yet, so bind one
		sk = new sock();
	}

	if (sk->initialized == false) {
		sk->initialized = true;
		sk->full_src_dag = true;
		sk->port = _sport;
		String str_local_addr = _local_addr.unparse_re();

		char xid_string[50];
		random_xid("SID", xid_string);
//		String rand(click_random(1000000, 9999999));
//		String xid_string = "SID:20000ff00000000000000000000000000" + rand;
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID

		sk->src_path.parse_re(str_local_addr);

		sk->last = LAST_NODE_DEFAULT;
		sk->hlim = hlim.get(_sport);

		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());

		XIDtoPort.set(source_xid, _sport); //Maybe change the mapping to XID->sock?
		addRoute(source_xid);
	}

	// Case of initial binding to only SID
	if(sk->full_src_dag == false) {
		sk->full_src_dag = true;
		String str_local_addr = _local_addr.unparse_re();
		XID front_xid = sk->src_path.xid(sk->src_path.destination_node());
		String xid_string = front_xid.unparse();
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		sk->src_path.parse_re(str_local_addr);
	}


	if(sk->src_path.unparse_re().length() != 0) {
		//Recalculate source path
		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
		String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse(); //Make source DAG _local_addr:SID
		sk->src_path.parse(str_local_addr);
	}

	portToSock.set(_sport, *sk);

	sk = portToSock.get_pointer(_sport);

//			if (DEBUG)
//				click_chatter("sent packet from %s, to %s\n", sk->src_path.unparse_re().c_str(), dest.c_str());

	//Add XIA headers
	XIAHeaderEncap xiah;

	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(sk->src_path);

	WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_sendto_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

	WritablePacket *p = NULL;

	if (sk->sock_type == XSOCKET_RAW) {
		xiah.set_nxt(nxt_xport.get(_sport));

		xiah.set_plen(pktPayloadSize);
		p = xiah.encap(just_payload_part, false);

	} else {
		xiah.set_nxt(CLICK_XIA_NXT_TRN);
		xiah.set_plen(pktPayloadSize);

		//p = xiah.encap(just_payload_part, true);
		//printf("\n\nSEND: %s ---> %s\n\n", sk->src_path.unparse_re().c_str(), dest.c_str());
		//printf("payload=%s len=%d \n\n", x_sendto_msg->payload().c_str(), pktPayloadSize);

		//Add XIA Transport headers
		TransportHeaderEncap *thdr = TransportHeaderEncap::MakeDGRAMHeader(0); // length
		p = thdr->encap(just_payload_part);

		thdr->update();
		xiah.set_plen(pktPayloadSize + thdr->hlen()); // XIA payload = transport header + transport-layer data

		p = xiah.encap(p, false);
		delete thdr;
	}

	output(NETWORK_PORT).push(p);

	// removed due to multi peer collision problem
	// (for Ack purpose) Reply with a packet with the destination port=source port
//				ReturnResult(_sport, xia::XSENDTO);
}

void XTRANSPORT::XrequestChunk(unsigned short _sport, WritablePacket *p_in)
{
	xia::X_Requestchunk_Msg *x_requestchunk_msg = xia_socket_msg.mutable_x_requestchunk();

	String pktPayload(x_requestchunk_msg->payload().c_str(), x_requestchunk_msg->payload().size());
	int pktPayloadSize = pktPayload.length();

	// send CID-Requests

	for (int i = 0; i < x_requestchunk_msg->dag_size(); i++) {
		String dest = x_requestchunk_msg->dag(i).c_str();
		//printf("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
		//printf("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
		XIAPath dst_path;
		dst_path.parse(dest);

		//Find sock info for this DGRAM
		sock *sk = portToSock.get_pointer(_sport);

		if(!sk) {
			//No local SID bound yet, so bind one
			sk = new sock();
		}

		if (sk->initialized == false) {
			sk->initialized = true;
			sk->full_src_dag = true;
			sk->port = _sport;
			String str_local_addr = _local_addr.unparse_re();

			char xid_string[50];
			random_xid("SID", xid_string);
//			String rand(click_random(1000000, 9999999));
//			String xid_string = "SID:20000ff00000000000000000000000000" + rand;
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID

			sk->src_path.parse_re(str_local_addr);

			sk->last = LAST_NODE_DEFAULT;
			sk->hlim = hlim.get(_sport);

			XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());

			XIDtoPort.set(source_xid, _sport); //Maybe change the mapping to XID->sock?
			addRoute(source_xid);

		}
	
		// Case of initial binding to only SID
		if(sk->full_src_dag == false) {
			sk->full_src_dag = true;
			String str_local_addr = _local_addr.unparse_re();
			XID front_xid = sk->src_path.xid(sk->src_path.destination_node());
			String xid_string = front_xid.unparse();
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
			sk->src_path.parse_re(str_local_addr);
		}
	
		if(sk->src_path.unparse_re().length() != 0) {
			//Recalculate source path
			XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
			String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse(); //Make source DAG _local_addr:SID
			sk->src_path.parse(str_local_addr);
		}

		portToSock.set(_sport, *sk);

		sk = portToSock.get_pointer(_sport);

		if (DEBUG)
			click_chatter("sent packet to %s, from %s\n", dest.c_str(), sk->src_path.unparse_re().c_str());

		//Add XIA headers
		XIAHeaderEncap xiah;
		xiah.set_nxt(CLICK_XIA_NXT_CID);
		xiah.set_last(LAST_NODE_DEFAULT);
		xiah.set_hlim(hlim.get(_sport));
		xiah.set_dst_path(dst_path);
		xiah.set_src_path(sk->src_path);
		xiah.set_plen(pktPayloadSize);

		WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_requestchunk_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

		WritablePacket *p = NULL;

		//Add Content header
		ContentHeaderEncap *chdr = ContentHeaderEncap::MakeRequestHeader();
		p = chdr->encap(just_payload_part);
		p = xiah.encap(p, true);
		delete chdr;

		XID	source_sid = sk->src_path.xid(sk->src_path.destination_node());
		XID	destination_cid = dst_path.xid(dst_path.destination_node());

		XIDpair xid_pair;
		xid_pair.set_src(source_sid);
		xid_pair.set_dst(destination_cid);

		// Map the src & dst XID pair to source port
		XIDpairToPort.set(xid_pair, _sport);

		// Store the packet into buffer
		WritablePacket *copy_req_pkt = copy_cid_req_packet(p, sk);
		sk->XIDtoCIDreqPkt.set(destination_cid, copy_req_pkt);

		// Set the status of CID request
		sk->XIDtoStatus.set(destination_cid, WAITING_FOR_CHUNK);

		// Set the status of ReadCID reqeust
		sk->XIDtoReadReq.set(destination_cid, false);

		// Set timer
		Timestamp cid_req_expiry  = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
		sk->XIDtoExpiryTime.set(destination_cid, cid_req_expiry);
		sk->XIDtoTimerOn.set(destination_cid, true);

		if (! _timer.scheduled() || _timer.expiry() >= cid_req_expiry )
			_timer.reschedule_at(cid_req_expiry);

		portToSock.set(_sport, *sk);

		output(NETWORK_PORT).push(p);
	}
}

void XTRANSPORT::XgetChunkStatus(unsigned short _sport)
{
	xia::X_Getchunkstatus_Msg *x_getchunkstatus_msg = xia_socket_msg.mutable_x_getchunkstatus();

	int numCids = x_getchunkstatus_msg->dag_size();
	String pktPayload(x_getchunkstatus_msg->payload().c_str(), x_getchunkstatus_msg->payload().size());

	// send CID-Requests
	for (int i = 0; i < numCids; i++) {
		String dest = x_getchunkstatus_msg->dag(i).c_str();
		//printf("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
		//printf("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
		XIAPath dst_path;
		dst_path.parse(dest);

		//Find sock info for this DGRAM
		sock *sk = portToSock.get_pointer(_sport);

		XID	destination_cid = dst_path.xid(dst_path.destination_node());

		// Check the status of CID request
		HashTable<XID, int>::iterator it;
		it = sk->XIDtoStatus.find(destination_cid);

		if(it != sk->XIDtoStatus.end()) {
			// There is an entry
			int status = it->second;

			if(status == WAITING_FOR_CHUNK) {
				x_getchunkstatus_msg->add_status("WAITING");

			} else if(status == READY_TO_READ) {
				x_getchunkstatus_msg->add_status("READY");

			} else if(status == INVALID_HASH) {
				x_getchunkstatus_msg->add_status("INVALID_HASH");

			} else if(status == REQUEST_FAILED) {
				x_getchunkstatus_msg->add_status("FAILED");
			}

		} else {
			// Status query for the CID that was not requested...
			x_getchunkstatus_msg->add_status("FAILED");
		}
	}

	// Send back the report

	const char *buf = "CID request status response";
	x_getchunkstatus_msg->set_payload((const char*)buf, strlen(buf) + 1);

	std::string p_buf;
	xia_socket_msg.SerializeToString(&p_buf);

	WritablePacket *reply = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::XreadChunk(unsigned short _sport)
{
	xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg.mutable_x_readchunk();

	String dest = x_readchunk_msg->dag().c_str();
	WritablePacket *copy;
	//printf("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
	//printf("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
	XIAPath dst_path;
	dst_path.parse(dest);

	//Find sock info for this DGRAM
	sock *sk = portToSock.get_pointer(_sport);

	XID	destination_cid = dst_path.xid(dst_path.destination_node());

	// Update the status of ReadCID reqeust
	sk->XIDtoReadReq.set(destination_cid, true);
	portToSock.set(_sport, *sk);

	// Check the status of CID request
	HashTable<XID, int>::iterator it;
	it = sk->XIDtoStatus.find(destination_cid);

	if(it != sk->XIDtoStatus.end()) {
		// There is an entry
		int status = it->second;

		if (status != READY_TO_READ  &&
			status != INVALID_HASH) {
			// Do nothing

		} else {
			// Send the buffered pkt to upper layer

			sk->XIDtoReadReq.set(destination_cid, false);
			portToSock.set(_sport, *sk);

			HashTable<XID, WritablePacket*>::iterator it2;
			it2 = sk->XIDtoCIDresponsePkt.find(destination_cid);
			copy = copy_cid_response_packet(it2->second, sk);

			XIAHeader xiah(copy->xia_header());

			//Unparse sock info
			String src_path = xiah.src_path().unparse();

			xia::XSocketMsg xia_socket_msg;
			xia_socket_msg.set_type(xia::XREADCHUNK);
			xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg.mutable_x_readchunk();
			x_readchunk_msg->set_dag(src_path.c_str());
			x_readchunk_msg->set_payload((const char *)xiah.payload(), xiah.plen());

			std::string p_buf;
			xia_socket_msg.SerializeToString(&p_buf);

			WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

			//printf("FROM CACHE. data length = %d  \n", str.length());
			if (DEBUG)
				click_chatter("Sent packet to socket: sport %d dport %d", _sport, _sport);

			output(API_PORT).push(UDPIPPrep(p2, _sport));

			it2->second->kill();
			sk->XIDtoCIDresponsePkt.erase(it2);

			portToSock.set(_sport, *sk);
		}
	}

}

void XTRANSPORT::XremoveChunk(unsigned short _sport)
{
	xia::X_Removechunk_Msg *x_rmchunk_msg = xia_socket_msg.mutable_x_removechunk();

	int32_t contextID = x_rmchunk_msg->contextid();
	String src(x_rmchunk_msg->cid().c_str(), x_rmchunk_msg->cid().size());
	//append local address before CID
	String str_local_addr = _local_addr.unparse_re();
	str_local_addr = "RE " + str_local_addr + " CID:" + src;
	XIAPath src_path;
	src_path.parse(str_local_addr);

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(HLIM_DEFAULT);
	xiah.set_dst_path(_local_addr);
	xiah.set_src_path(src_path);
	xiah.set_nxt(CLICK_XIA_NXT_CID);

	WritablePacket *just_payload_part = WritablePacket::make(256, (const void*)NULL, 0, 0);

	WritablePacket *p = NULL;
	ContentHeaderEncap  contenth(0, 0, 0, 0, ContentHeader::OP_LOCAL_REMOVECID, contextID);
	p = contenth.encap(just_payload_part);
	p = xiah.encap(p, true);

	if (DEBUG) {
		click_chatter("sent remove cid packet to cache");
	}
	output(CACHE_PORT).push(p);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	xia::XSocketMsg _socketResponse;
	_socketResponse.set_type(xia::XREMOVECHUNK);
	xia::X_Removechunk_Msg *_msg = _socketResponse.mutable_x_removechunk();
	_msg->set_contextid(contextID);
	_msg->set_cid(src.c_str());
	_msg->set_status(0);

	std::string p_buf1;
	_socketResponse.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::XputChunk(unsigned short _sport)
{
	xia::X_Putchunk_Msg *x_putchunk_msg = xia_socket_msg.mutable_x_putchunk();
//			int hasCID = x_putchunk_msg->hascid();
	int32_t contextID = x_putchunk_msg->contextid();
	int32_t ttl = x_putchunk_msg->ttl();
	int32_t cacheSize = x_putchunk_msg->cachesize();
	int32_t cachePolicy = x_putchunk_msg->cachepolicy();

	String pktPayload(x_putchunk_msg->payload().c_str(), x_putchunk_msg->payload().size());
	String src;

	/* Computes SHA1 Hash if user does not supply it */
	char hexBuf[3];
	int i = 0;
	SHA1_ctx sha_ctx;
	unsigned char digest[HASH_KEYSIZE];
	SHA1_init(&sha_ctx);
	SHA1_update(&sha_ctx, (unsigned char *)pktPayload.c_str() , pktPayload.length() );
	SHA1_final(digest, &sha_ctx);
	for(i = 0; i < HASH_KEYSIZE; i++) {
		sprintf(hexBuf, "%02x", digest[i]);
		src.append(const_cast<char *>(hexBuf), 2);
	}

	if(DEBUG) {
		click_chatter("ctxID=%d, length=%d, ttl=%d cid=%s\n",
					  contextID, x_putchunk_msg->payload().size(), ttl, src.c_str());
	}

	//append local address before CID
	String str_local_addr = _local_addr.unparse_re();
	str_local_addr = "RE " + str_local_addr + " CID:" + src;
	XIAPath src_path;
	src_path.parse(str_local_addr);

	if(DEBUG) {
		click_chatter("DAG: %s\n", str_local_addr.c_str());
	}

	/*TODO: The destination dag of the incoming packet is local_addr:XID
	 * Thus the cache thinks it is destined for local_addr and delivers to socket
	 * This must be ignored. Options
	 * 1. Use an invalid SID
	 * 2. The cache should only store the CID responses and not forward them to
	 *	local_addr when the source and the destination HIDs are the same.
	 * 3. Use the socket SID on which putCID was issued. This will
	 *	result in a reply going to the same socket on which the putCID was issued.
	 *	Use the response to return 1 to the putCID call to indicate success.
	 *	Need to add sk/ephemeral SID generation for this to work.
	 * 4. Special OPCODE in content extension header and treat it specially in content module (done below)
	 */

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(_local_addr);
	xiah.set_src_path(src_path);
	xiah.set_nxt(CLICK_XIA_NXT_CID);

	//Might need to remove more if another header is required (eg some control/sock info)

	WritablePacket *just_payload_part = WritablePacket::make(256, (const void*)pktPayload.c_str(), pktPayload.length(), 0);

	WritablePacket *p = NULL;
	int chunkSize = pktPayload.length();
	ContentHeaderEncap  contenth(0, 0, pktPayload.length(), chunkSize, ContentHeader::OP_LOCAL_PUTCID,
								 contextID, ttl, cacheSize, cachePolicy);
	p = contenth.encap(just_payload_part);
	p = xiah.encap(p, true);

	if (DEBUG)
		click_chatter("sent packet to cache");
	output(CACHE_PORT).push(p);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	struct timeval timestamp;
	gettimeofday(&timestamp, NULL);
	xia::XSocketMsg _socketResponse;
	_socketResponse.set_type(xia::XPUTCHUNK);
	xia::X_Putchunk_Msg *_msg = _socketResponse.mutable_x_putchunk();
	_msg->set_contextid(contextID);
	_msg->set_cid(src.c_str());
	_msg->set_ttl(ttl);
	_msg->set_timestamp(timestamp.tv_sec);
//	_msg->set_hascid(1);
	_msg->set_cachepolicy(0);
	_msg->set_cachesize(0);
	_msg->set_payload(x_putchunk_msg->payload().c_str(), x_putchunk_msg->payload().size());

	std::string p_buf1;
	_socketResponse.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

CLICK_ENDDECLS

EXPORT_ELEMENT(XTRANSPORT)
ELEMENT_REQUIRES(userlevel)
ELEMENT_REQUIRES(XIAContentModule)
ELEMENT_MT_SAFE(XTRANSPORT)
