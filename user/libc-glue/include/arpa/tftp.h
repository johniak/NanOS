/*
 * arpa/tftp.h — TFTP packet format (RFC 1350, classic BSD layout). NanOS doesn't ship a TFTP
 * client/server, but inetutils' shared libinetutils references struct tftphdr at compile time.
 */
#ifndef _ARPA_TFTP_H
#define _ARPA_TFTP_H

#define SEGSIZE 512   /* data segment size */

/* opcodes */
#define RRQ   1   /* read request */
#define WRQ   2   /* write request */
#define DATA  3   /* data packet */
#define ACK   4   /* acknowledgement */
#define ERROR 5   /* error code */
#define OACK  6   /* option acknowledgement */

struct tftphdr {
	short th_opcode;       /* packet type */
	union {
		unsigned short tu_block;  /* block # */
		short          tu_code;   /* error code */
		char           tu_stuff[1]; /* request packet stuff */
	} th_u;
	char th_data[1];       /* data or error string */
};

#define th_block th_u.tu_block
#define th_code  th_u.tu_code
#define th_stuff th_u.tu_stuff
#define th_msg   th_data

/* error codes */
#define EUNDEF    0   /* not defined */
#define ENOTFOUND 1   /* file not found */
#define EACCESS   2   /* access violation */
#define ENOSPACE  3   /* disk full or allocation exceeded */
#define EBADOP    4   /* illegal TFTP operation */
#define EBADID    5   /* unknown transfer ID */
#define EEXISTS   6   /* file already exists */
#define ENOUSER   7   /* no such user */

#endif /* _ARPA_TFTP_H */
