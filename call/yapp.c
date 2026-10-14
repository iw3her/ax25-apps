/* yapp.c
 *
 * Copyright (C) 1994 by Jonathan Naylor
 *
 * This module implements the YAPP file transfer protocol as defined by Jeff
 * Jacobsen WA7MBL in the files yappxfer.doc and yappxfer.pas.
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the license, or (at your option) any later version.
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED

/*
 * Yapp C and Resume support added by S N Henson.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <linux/ax25.h>

#include "call.h"

#define	TIMEOUT		300	/* 5 Minutes */

#define	NUL		0x00
#define	SOH		0x01
#define	STX		0x02
#define	ETX		0x03
#define	EOT		0x04
#define	ENQ		0x05
#define	ACK		0x06
#define	DLE		0x10
#define	NAK		0x15
#define	CAN		0x18

#define	STATE_S		0
#define	STATE_SH	1
#define	STATE_SD	2
#define	STATE_SE	3
#define	STATE_ST	4
#define	STATE_R		5
#define	STATE_RH	6
#define	STATE_RD	7

static int state;
static int total = 0;

static int readlen = 0;
static int outlen = 0;
static int outbufptr = 0;
static unsigned char outbuffer[512];
static char yappc;		/* Nonzero if using YAPP C */

#define YAPP_SEND_ERROR   (-1)
#define YAPP_SEND_DONE    0
#define YAPP_SEND_PENDING 1

#define YAPP_HANDLER_ERROR 0
#define YAPP_HANDLER_OK    1
#define YAPP_HANDLER_WAIT  2

static unsigned char yapp_tx_pending[257];
static int yapp_tx_length;
static int yapp_tx_offset;
static int yapp_tx_next_state;
static int yapp_tx_abort_after;

static void yapp_tx_reset(void)
{
	yapp_tx_length = 0;
	yapp_tx_offset = 0;
	yapp_tx_next_state = -1;
	yapp_tx_abort_after = FALSE;
}

static int yapp_tx_is_pending(void)
{
	return yapp_tx_offset < yapp_tx_length;
}

static int yapp_tx_flush(void)
{
	ssize_t n;

	if (!yapp_tx_is_pending())
		return YAPP_SEND_DONE;

	n = write(fd, yapp_tx_pending + yapp_tx_offset,
		(size_t)(yapp_tx_length - yapp_tx_offset));
	if (n > 0) {
		if (n != yapp_tx_length - yapp_tx_offset) {
			errno = EPROTO;
			return YAPP_SEND_ERROR;
		}
		yapp_tx_offset = yapp_tx_length;
		if (yapp_tx_next_state >= 0)
			state = yapp_tx_next_state;
		yapp_tx_next_state = -1;
		return YAPP_SEND_DONE;
	}
	if (n == 0) {
		errno = EIO;
		return YAPP_SEND_ERROR;
	}
	if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
		return YAPP_SEND_PENDING;
	return YAPP_SEND_ERROR;
}

static int yapp_tx_send(const void *data, size_t length, int next_state,
	int abort_after)
{
	ssize_t n;

	if (length == 0 || length > sizeof(yapp_tx_pending) ||
		yapp_tx_is_pending()) {
		errno = EBUSY;
		return YAPP_SEND_ERROR;
	}

	n = write(fd, data, length);
	if (n == (ssize_t)length) {
		if (next_state >= 0)
			state = next_state;
		return YAPP_SEND_DONE;
	}
	if (n > 0) {
		errno = EPROTO;
		return YAPP_SEND_ERROR;
	}
	if (n == 0) {
		errno = EIO;
		return YAPP_SEND_ERROR;
	}
	if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
		return YAPP_SEND_ERROR;

	memcpy(yapp_tx_pending, data, length);
	yapp_tx_length = (int)length;
	yapp_tx_offset = 0;
	yapp_tx_next_state = next_state;
	yapp_tx_abort_after = abort_after;
	return YAPP_SEND_PENDING;
}

static int yapp_tx_finish_pending(void)
{
	int result = yapp_tx_flush();

	if (result == YAPP_SEND_DONE && yapp_tx_abort_after) {
		yapp_tx_abort_after = FALSE;
		return YAPP_SEND_ERROR;
	}
	return result;
}

/* Terminal control frames must not be abandoned when their first write gets
 * EAGAIN.  Wait for writability with the existing YAPP timeout, while also
 * monitoring RX so a congested terminal frame is not a write-only wait. */
