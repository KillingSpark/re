/**
 * @file accept.c  SIP Session Accept
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re_types.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_hash.h>
#include <re_fmt.h>
#include <re_uri.h>
#include <re_tmr.h>
#include <re_msg.h>
#include <re_sip.h>
#include <re_sipsess.h>
#include "sipsess.h"


static void cancel_handler(const struct sip_msg *msg, void *arg)
{
	struct sipsess *sess = arg;
	sipsess_cancel_h *cancelh;

	(void)sip_treply(&sess->st, sess->sip, sess->msg,
			 487, "Request Terminated");

	sess->peerterm = true;

	if (sess->terminated)
		return;
	cancelh = sess->cancelh;

	if (cancelh)
		cancelh(msg, sess->arg);

	sipsess_terminate(sess, ECONNRESET, NULL);
}


/**
 * Accept an incoming SIP Session connection
 *
 * @param sessp     Pointer to allocated SIP Session
 * @param sock      SIP Session socket
 * @param msg       Incoming SIP message
 * @param scode     Response status code
 * @param reason    Response reason phrase
 * @param rel100    Sending 1xx reliably supported, required or disabled
 * @param cuser     Contact username or URI
 * @param ctype     Session content-type
 * @param desc      Content description (e.g. SDP)
 * @param authh     SIP Authentication handler
 * @param aarg      Authentication handler argument
 * @param aref      True to mem_ref() aarg
 * @param offerh    Session offer handler
 * @param answerh   Session answer handler
 * @param estabh    Session established handler
 * @param infoh     Session info handler
 * @param referh    Session refer handler
 * @param closeh    Session close handler
 * @param arg       Handler argument
 * @param fmt       Formatted strings with extra SIP Headers
 *
 * @return 0 if success, otherwise errorcode
 */
int sipsess_accept(struct sipsess **sessp, struct sipsess_sock *sock,
		   const struct sip_msg *msg, uint16_t scode,
		   const char *reason, enum rel100_mode rel100,
		   const char *cuser, const char *ctype, struct mbuf *desc,
		   sip_auth_h *authh, void *aarg, bool aref,
		   sipsess_offer_h *offerh, sipsess_answer_h *answerh,
		   sipsess_estab_h *estabh, sipsess_info_h *infoh,
		   sipsess_refer_h *referh, sipsess_close_h *closeh,
		   sipsess_cancel_h *cancelh,
		   void *arg, const char *fmt, ...)
{
	struct sipsess *sess;
	va_list ap;
	int err;

	if (!sessp || !sock || !msg || scode < 101 || scode > 299 ||
	    !cuser || !ctype)
		return EINVAL;

	err = sipsess_alloc(&sess, sock, cuser, ctype, NULL, authh, aarg, aref,
			    NULL, offerh, answerh, NULL, estabh, infoh, referh,
			    closeh, cancelh, arg);
	if (err)
		return err;

	err = sip_dialog_accept(&sess->dlg, msg);
	if (err)
		goto out;

	hash_append(sock->ht_sess,
		    hash_joaat_str(sip_dialog_callid(sess->dlg)),
		    &sess->he, sess);

	sess->msg = mem_ref((void *)msg);

	err = sip_strans_alloc(&sess->st, sess->sip, msg, cancel_handler,
			       sess);
	if (err)
		goto out;

	if (mbuf_get_left(msg->mb))
		sess->neg_state = SDP_NEG_REMOTE_OFFER;

	va_start(ap, fmt);

	if (scode > 100 && scode < 200) {
		err = sipsess_reply_1xx(sess, msg, scode, reason, rel100, desc,
					fmt, &ap);
	}
	else if (scode >= 200) {
		err = sipsess_reply_2xx(sess, msg, scode, reason, desc,
					fmt, &ap);
	}

	va_end(ap);

	if (err)
		goto out;

 out:
	if (err)
		mem_deref(sess);
	else
		*sessp = sess;

	return err;
}


/**
 * Send progress response
 *
 * @param sess      SIP Session
 * @param scode     Response status code
 * @param reason    Response reason phrase
 * @param rel100    Sending 1xx reliably supported, required or disabled
 * @param desc      Content description (e.g. SDP)
 * @param fmt       Formatted strings with extra SIP Headers
 *
 * @return 0 if success, otherwise errorcode
 */
int sipsess_progress(struct sipsess *sess, uint16_t scode, const char *reason,
		     enum rel100_mode rel100, struct mbuf *desc,
		     const char *fmt, ...)
{
	va_list ap;
	int err;

	if (!sess || !sess->st || !sess->msg || scode < 101 || scode > 199)
		return EINVAL;

	va_start(ap, fmt);

	err = sipsess_reply_1xx(sess, sess->msg, scode, reason, rel100, desc,
				fmt, &ap);

	va_end(ap);

	return err;
}


/**
 * Answer an incoming SIP Session connection
 *
 * @param sess      SIP Session
 * @param scode     Response status code
 * @param reason    Response reason phrase
 * @param desc      Content description (e.g. SDP)
 * @param fmt       Formatted strings with extra SIP Headers
 *
 * @return 0 if success, otherwise errorcode
 */
int sipsess_answer(struct sipsess *sess, uint16_t scode, const char *reason,
		   struct mbuf *desc, const char *fmt, ...)
{
	va_list ap;
	int err;

	if (!sess || !sess->st || !sess->msg || scode < 200 || scode > 299)
		return EINVAL;

	va_start(ap, fmt);
	err = sipsess_reply_2xx(sess, sess->msg, scode, reason, desc,
				fmt, &ap);
	va_end(ap);

	return err;
}


/**
 * Reject an incoming SIP Session connection
 *
 * @param sess      SIP Session
 * @param scode     Response status code
 * @param reason    Response reason phrase
 * @param fmt       Formatted strings with extra SIP Headers
 *
 * @return 0 if success, otherwise errorcode
 */
int sipsess_reject(struct sipsess *sess, uint16_t scode, const char *reason,
		   const char *fmt, ...)
{
	va_list ap;
	int err;

	if (!sess || !sess->st || !sess->msg || scode < 300)
		return EINVAL;

	va_start(ap, fmt);
	err = sip_treplyf(&sess->st, NULL, sess->sip, sess->msg, false,
			  scode, reason, fmt ? "%v" : NULL, fmt, &ap);
	va_end(ap);

	return err;
}
