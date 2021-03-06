/*-
 * Copyright (c) 2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Pål Hermunn Johansen <hermunn@varnish-software.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
/*
 * Very simple utility for parsing a proxy protocol header (version 1 or 2)
 * and printing the contents to standard out.
 *
 * The program simply does a single read, and according to the spec
 * (http://www.haproxy.org/download/1.5/doc/proxy-protocol.txt), this is the
 * correct thing to do.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <openssl/opensslv.h>

#include "proxyv2.h"

ssize_t
read_from_socket(const char *port, unsigned char *buf, int len)
{
	struct addrinfo hints;
	struct addrinfo *result;
	struct addrinfo *rp;
	int listen_socket = -1;
	int yes = 1;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_protocol = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;

	int s = getaddrinfo(NULL, port, &hints, &result);
	if (s != 0) {
		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(s));
		exit(1);
	}

	// getaddrinfo just returned a list of address structures. Try
	// each address until we successfully bind(2).  If socket(2) (or
	// bind(2)) fails, we (close the socket and) try the next address.

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		listen_socket = socket(rp->ai_family, rp->ai_socktype,
		    rp->ai_protocol);
		if (listen_socket == -1)
			continue;
		if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
			&yes, sizeof(yes)) == -1) {
			perror("setsockopt");
			continue;
		}
		if (bind(listen_socket, rp->ai_addr, rp->ai_addrlen) == 0)
			break; // success!
		close(listen_socket);
		listen_socket = -1;
	}

	freeaddrinfo(result);

	if (rp == NULL) {
		printf("ERROR: Could not create and bind listen socket.\n");
		exit(1);
	}

	if (0 != listen(listen_socket, 1)) {
		perror("Call to listen() failed");
		exit(1);
	}
	fprintf(stderr, "Listening on port %s\n", port);
	int sock = accept(listen_socket, NULL, NULL);
	if (sock < 0) {
		close(listen_socket);
		perror("Calling accept failed");
		exit(1);
	}
	ssize_t n = recv(sock, buf, len, 0);
	fprintf(stderr, "Read %zd bytes in recv\n", n);
	close(sock);
	close(listen_socket);
	return (n);
}

void
print_addr_with_ports(int af, int len, unsigned char *p)
{
	char buf1[256], buf2[256];
	const char *addr1 = inet_ntop(af, p, buf1, 256);
	const char *addr2 = inet_ntop(af, p + len, buf2, 256);
	int src_port, dest_port;

	if (addr1 == NULL || addr2 == NULL) {
		printf("ERROR:\tIP addresses printing failed.\n");
		exit(1);
	}
	src_port = (p[2 * len] << 8) + p[2 * len + 1];
	dest_port = (p[2 * len + 2] << 8) + p[2 * len + 3];

	printf("Source IP:\t%s\n", addr1);
	printf("Destination IP:\t%s\n", addr2);
	printf("Source port:\t%d\n", src_port);
	printf("Destination port:\t%d\n", dest_port);
}

int
extensions_error(unsigned char *ext, int n_ext)
{
	int i;

	printf("ERROR:\tExtension parse error\n");
	printf("Extensions data:");
	for (i = 0; i < n_ext; i++)
		printf(" 0x%x", (int)ext[i]);
	printf("\n");
	return (1);
}

int
print_extensions(unsigned char *extensions, int extensions_len)
{
	int i, j, l, type, subtype, sublen;

	for (i = 0; i < extensions_len; i++) {
		if(i > extensions_len - 4)
			return (extensions_error(extensions, extensions_len));
		type = extensions[i];
		l = (extensions[i + 1] << 8) + extensions[i + 2];
		i += 3;
		if (l <= 0 || i + l > extensions_len)
			return (extensions_error(extensions, extensions_len));
		switch(type) {
		case PP2_TYPE_ALPN:
			printf("ALPN extension:\t%.*s\n", l, extensions + i);
			break;
		case PP2_TYPE_AUTHORITY:
			printf("Authority extension:\t%.*s\n", l,
			    extensions + i);
			break;
		case PP2_TYPE_SSL:
			printf("PP2_TYPE_SSL client:\t0x%x\n",
			    *((char *)extensions + i));
			printf("PP2_TYPE_SSL verify:\t0x%x\n",
			    ntohl(*((uint32_t *)(extensions + i + 1))));
			j = i + 5;
			/*  Handle subtypes: */
			while (j < i + l) {
				subtype = extensions[j];
				sublen = (extensions[j + 1] << 8) +
				    extensions[j + 2];
				j += 3;
				switch (subtype) {
				case PP2_SUBTYPE_SSL_VERSION:
					printf("SSL_VERSION:\t%.*s\n",
					    sublen, extensions + j);
					break;
				case PP2_SUBTYPE_SSL_CIPHER:
					printf("SSL_CIPHER:\t%.*s\n",
					    sublen, extensions + j);
					break;
				}
				j += sublen;
			}
			break;
		default:
			printf("ERROR:\tUnknown extension %d\n", type);
			break;
		}
		i += l - 1;
	}
	if (i != extensions_len) {
		printf("ERROR:\tBuffer overrun (%d / %d)\n", i, extensions_len);
		return (1);
	}
	return (0);
}