static int yapp_tx_drain_terminal(void)
{
	struct timespec deadline;
	struct timespec now;
	struct timeval timeout;
	fd_set sock_read;
	fd_set sock_write;
	unsigned char discard[512];
	int result;
	int n;
	ssize_t length;

	if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
		return -1;
	deadline = now;
	deadline.tv_sec += TIMEOUT;

	while (yapp_tx_is_pending()) {
		if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
			return -1;
		if (now.tv_sec > deadline.tv_sec ||
			(now.tv_sec == deadline.tv_sec &&
			 now.tv_nsec >= deadline.tv_nsec)) {
			errno = ETIMEDOUT;
			return -1;
		}

		timeout.tv_sec = deadline.tv_sec - now.tv_sec;
		timeout.tv_usec = (deadline.tv_nsec - now.tv_nsec) / 1000;
		if (timeout.tv_usec < 0) {
			timeout.tv_sec--;
			timeout.tv_usec += 1000000;
		}

		FD_ZERO(&sock_read);
		FD_ZERO(&sock_write);
		FD_SET(fd, &sock_read);
		FD_SET(fd, &sock_write);
		n = select(fd + 1, &sock_read, &sock_write, NULL, &timeout);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (FD_ISSET(fd, &sock_read)) {
			length = read(fd, discard, sizeof(discard));
			if (length == 0) {
				errno = ECONNRESET;
				return -1;
			}
			if (length == -1 && errno != EINTR &&
				errno != EAGAIN && errno != EWOULDBLOCK)
				return -1;
		}
		if (!FD_ISSET(fd, &sock_write))
			continue;
		result = yapp_tx_flush();
		if (result == YAPP_SEND_ERROR)
			return -1;
	}
	yapp_tx_abort_after = FALSE;
	return 0;
}

static void Write_Status(char *s)
{
	fprintf(stdout, "State: %-60s\r", s);
	fflush(stdout);
}

static int Send_RR(int next_state)
{
	char buffer[2];

	buffer[0] = ACK;
	buffer[1] = 0x01;

	return yapp_tx_send(buffer, 2, next_state, FALSE);
}

static int Send_RF(int next_state)
{
	char buffer[2];

	buffer[0] = ACK;
	buffer[1] = 0x02;

	return yapp_tx_send(buffer, 2, next_state, FALSE);
}

static int Send_RT(int next_state)
{
	char buffer[2];

	buffer[0] = ACK;
	buffer[1] = ACK;

	return yapp_tx_send(buffer, 2, next_state, FALSE);
}

static int Send_AF(int next_state)
{
	char buffer[2];

	buffer[0] = ACK;
	buffer[1] = 0x03;

	return yapp_tx_send(buffer, 2, next_state, FALSE);
}

static int Send_AT(int next_state)
{
	char buffer[2];

	buffer[0] = ACK;
	buffer[1] = 0x04;

	return yapp_tx_send(buffer, 2, next_state, FALSE);
}

static int Send_NR(char *reason)
{
	char buffer[257];
	int length;
	int result;

	length = strlen(reason);
	if (length > 255)
		length = 255;

	buffer[0] = NAK;
	buffer[1] = length;
	memcpy(buffer + 2, reason, length);

	result = yapp_tx_send(buffer, length + 2, -1, TRUE);
	return result;
}

/* Send a Resume Sequence */

static int Send_RS(int length, int next_state)
{
	char buffer[256];
	int len;

	buffer[0] = NAK;
	buffer[2] = 'R';
	buffer[3] = 0;

	len = sprintf(buffer + 4, "%d", length) + 5;

	buffer[len] = 'C';
	buffer[len + 1] = 0;
	buffer[1] = len;

	return yapp_tx_send(buffer, len + 2, next_state, FALSE);
}

static int Send_SI(void)
{
	char buffer[2];

	buffer[0] = ENQ;
	buffer[1] = 0x01;

	return yapp_tx_send(buffer, 2, -1, FALSE);
}

static int Send_CN(char *reason)
{
	char buffer[257];
	int length;
	int result;

	length = strlen(reason);
	if (length > 255)
		length = 255;

	buffer[0] = CAN;
	buffer[1] = length;
	memcpy(buffer + 2, reason, length);

	result = yapp_tx_send(buffer, length + 2, -1, TRUE);
	return result;
}

