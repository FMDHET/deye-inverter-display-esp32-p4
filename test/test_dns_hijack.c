#include "harness.h"

/* The captive portal's DNS reply. This parser is fed straight off the network
 * by anyone who can reach the emergency access point, so the malformed cases
 * matter as much as the working one -- and the EDNS0 case below is the reason
 * the portal did not open on Windows or Chrome. */
#include "dns_hijack.h"

#define AP_IP  0x0104A8C0u        /* 192.168.4.1, network byte order */

/* Build "captive.apple.com" style queries. `extra` is appended verbatim after
 * the question -- that is where an EDNS0 OPT record sits. */
static int make_query(uint8_t *q, const char *const *labels, int nlabels,
                      uint16_t qtype, uint16_t arcount,
                      const uint8_t *extra, int extralen)
{
    int p = 0;
    q[p++] = 0x12; q[p++] = 0x34;              /* transaction id */
    q[p++] = 0x01; q[p++] = 0x00;              /* RD */
    q[p++] = 0x00; q[p++] = 0x01;              /* QDCOUNT */
    q[p++] = 0x00; q[p++] = 0x00;              /* ANCOUNT */
    q[p++] = 0x00; q[p++] = 0x00;              /* NSCOUNT */
    q[p++] = (uint8_t)(arcount >> 8); q[p++] = (uint8_t)arcount;
    for (int i = 0; i < nlabels; i++) {
        int l = (int)strlen(labels[i]);
        q[p++] = (uint8_t)l;
        memcpy(&q[p], labels[i], (size_t)l);
        p += l;
    }
    q[p++] = 0x00;                             /* root label */
    q[p++] = (uint8_t)(qtype >> 8); q[p++] = (uint8_t)qtype;
    q[p++] = 0x00; q[p++] = 0x01;              /* class IN */
    if (extra && extralen) { memcpy(&q[p], extra, (size_t)extralen); p += extralen; }
    return p;
}

static const char *const APPLE[] = { "captive", "apple", "com" };

/* An EDNS0 OPT record as Windows and Chrome send it: root name, type 41,
 * "class" = UDP payload size, then TTL and a zero RDLENGTH. */
static const uint8_t OPT_RR[] = {
    0x00,                    /* root name              */
    0x00, 0x29,              /* type 41 = OPT          */
    0x10, 0x00,              /* UDP payload size 4096  */
    0x00, 0x00, 0x00, 0x00,  /* extended rcode + flags */
    0x00, 0x00,              /* RDLENGTH 0             */
};

/* ------------------------------ the question --------------------------- */

static void test_a_plain_question_is_measured(void)
{
    uint8_t q[512];
    int qlen = make_query(q, APPLE, 3, 1, 0, NULL, 0);
    uint16_t qt = 0;
    /* 12 header + (1+7)+(1+5)+(1+3) + 1 root + 4 = 35 */
    CHECK_I(dns_question_end(q, qlen, &qt), 35);
    CHECK_I(qt, 1);
    CHECK_I(qlen, 35);
}

static void test_a_truncated_name_is_refused(void)
{
    uint8_t q[512];
    int qlen = make_query(q, APPLE, 3, 1, 0, NULL, 0);
    uint16_t qt = 0;
    /* Cut the packet mid-name: the label lengths now point past the end. */
    CHECK_I(dns_question_end(q, 20, &qt), -1);
    /* And cut it between the name and QTYPE/QCLASS. */
    CHECK_I(dns_question_end(q, qlen - 2, &qt), -1);
}

static void test_a_compression_pointer_is_refused_not_followed(void)
{
    uint8_t q[512];
    int qlen = make_query(q, APPLE, 3, 1, 0, NULL, 0);
    q[12] = 0xC0;                      /* pointer where a label length belongs */
    q[13] = 0x0C;                      /* ... pointing at itself: a loop */
    uint16_t qt = 0;
    CHECK_I(dns_question_end(q, qlen, &qt), -1);
}

static void test_a_header_without_exactly_one_question_is_refused(void)
{
    uint8_t q[512];
    int qlen = make_query(q, APPLE, 3, 1, 0, NULL, 0);
    uint16_t qt = 0;

    q[4] = 0x00; q[5] = 0x00;          /* QDCOUNT 0 */
    CHECK_I(dns_question_end(q, qlen, &qt), -1);
    q[4] = 0x00; q[5] = 0x02;          /* QDCOUNT 2 -- we only build one */
    CHECK_I(dns_question_end(q, qlen, &qt), -1);
}

static void test_a_runt_packet_is_refused(void)
{
    uint8_t q[16] = { 0 };
    uint16_t qt = 0;
    CHECK_I(dns_question_end(q, 11, &qt), -1);     /* shorter than a header */
    CHECK_I(dns_question_end(NULL, 64, &qt), -1);
}

/* ------------------------------- the reply ----------------------------- */

static void test_an_a_query_gets_the_ap_address(void)
{
    uint8_t q[512], r[512];
    int qlen = make_query(q, APPLE, 3, 1, 0, NULL, 0);
    int rlen = dns_build_reply(q, qlen, AP_IP, r, sizeof(r));

    CHECK_I(rlen, qlen + DNS_ANSWER_LEN);
    CHECK_I(r[0], 0x12); CHECK_I(r[1], 0x34);      /* id echoed */
    CHECK_I(r[2], 0x81);                           /* QR + RD */
    CHECK_I(r[3], 0x80);                           /* RA, RCODE 0 */
    CHECK_I((r[4] << 8) | r[5], 1);                /* QDCOUNT */
    CHECK_I((r[6] << 8) | r[7], 1);                /* ANCOUNT */
    CHECK_I((r[10] << 8) | r[11], 0);              /* ARCOUNT */

    const uint8_t *a = r + qlen;
    CHECK_I(a[0], 0xC0); CHECK_I(a[1], 0x0C);      /* name -> the question */
    CHECK_I((a[2] << 8) | a[3], 1);                /* type A */
    CHECK_I((a[4] << 8) | a[5], 1);                /* class IN */
    CHECK_I((a[10] << 8) | a[11], 4);              /* RDLENGTH */
    uint32_t ip;
    memcpy(&ip, a + 12, 4);
    CHECK_I(ip, AP_IP);
}

