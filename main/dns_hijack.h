#pragma once

/* The captive portal's DNS reply -- built, not echoed.
 *
 * The hijack answers every A query with the AP's own address so the operating
 * system's captive-portal probe lands on our page. It used to do that by
 * copying the whole request, overwriting the counts in the header and
 * appending an answer. That works only for a bare query: Windows and Chrome
 * attach an EDNS0 OPT record in the additional section, and those bytes stayed
 * in the copy. Setting ARCOUNT to 0 did not remove them -- it only stopped
 * announcing them, so the client read the OPT record where the answer should
 * be, parsed it as one (root name, type 41) and gave up. The portal never
 * opened on the two clients that matter most.
 *
 * Building the reply from the question alone fixes that by construction:
 * anything we do not understand is simply not carried over. Dropping OPT is
 * also the correct way to tell a client there is no EDNS here; it falls back
 * on its own.
 *
 * Split out of captive.c so it can be tested: this is a parser fed straight
 * off the network by anyone who can reach the SoftAP, so its bounds are worth
 * more than a comment.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes an A answer adds after the question. */
#define DNS_ANSWER_LEN  16

/* Walk the first question and return the offset just past it (QNAME + QTYPE +
 * QCLASS), writing QTYPE to *qtype. Returns -1 when the message does not hold
 * exactly one parsable question.
 *
 * Everything is bounded against `len`: a label length that runs off the end, a
 * name longer than the 255 bytes DNS allows, or a compression pointer (0xC0 --
 * legal in an answer, never in a question, and a loop risk) is rejected rather
 * than followed. */
static inline int dns_question_end(const uint8_t *m, int len, uint16_t *qtype)
{
    if (!m || len < 12) return -1;
    if (((m[4] << 8) | m[5]) != 1) return -1;      /* exactly one question */

    int off = 12, name = 0;
    for (;;) {
        if (off >= len) return -1;
        uint8_t l = m[off];
        if (l == 0) { off++; break; }              /* root label ends QNAME */
        if (l & 0xC0) return -1;                   /* pointer / reserved */
        off  += 1 + l;
        name += 1 + l;
        if (name > 255) return -1;
    }
    if (off + 4 > len) return -1;                  /* QTYPE + QCLASS */
    if (qtype) *qtype = (uint16_t)((m[off] << 8) | m[off + 1]);
    return off + 4;
}

/* Build the reply to `q` into `out`. `ip_be` is the address to hand out, in
 * network byte order. Returns the reply length, or -1 when the query cannot be
 * answered and the right move is to stay silent.
 *
 * An A answer is only an answer to an A question. For AAAA -- or anything else
 * -- the reply is NOERROR with no records, so the client asks for A next
 * instead of treating the response as broken. */
static inline int dns_build_reply(const uint8_t *q, int qlen, uint32_t ip_be,
                                  uint8_t *out, int outcap)
{
    uint16_t qtype = 0;
    int qend = dns_question_end(q, qlen, &qtype);
    if (qend < 0) return -1;

    bool answer = (qtype == 1 || qtype == 255);    /* A, or ANY */
    int  need   = qend + (answer ? DNS_ANSWER_LEN : 0);
    if (!out || outcap < need) return -1;

    memcpy(out, q, (size_t)qend);                  /* header + question only */
    out[2] = 0x81;                 /* QR=1, RD copied from the query */
    out[3] = 0x80;                 /* RA=1, RCODE=0                  */
    out[4] = 0x00; out[5] = 0x01;  /* QDCOUNT = 1, the one we echoed */
    out[6] = 0x00; out[7] = answer ? 0x01 : 0x00;          /* ANCOUNT */
    out[8] = 0x00; out[9] = 0x00;                          /* NSCOUNT */
    out[10] = 0x00; out[11] = 0x00;  /* ARCOUNT -- and the OPT record with it */

    int p = qend;
    if (answer) {
        out[p++] = 0xC0; out[p++] = 0x0C;      /* name -> offset 12 */
        out[p++] = 0x00; out[p++] = 0x01;      /* type A            */
        out[p++] = 0x00; out[p++] = 0x01;      /* class IN          */
        out[p++] = 0x00; out[p++] = 0x00;
        out[p++] = 0x00; out[p++] = 0x3C;      /* TTL 60 s          */
        out[p++] = 0x00; out[p++] = 0x04;      /* RDLENGTH 4        */
        memcpy(&out[p], &ip_be, 4);            /* RDATA = AP IP     */
        p += 4;
    }
    return p;
}

#ifdef __cplusplus
}
#endif