static int yapp_send_terminal(int (*sender)(char *), char *reason)
{
	int result;

	result = sender(reason);
	if (result == YAPP_SEND_PENDING) {
		if (yapp_tx_drain_terminal() == -1)
			return YAPP_SEND_ERROR;
		return YAPP_SEND_DONE;
	}
	return result;
}

static int Send_HD(char *filename, long length)
{
	char buffer[257];
	char size_buffer[12];
	int len_filename;
	int len_size;
	int len;
	struct stat sb;

	sprintf(size_buffer, "%ld", length);

	len_filename = strlen(filename) + 1;	/* Include the NUL */
	len_size = strlen(size_buffer) + 1;	/* Include the NUL */

	len = len_filename + len_size;

	if (!stat(filename, &sb)) {
		unix2yapp(sb.st_mtime, buffer + len + 2);
		len += 9;
	}

	buffer[0] = SOH;
	buffer[1] = len;

	memcpy(buffer + 2, filename, len_filename);
	memcpy(buffer + len_filename + 2, size_buffer, len_size);

	return yapp_tx_send(buffer, len + 2, STATE_SH, FALSE);
}

static int Send_ET(void)
{
	char buffer[2];

	buffer[0] = EOT;
	buffer[1] = 0x01;

	return yapp_tx_send(buffer, 2, STATE_ST, FALSE);
}

static int Send_DT(int length)
{
	char buffer[2];

	if (length > 255)
		length = 0;

	buffer[0] = STX;
	buffer[1] = length;

	return yapp_tx_send(buffer, 2, STATE_SD, FALSE);
}

static int Send_EF(void)
{
	char buffer[2];

	buffer[0] = ETX;
	buffer[1] = 0x01;

	return yapp_tx_send(buffer, 2, STATE_SE, FALSE);
}

static unsigned char checksum(unsigned char *buf, int len)
{
	int i;
	unsigned char sum = 0;

	for (i = 0; i < len; i++)
		sum += buf[i];

	return sum;
}

