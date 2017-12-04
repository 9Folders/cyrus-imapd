/* http_jmap.h -- Routines for handling JMAP requests in httpd
 *
 * Copyright (c) 1994-2016 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef HTTP_JMAP_H
#define HTTP_JMAP_H

#include "auth.h"
#include "conversations.h"
#include "httpd.h"
#include "json_support.h"
#include "mailbox.h"
#include "mboxname.h"
#include "msgrecord.h"

#include "jmapauth.h"

extern struct namespace jmap_namespace;

struct jmap_idmap {
    struct hash_table mailboxes;
    struct hash_table messages;
    struct hash_table calendars;
    struct hash_table calendarevents;
    struct hash_table contactgroups;
    struct hash_table contacts;
};

typedef struct jmap_req {
    const char           *userid;
    const char           *accountid;
    const char           *inboxname;
    struct conversations_state *cstate;
    struct auth_state    *authstate;
    struct jmap_idmap    *idmap;
    json_t               *args;
    json_t               *response;
    const char           *tag;
    struct transaction_t *txn;
    struct mboxname_counters counters;

    /* Owned by JMAP HTTP handler */
    ptrarray_t *mboxes;
    int is_shared_account;
    hash_table *mboxrights;
} jmap_req_t;

typedef struct jmap_msg {
    const char *name;
    int (*proc)(struct jmap_req *req);
} jmap_msg_t;

/* Protocol implementations */
extern jmap_msg_t jmap_mail_messages[];
extern jmap_msg_t jmap_contact_messages[];
extern jmap_msg_t jmap_calendar_messages[];

/* Request-scoped mailbox cache */
extern int  jmap_openmbox(jmap_req_t *req, const char *name,
                          struct mailbox **mboxp, int rw);
extern int  jmap_isopenmbox(jmap_req_t *req, const char *name);
extern void jmap_closembox(jmap_req_t *req, struct mailbox **mboxp);

/* usermbox-like mailbox tree traversal, scoped by accountid.
 * Reports only active (not deleted) mailboxes. Checks presence
 * of ACL_LOOKUP for shared accounts. */
extern int  jmap_mboxlist(jmap_req_t *req, mboxlist_cb *proc, void *rock);

/* allmbox-like mailbox tree traversal, scoped by accountid.
 * Reports all mailboxes and does not check permissions. */
extern int  jmap_allmbox(jmap_req_t *req, mboxlist_cb *proc, void *rock);

/* Request-scoped cache of mailbox rights for authenticated user */

extern int  jmap_myrights(jmap_req_t *req, const mbentry_t *mbentry);
extern int  jmap_myrights_byname(jmap_req_t *req, const char *mboxname);
extern void jmap_myrights_delete(jmap_req_t *req, const char *mboxname);

/* Blob services */
extern int jmap_upload(struct transaction_t *txn);
extern int jmap_download(struct transaction_t *txn);
extern int jmap_findblob(jmap_req_t *req, const char *blobid,
                         struct mailbox **mbox, msgrecord_t **mr,
                         struct body **body, const struct body **part);
extern char *jmap_blobid(const struct message_guid *guid);

/* JMAP states */
extern json_t* jmap_getstate(jmap_req_t *req, int mbtype);
extern int jmap_bumpstate(jmap_req_t *req, int mbtype);
extern int jmap_cmpstate(jmap_req_t *req, json_t *state, int mbtype);
extern modseq_t jmap_highestmodseq(jmap_req_t *req, int mbtype);

/* Helpers for DAV-based JMAP types */
extern char *jmap_xhref(const char *mboxname, const char *resource);

#endif /* HTTP_JMAP_H */
