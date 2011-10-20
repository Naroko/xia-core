/* ts=4 */
#include "Xsocket.h"
#include "Xinit.h"

int Xclose(int sockfd)
{
   	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;

	//char buf[MAXBUFLEN];
	//struct sockaddr_in their_addr;
	//socklen_t addr_len;
	
    //Send a control packet to inform Click of socket closing
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	if ((rv = getaddrinfo(CLICKCONTROLADDRESS, CLICKCONTROLPORT, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return -1;
	}

    p=servinfo;


        // protobuf message
        xia::XSocketMsg xia_socket_msg;

        xia_socket_msg.set_type(xia::XCLOSE);

        xia::X_Close_Msg *x_close_msg = xia_socket_msg.mutable_x_close();

	const char *message="close socket"; // well... not necessary though.. 
	x_close_msg->set_payload(message);

	std::string p_buf;
	xia_socket_msg.SerializeToString(&p_buf);


	if ((numbytes = sendto(sockfd, p_buf.c_str(), p_buf.size(), 0, p->ai_addr, p->ai_addrlen)) == -1) {
		perror("Xclose(): sendto failed");
		return(-1);
	}
	freeaddrinfo(servinfo);

/*
        //Process the reply
        addr_len = sizeof their_addr;
        if ((numbytes = recvfrom(sockfd, buf, MAXBUFLEN-1 , 0,
                                        (struct sockaddr *)&their_addr, &addr_len)) == -1) {
                        perror("Xclose(): recvfrom");
                        return -1;
        }

	//protobuf message parsing
	xia_socket_msg.ParseFromString(buf);
	if (xia_socket_msg.type() == xia::XSOCKET_CLOSE) {
 		return close(sockfd);;
	}

        return -1;
 */

	return numbytes;
}