static int yapp_download_data(int *filefd, unsigned char *buffer)
{
	int length, file_time;
	int send_result;
	char Message[50];

	if (buffer[0] == CAN || buffer[0] == NAK) {
		Write_Status("RcdABORT");
		return FALSE;
	}

	switch (state) {
	case STATE_R:
		if (buffer[0] == ENQ && buffer[1] == 0x01) {
			send_result = Send_RR(STATE_RH);
			if (send_result == YAPP_SEND_ERROR)
				return YAPP_HANDLER_ERROR;
			if (send_result == YAPP_SEND_PENDING)
				return YAPP_HANDLER_WAIT;
			Write_Status("RcvHeader");
			break;
		}

		send_result = Send_CN("Unknown code");
		Write_Status("SndABORT");
		if (send_result == YAPP_SEND_ERROR)
			return YAPP_HANDLER_ERROR;
		return send_result == YAPP_SEND_PENDING ?
			YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;

	case STATE_RH:
		if (buffer[0] == SOH) {
			/* Parse header: 3 fields == YAPP C */
			unsigned char *hptr, *hfield[3];
			length = buffer[1];
			if (length == 0)
				length = 256;
			hptr = buffer + 2;
			while (length > 0) {
				int hlen;
				hlen = strlen((char *)hptr) + 1;
				hfield[(int) yappc++] = hptr;
				hptr += hlen;
				length -= hlen;
			}

			if (yappc < 3) {
				yappc = 0;
			} else {
				file_time = yapp2unix((char *)hfield[2]);
				yappc = 1;
			}

			if (*filefd == -1) {
				*filefd = open((char *)hfield[0],
					       O_RDWR | O_APPEND | O_CREAT,
					       0666);
				if (*filefd == -1) {
					printf("\n[Unable to open %s]\n",
					       hfield[0]);
					send_result = Send_NR("Invalid filename");
					return send_result == YAPP_SEND_PENDING ?
						YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;
				}
			}

			printf("Receiving %s %s %s", hfield[0], hfield[1],
			       yappc ? ctime((time_t *) & file_time) :
			       " ");

			if (yappc) {
				struct stat sb;

				if (!fstat(*filefd, &sb) && sb.st_size)
					send_result = Send_RS(sb.st_size, STATE_RD);
				else
					send_result = Send_RT(STATE_RD);
			} else {
				send_result = Send_RF(STATE_RD);
			}
			if (send_result == YAPP_SEND_ERROR)
				return YAPP_HANDLER_ERROR;
			if (send_result == YAPP_SEND_PENDING)
				return YAPP_HANDLER_WAIT;
			break;
		}

		if (buffer[0] == ENQ && buffer[1] == 0x01) {
			break;
		}

		if (buffer[0] == EOT && buffer[1] == 0x01) {
			send_result = Send_AT(-1);
			Write_Status("RcvEOT");
			if (send_result == YAPP_SEND_ERROR)
				return YAPP_HANDLER_ERROR;
			if (send_result == YAPP_SEND_PENDING) {
				yapp_tx_abort_after = TRUE;
				return YAPP_HANDLER_WAIT;
			}
			return YAPP_HANDLER_ERROR;
		}

		send_result = Send_CN("Unknown code");
		Write_Status("SndABORT");
		if (send_result == YAPP_SEND_ERROR)
			return YAPP_HANDLER_ERROR;
		return send_result == YAPP_SEND_PENDING ?
			YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;

	case STATE_RD:
		if (buffer[0] == STX) {
			length = buffer[1];
			if (length == 0)
				length = 256;
			total += length;
			sprintf(Message, "RcvData  %5d bytes received",
				total);
			Write_Status(Message);

			if (yappc) {
				int i;
				unsigned char checksum = 0;

				for (i = 0; i < length; i++)
					checksum += buffer[i + 2];

				if (checksum != buffer[length + 2]) {
					send_result = Send_CN("Bad Checksum");
					Write_Status
					    ("SndABORT: Bad Checksum");
					if (send_result == YAPP_SEND_ERROR)
						return YAPP_HANDLER_ERROR;
					return send_result == YAPP_SEND_PENDING ?
						YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;
				}
			}

			write(*filefd, buffer + 2, length);
			break;
		}

		if (buffer[0] == ETX && buffer[1] == 0x01) {
			send_result = Send_AF(STATE_RH);
			Write_Status("RcvEof");
			close(*filefd);
			*filefd = -1;
			if (send_result == YAPP_SEND_ERROR)
				return YAPP_HANDLER_ERROR;
			if (send_result == YAPP_SEND_PENDING)
				return YAPP_HANDLER_WAIT;
			break;
		}

		send_result = Send_CN("Unknown code");
		Write_Status("SndABORT");
		if (send_result == YAPP_SEND_ERROR)
			return YAPP_HANDLER_ERROR;
		return send_result == YAPP_SEND_PENDING ?
			YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;
	}

	return YAPP_HANDLER_OK;
}

static void yapp_download(int filefd)
{
	struct timeval timeval;
	fd_set sock_read;
	fd_set sock_write;
	int n;
	int buflen = 0;
	int length;
	int used;
	int handler_result;
	int tx_result;
	unsigned char buffer[1024];

	Write_Status("RcvWait");

	state = STATE_R;
	total = 0;
	yappc = 0;
	yapp_tx_reset();

	while (TRUE) {
		FD_ZERO(&sock_read);
		FD_ZERO(&sock_write);
		if (!yapp_tx_is_pending())
			FD_SET(STDIN_FILENO, &sock_read);
		FD_SET(fd, &sock_read);
		if (yapp_tx_is_pending())
			FD_SET(fd, &sock_write);

		timeval.tv_usec = 0;
		timeval.tv_sec = TIMEOUT;

		n = select(fd + 1, &sock_read, &sock_write, NULL, &timeval);

		if (n == -1) {
			if (!interrupted && errno == EAGAIN)
				continue;
			if (!interrupted)
				perror("select");
			yapp_send_terminal(Send_CN, "Internal error");
			Write_Status("SndABORT");
			return;
		}

		if (n == 0) {	/* Timeout */
			yapp_send_terminal(Send_CN, "Timeout");
			Write_Status("SndABORT");
			return;
		}

		if (yapp_tx_is_pending() && FD_ISSET(fd, &sock_write)) {
			tx_result = yapp_tx_finish_pending();
			if (tx_result == YAPP_SEND_ERROR) {
				perror("write");
				return;
			}
			if (yapp_tx_is_pending())
				continue;
		}

		if (FD_ISSET(STDIN_FILENO, &sock_read)) {
			yapp_send_terminal(Send_CN, "Cancelled by user");
			Write_Status("SndABORT");
			return;
		}

		if (FD_ISSET(fd, &sock_read)) {
			length = read(fd, buffer + buflen, 511);
			if (length > 0) {
				buflen += length;

				do {
					used = FALSE;

					switch (buffer[0]) {
					case ACK:
					case ENQ:
					case ETX:
					case EOT:
						if (buflen >= 2) {
							handler_result = yapp_download_data(&filefd, buffer);
							if (handler_result == YAPP_HANDLER_ERROR)
								return;
							buflen -= 2;
							memcpy(buffer,
							       buffer + 2,
							       buflen);
							used = TRUE;
							if (handler_result == YAPP_HANDLER_WAIT)
								break;
						}
						break;
					default:
						length = buffer[1];
						if (length == 0)
							length = 256;
						if (buffer[0] == STX)
							length += yappc;
						if (buflen >= (length + 2)) {
							handler_result = yapp_download_data(&filefd, buffer);
							if (handler_result == YAPP_HANDLER_ERROR)
								return;
							buflen -=
							    length + 2;
							memcpy(buffer,
							       buffer +
							       length + 2,
							       buflen);
							used = TRUE;
							if (handler_result == YAPP_HANDLER_WAIT)
								break;
						}
						break;
					}
				}
				while (used);
			}
		}
	}
}