static void test_an_edns_query_loses_its_opt_record(void)
{
    /* THE case this rewrite exists for. Windows and Chrome attach an OPT
     * record. The old reply copied the whole datagram and only zeroed
     * ARCOUNT, so those eleven bytes stayed between the question and the
     * appended answer -- the client parsed the OPT record AS the answer
     * (root name, type 41) and the portal never opened. */
    uint8_t q[512], r[512];
    int qlen = make_query(q, APPLE, 3, 1, 1, OPT_RR, (int)sizeof(OPT_RR));
    int qend = 35;
    CHECK_I(qlen, qend + (int)sizeof(OPT_RR));     /* the OPT record is there */

    int rlen = dns_build_reply(q, qlen, AP_IP, r, sizeof(r));

    /* The reply is question-sized plus the answer: the OPT bytes are gone,
     * not merely unannounced. */
    CHECK_I(rlen, qend + DNS_ANSWER_LEN);
    CHECK_I((r[10] << 8) | r[11], 0);              /* ARCOUNT */

    /* And what directly follows the question really is our A record. */
    const uint8_t *a = r + qend;
    CHECK_I(a[0], 0xC0);
    CHECK_I((a[2] << 8) | a[3], 1);                /* type A, not 41 */
    uint32_t ip;
    memcpy(&ip, a + 12, 4);
    CHECK_I(ip, AP_IP);
}

static void test_aaaa_gets_an_empty_answer_not_an_a_record(void)
{
    /* Handing an A record back for a AAAA question is a malformed answer.
     * NOERROR with no records makes the client ask for A next. */
    uint8_t q[512], r[512];
    int qlen = make_query(q, APPLE, 3, 28, 0, NULL, 0);   /* AAAA */
    int rlen = dns_build_reply(q, qlen, AP_IP, r, sizeof(r));

    CHECK_I(rlen, qlen);                           /* no answer appended */
    CHECK_I((r[6] << 8) | r[7], 0);                /* ANCOUNT 0 */
    CHECK_I(r[3] & 0x0F, 0);                       /* RCODE still NOERROR */
    CHECK_I((r[4] << 8) | r[5], 1);                /* the question is kept */
}

static void test_any_is_answered_like_a(void)
{
    uint8_t q[512], r[512];
    int qlen = make_query(q, APPLE, 3, 255, 0, NULL, 0);
    int rlen = dns_build_reply(q, qlen, AP_IP, r, sizeof(r));
    CHECK_I(rlen, qlen + DNS_ANSWER_LEN);
    CHECK_I((r[6] << 8) | r[7], 1);
}

static void test_an_unparsable_query_gets_no_reply_at_all(void)
{
    uint8_t q[512], r[512];
    int qlen = make_query(q, APPLE, 3, 1, 0, NULL, 0);
    q[12] = 0xC0;                                  /* compression pointer */
    /* -1 means "stay silent": answering something we could not read would be
     * worse than not answering. */
    CHECK_I(dns_build_reply(q, qlen, AP_IP, r, sizeof(r)), -1);
}

static void test_the_output_buffer_is_respected(void)
{
    uint8_t q[512], r[512];
    int qlen = make_query(q, APPLE, 3, 1, 0, NULL, 0);
    int need = qlen + DNS_ANSWER_LEN;

    CHECK_I(dns_build_reply(q, qlen, AP_IP, r, need - 1), -1);
    CHECK_I(dns_build_reply(q, qlen, AP_IP, r, need), need);
    CHECK_I(dns_build_reply(q, qlen, AP_IP, NULL, need), -1);
}

static void test_a_maximum_length_name_still_works(void)
{
    /* Four 63-byte labels: 255 bytes of name, the DNS limit. It must be
     * accepted, and one byte more must not be. */
    static char l[4][64];
    const char *labels[4];
    for (int i = 0; i < 4; i++) {
        memset(l[i], 'a', 62); l[i][62] = '\0';    /* 62 + 1 length byte = 63 */
        labels[i] = l[i];
    }
    uint8_t q[512];
    uint16_t qt = 0;
    int qlen = make_query(q, labels, 4, 1, 0, NULL, 0);
    CHECK(dns_question_end(q, qlen, &qt) > 0);

    /* 255 is the cap; a fifth label pushes past it. */
    const char *five[5] = { labels[0], labels[1], labels[2], labels[3], labels[0] };
    qlen = make_query(q, five, 5, 1, 0, NULL, 0);
    CHECK_I(dns_question_end(q, qlen, &qt), -1);
}

int main(void)
{
    RUN(test_a_plain_question_is_measured);
    RUN(test_a_truncated_name_is_refused);
    RUN(test_a_compression_pointer_is_refused_not_followed);
    RUN(test_a_header_without_exactly_one_question_is_refused);
    RUN(test_a_runt_packet_is_refused);

    RUN(test_an_a_query_gets_the_ap_address);
    RUN(test_an_edns_query_loses_its_opt_record);
    RUN(test_aaaa_gets_an_empty_answer_not_an_a_record);
    RUN(test_any_is_answered_like_a);
    RUN(test_an_unparsable_query_gets_no_reply_at_all);
    RUN(test_the_output_buffer_is_respected);
    RUN(test_a_maximum_length_name_still_works);
    return t_report("dns_hijack");
}