int
main(int argc, const char **argv)
{
	unsigned char proxy_header[PP2_HEADER_MAX + 1];
	ssize_t n = 0;
	int address_len = 0;

	if (argc == 1)
		n = read(STDIN_FILENO, proxy_header, PP2_HEADER_MAX);
	else if (argc == 2)
		n = read_from_socket(argv[1], proxy_header, PP2_HEADER_MAX);
	else {
		fprintf(stderr, "Usage: parse_proxy_v2 [port]\n");
		return (1);
	}

	if (n < 16) {
		printf("ERROR:\tread too few bytes.\n");
		return (1);
	}
	proxy_header[n] = '\0';

	if (strncmp("PROXY TCP", (char *)proxy_header, 9) == 0) {
		/* PROXY version 1 over TCP */
		fprintf(stdout,
		    "ERROR:\tPROXY v1 parsing not supported in this tool.\n");
		return (1);
	} else if (memcmp(PP2_SIG, proxy_header, sizeof PP2_SIG) != 0) {
		printf("ERROR:\tNot a valid PROXY header\n");
		return (1);
	}

	printf("PROXY v2 detected.\n");
	if ((proxy_header[12] & PP2_VERSION_MASK) != PP2_VERSION) {
		printf("ERROR:\t13th byte has illegal version %02x\n",
		    proxy_header[12]);
		return (1);
	}

	switch (proxy_header[12] & PP2_CMD_MASK) {
	case PP2_CMD_LOCAL:
		printf("ERROR:\tLOCAL connection\n");
		return (1);
	case PP2_CMD_PROXY:
		printf("Connection:\tPROXYed connection detected\n");
		break;
	default:
		printf("ERROR:\t13th byte has illegal command %02x\n",
		    proxy_header[12]);
		return (1);
	}

	switch (proxy_header[13]) {
	case PP2_TRANS_UNSPEC|PP2_FAM_UNSPEC:
		printf("ERROR:\tProtocol:\tUnspecified/unsupported\n");
		return (1);
	case PP2_TRANS_STREAM|PP2_FAM_INET:
		printf("Protocol:\tTCP over IPv4\n");
		address_len = 12;
		break;
	case PP2_TRANS_DGRAM|PP2_FAM_INET:
		printf("Protocol:\tUDP over IPv4\n");
		printf("ERROR:\tProtocol unsupported in hitch seen\n");
		address_len = 12;
		break;
	case PP2_TRANS_STREAM|PP2_FAM_INET6:
		printf("Protocol:\tTCP over IPv6\n");
		address_len = 36;
		break;
	case PP2_TRANS_DGRAM|PP2_FAM_INET6:
		printf("Protocol:\tUDP over IPv6\n");
		printf("ERROR:\tProtocol unsupported in hitch\n");
		address_len = 36;
		break;
	case PP2_FAM_UNIX|PP2_TRANS_STREAM:
		printf("Protocol:\tUNIX stream\n");
		address_len = 216;
		break;
	case PP2_FAM_UNIX|PP2_TRANS_DGRAM:
		printf("Protocol:\tUNIX datagram\n");
		printf("ERROR:\tProtocol unsupported in hitch\n");
		address_len = 216;
		break;
	default:
		printf("ERROR:\t14th byte has illegal value %02x\n",
		    proxy_header[13]);
		return (1);
	}

	int additional_len = (proxy_header[14] << 8) + proxy_header[15];
	if (additional_len < address_len) {
		printf("ERROR:\tThe the total header length %d does"
		    " not leave room for the addresses\n",
		    additional_len + 16);
		return (1);
	}
	if (additional_len + 16 > n) {
		printf("ERROR:\tToo few bytes was read; %zd\n", n);
		return (1);
	}
	if (address_len == 12)
		print_addr_with_ports(AF_INET, 4, proxy_header + 16);
	else if (address_len == 36)
		print_addr_with_ports(AF_INET6, 16, proxy_header + 16);
	else {
		printf("ERROR:\tPrinting of UNIX socket addresses"
		    " not implemented.\n");
	}
	if (address_len < additional_len)
		return (print_extensions(proxy_header + 16 + address_len,
		    additional_len - address_len));
	return (0);
}