static int yapp_upload_data(int filefd, char *filename, int filelength,
			    unsigned char *buffer)
{
	int send_result;
	char Message[80];

	if (buffer[0] == CAN || buffer[0] == NAK) {
		Write_Status("RcvABORT");
		return FALSE;
	}

	switch (state) {
	case STATE_S:
		if (buffer[0] == ACK && buffer[1] == 0x01) {
			Write_Status("SendHeader");
			send_result = Send_HD(filename, filelength);
			if (send_result == YAPP_SEND_ERROR)
				return YAPP_HANDLER_ERROR;
			if (send_result == YAPP_SEND_PENDING)
				return YAPP_HANDLER_WAIT;
			break;
		}

		if (buffer[0] == ACK && buffer[1] == 0x02) {
			sprintf(Message, "SendData  %5d bytes transmitted",
				total);
			Write_Status(Message);
			outlen = read(filefd, outbuffer, readlen);
			outbufptr = 0;

			if (yappc) {
				outbuffer[outlen] =
				    checksum(outbuffer, outlen);
				outlen++;
			}
			if (outlen) {
				send_result = Send_DT(outlen);
				if (send_result == YAPP_SEND_ERROR)
					return YAPP_HANDLER_ERROR;
				if (send_result == YAPP_SEND_PENDING)
					return YAPP_HANDLER_WAIT;
			} else
				state = STATE_SD;

			state = STATE_SD;
			break;
		}

		send_result = Send_CN("Unknown code");
		Write_Status("SndABORT");
		if (send_result == YAPP_SEND_ERROR)
			return YAPP_HANDLER_ERROR;
		return send_result == YAPP_SEND_PENDING ?
			YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;

	case STATE_SH:
		/* Could get three replies here:
		 * ACK 02 : normal acknowledge.
		 * ACK ACK: yappc acknowledge.
		 * NAK ...: resume request.
		 */
		if (buffer[0] == NAK && buffer[2] == 'R') {
			int len;
			off_t rpos;

			len = buffer[1];
			if (buffer[len] == 'C')
				yappc = 1;
			rpos = atol((char *)buffer + 4);
			lseek(filefd, rpos, SEEK_SET);
			buffer[0] = ACK;
			buffer[1] = yappc ? ACK : 0x02;
		}

		if (buffer[0] == ACK &&
		    (buffer[1] == 0x02 || buffer[1] == ACK)) {
			if (buffer[1] == ACK)
				yappc = 1;

			sprintf(Message, "SendData  %5d bytes transmitted",
				total);
			Write_Status(Message);
			outlen = read(filefd, outbuffer, readlen);
			outbufptr = 0;

			if (yappc) {
				outbuffer[outlen] =
				    checksum(outbuffer, outlen);
				outlen++;
			}
			if (outlen) {
				send_result = Send_DT(outlen);
				if (send_result == YAPP_SEND_ERROR)
					return YAPP_HANDLER_ERROR;
				if (send_result == YAPP_SEND_PENDING)
					return YAPP_HANDLER_WAIT;
			} else
				state = STATE_SD;
			break;
		}

		send_result = Send_CN("Unknown code");
		Write_Status("SndABORT");
		if (send_result == YAPP_SEND_ERROR)
			return YAPP_HANDLER_ERROR;
		return send_result == YAPP_SEND_PENDING ?
			YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;

	case STATE_SD:
		send_result = Send_CN("Unknown code");
		Write_Status("SndABORT");
		if (send_result == YAPP_SEND_ERROR)
			return YAPP_HANDLER_ERROR;
		return send_result == YAPP_SEND_PENDING ?
			YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;

	case STATE_SE:
		if (buffer[0] == ACK && buffer[1] == 0x03) {
			Write_Status("SendEOT");
			send_result = Send_ET();
			if (send_result == YAPP_SEND_ERROR)
				return YAPP_HANDLER_ERROR;
			if (send_result == YAPP_SEND_PENDING)
				return YAPP_HANDLER_WAIT;
			break;
		}

		send_result = Send_CN("Unknown code");
		Write_Status("SndABORT");
		if (send_result == YAPP_SEND_ERROR)
			return YAPP_HANDLER_ERROR;
		return send_result == YAPP_SEND_PENDING ?
			YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;

	case STATE_ST:
		if (buffer[0] == ACK && buffer[1] == 0x04) {
			return FALSE;
		}

		send_result = Send_CN("Unknown code");
		Write_Status("SndABORT");
		if (send_result == YAPP_SEND_ERROR)
			return YAPP_HANDLER_ERROR;
		return send_result == YAPP_SEND_PENDING ?
			YAPP_HANDLER_WAIT : YAPP_HANDLER_ERROR;
	}

	return YAPP_HANDLER_OK;
}

static void yapp_upload(int filefd, char *filename, long filelength)
{
	struct timeval timeval;
	fd_set sock_read;
	fd_set sock_write;
	int n;
	unsigned char buffer[1024];
	int buflen = 0;
	int length;
	int used;
	int handler_result;
	char Message[80];

	Write_Status("SendInit");

	readlen = (paclen - 2 > 253) ? 253 : paclen - 2;
	state = STATE_S;
	total = 0;
	yappc = 0;
	yapp_tx_reset();

	if (Send_SI() == YAPP_SEND_ERROR) {
		perror("write");
		return;
	}

	while (TRUE) {
		FD_ZERO(&sock_read);
		FD_ZERO(&sock_write);
		if (!yapp_tx_is_pending())
			FD_SET(STDIN_FILENO, &sock_read);
		FD_SET(fd, &sock_read);

		if (state == STATE_SD || yapp_tx_is_pending()) {
			FD_SET(fd, &sock_write);

			n = select(fd + 1, &sock_read, &sock_write, NULL,
				   NULL);
		} else {
			timeval.tv_usec = 0;
			timeval.tv_sec = TIMEOUT;

			n = select(fd + 1, &sock_read, NULL, NULL,
				   &timeval);
		}

		if (n == -1) {
			if (!interrupted && errno == EAGAIN)
				continue;
			if (!interrupted)
				perror("select");
			Write_Status("SndABORT");
			yapp_send_terminal(Send_CN, "Internal error");
			return;
		}

		if (n == 0) {	/* Timeout, not STATE_SD */
			Write_Status("SndABORT");
			yapp_send_terminal(Send_CN, "Timeout");
			return;
		}

		if (yapp_tx_is_pending() && FD_ISSET(fd, &sock_write)) {
			n = yapp_tx_finish_pending();
			if (n == YAPP_SEND_ERROR) {
				perror("write");
				return;
			}
			if (yapp_tx_is_pending())
				continue;
		}

		if (FD_ISSET(STDIN_FILENO, &sock_read)) {
			Write_Status("SndABORT");
			yapp_send_terminal(Send_CN, "Cancelled by user");
			return;
		}

		if (FD_ISSET(fd, &sock_write)) {	/* Writable, only STATE_SD */
			if (outlen > 0) {
				n = write(fd, outbuffer + outbufptr, outlen);
				if (n > 0) {
					if (n != outlen) {
						errno = EPROTO;
						perror("write");
						return;
					}
					outbufptr += n;
					outlen -= n;
					total += n;
				} else if (n == 0) {
					errno = EIO;
					perror("write");
					return;
				} else if (errno != EINTR && errno != EAGAIN &&
					errno != EWOULDBLOCK) {
					perror("write");
					return;
				}
			}

			if (outlen == 0) {
				total -= yappc;
				if ((outlen =
				     read(filefd, outbuffer,
					  readlen)) > 0) {
					sprintf(Message,
						"SendData  %5d bytes transmitted",
						total);
					Write_Status(Message);

					outbufptr = 0;
					if (Send_DT(outlen) == YAPP_SEND_ERROR) {
						perror("write");
						return;
					}

					if (yappc) {
						outbuffer[outlen] =
						    checksum(outbuffer,
							     outlen);
						outlen++;
					}
				} else {
					Write_Status("SendEof");
					if (Send_EF() == YAPP_SEND_ERROR) {
						perror("write");
						return;
					}
				}
			}
		}

		if (FD_ISSET(fd, &sock_read)) {
			length = read(fd, buffer + buflen, 511);
			if (length > 0) {
				buflen += length;

				do {
					used = FALSE;

					switch (buffer[0]) {
					case ACK:
					case ENQ:
					case ETX:
					case EOT:
						if (buflen >= 2) {
						handler_result = yapp_upload_data(filefd, filename,
							filelength, buffer);
						if (handler_result == YAPP_HANDLER_ERROR)
								return;
							buflen -= 2;
							memcpy(buffer,
							       buffer + 2,
							       buflen);
							used = TRUE;
						if (handler_result == YAPP_HANDLER_WAIT)
							break;
						}
						break;
					default:
						length = buffer[1];
						if (length == 0)
							length = 256;
						if (buflen >= (length + 2)) {
							handler_result = yapp_upload_data(filefd, filename,
							filelength, buffer);
						if (handler_result == YAPP_HANDLER_ERROR)
								return;
							buflen -=
							    length + 2;
							memcpy(buffer,
							       buffer +
							       length + 2,
							       buflen);
							used = TRUE;
						if (handler_result == YAPP_HANDLER_WAIT)
							break;
						}
						break;
					}
				}
				while (used);
			}
		}
	}
}

void cmd_yapp(char *buf, int bytes)
{
	int filefd;
	long size = 0L;
	char *t;

	if (bytes == 0)
		return;

	switch (buf[0]) {
	case 'U':
	case 'u':
		t = strchr(buf, '\n');
		if (t != NULL)
			*t = '\0';
		t = buf + 2;
		while (*t != '\0' && isspace(*t))
			t++;
		if (*t == '\0') {
			printf
			    ("\n[YAPP Upload requires a filename - eg ~yu hello.txt]\n");
			if (yapp_send_terminal(Send_NR, "No filename") ==
				YAPP_SEND_ERROR)
				perror("write");
			return;
		}
		filefd = open(t, O_RDONLY);
		if (filefd == -1) {
			printf("\n[Unable to open upload file]\n");
			if (yapp_send_terminal(Send_NR, "Invalid filename") ==
				YAPP_SEND_ERROR)
				perror("write");
			return;
		}
		if (lseek(filefd, 0L, SEEK_END) != -1)
			size = lseek(filefd, 0L, SEEK_CUR);
		lseek(filefd, 0L, SEEK_SET);
		if (size != -1)
			printf
			    ("\n[Uploading %ld bytes from %s using YAPP]\n",
			     size, t);
		else
			printf("\n[Uploading from %s using YAPP]\n", t);
		yapp_upload(filefd, t, size);
		close(filefd);
		printf("[Finished YAPP Upload, %d bytes Transmitted]\n",
		       total);
		break;

	case 'D':
	case 'd':
		t = strchr(buf, '\n');
		if (t != NULL)
			*t = '\0';
		t = buf + 2;
		while (*t != '\0' && isspace(*t))
			t++;
		if (*t == '\0')
			filefd = -1;
		else {filefd = open(t, O_RDWR | O_APPEND | O_CREAT, 0666);
			if (filefd == -1) {
				printf("\n[Unable to open %s]\n", buf + 2);
				if (yapp_send_terminal(Send_NR, "Invalid filename") ==
					YAPP_SEND_ERROR)
					perror("write");
				return;
			}
		}
		printf("\n[Downloading using YAPP]\n");
		yapp_download(filefd);
		close(filefd);
		printf("[Finished YAPP Download, %d bytes received]\n",
		       total);
		break;

	default:
		printf("\nUnknown '~y' escape. Type ~h for a list.\n");
		break;
	}
}
