/* jmap_mail.c -- Routines for handling JMAP mail messages
 *
 * Copyright (c) 1994-2014 Carnegie Mellon University.  All rights reserved.
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

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <assert.h>
#include <sys/mman.h>

#include "acl.h"
#include "annotate.h"
#include "append.h"
#include "http_dav.h"
#include "http_jmap.h"
#include "http_proxy.h"
#include "json_support.h"
#include "mailbox.h"
#include "mboxlist.h"
#include "mboxname.h"
#include "msgrecord.h"
#include "parseaddr.h"
#include "proxy.h"
#include "search_query.h"
#include "statuscache.h"
#include "stristr.h"
#include "sync_log.h"
#include "times.h"
#include "util.h"
#include "xmalloc.h"
#include "xstrnchr.h"

/* generated headers are not necessarily in current directory */
#include "imap/http_err.h"
#include "imap/imap_err.h"

static int getMailboxes(jmap_req_t *req);
static int setMailboxes(jmap_req_t *req);
static int getMailboxUpdates(jmap_req_t *req);
static int getMessageList(jmap_req_t *req);
static int getMessages(jmap_req_t *req);
static int setMessages(jmap_req_t *req);
static int getMessageUpdates(jmap_req_t *req);
static int importMessages(jmap_req_t *req);
static int getSearchSnippets(jmap_req_t *req);
static int getThreads(jmap_req_t *req);
static int getIdentities(jmap_req_t *req);
static int getThreadUpdates(jmap_req_t *req);

/* TODO:
 * - copyMessages
 * - getVacationResponse
 * - setVacationResponse
 * - getIdentityUpdates
 * - setIdentities
 * - reportMessages
 */

jmap_msg_t jmap_mail_messages[] = {
    { "getMailboxes",           &getMailboxes },
    { "setMailboxes",           &setMailboxes },
    { "getMailboxUpdates",      &getMailboxUpdates },
    { "getMessageList",         &getMessageList },
    { "getMessages",            &getMessages },
    { "setMessages",            &setMessages },
    { "getMessageUpdates",      &getMessageUpdates },
    { "importMessages",         &importMessages },
    { "getSearchSnippets",      &getSearchSnippets },
    { "getThreads",             &getThreads },
    { "getThreadUpdates",       &getThreadUpdates },
    { "getIdentities",          &getIdentities },
    { NULL,                     NULL}
};

#define JMAP_INREPLYTO_HEADER "X-JMAP-In-Reply-To"
#define JMAP_HAS_ATTACHMENT_FLAG "$HasAttachment"

typedef enum MsgType {
        MSG_IS_ROOT = 0,
        MSG_IS_ATTACHED = 1,
} MsgType;

static int _wantprop(hash_table *props, const char *name)
{
    if (!props) return 1;
    return hash_lookup(name, props) != NULL;
}

static int JNOTNULL(json_t *item)
{
   if (!item) return 0;
   if (json_is_null(item)) return 0;
   return 1;
}

static int readprop_full(json_t *root, const char *prefix, const char *name,
                         int mandatory, json_t *invalid, const char *fmt,
                         void *dst)
{
    int r = 0;
    json_t *jval = json_object_get(root, name);
    if (!jval && mandatory) {
        r = -1;
    } else if (jval) {
        json_error_t err;
        if (!mandatory && json_is_null(jval)) {
            /* XXX not all non-mandatory properties are nullable */
            r = 0;
        }
        else if (json_unpack_ex(jval, &err, 0, fmt, dst)) {
            r = -2;
        }
        else {
            r = 1;
        }
    }
    if (r < 0 && prefix) {
        struct buf buf = BUF_INITIALIZER;
        buf_printf(&buf, "%s.%s", prefix, name);
        json_array_append_new(invalid, json_string(buf_cstring(&buf)));
        buf_free(&buf);
    } else if (r < 0) {
        json_array_append_new(invalid, json_string(name));
    }
    return r;
}

#define readprop(root, name,  mandatory, invalid, fmt, dst) \
    readprop_full((root), NULL, (name), (mandatory), (invalid), (fmt), (dst))

struct findmbox_data {
    const char *uniqueid;
    char **name;
};

static int findmbox_cb(const mbentry_t *mbentry, void *rock)
{
    struct findmbox_data *data = rock;
    if (strcmp(mbentry->uniqueid, data->uniqueid))
        return 0;
    *(data->name) = xstrdup(mbentry->name);
    return IMAP_OK_COMPLETED;
}

static char *jmapmbox_find_uniqueid(jmap_req_t *req, const char *id)
{
    char *name = NULL;
    struct findmbox_data rock = { id, &name };
    int r = jmap_mboxlist(req, findmbox_cb, &rock, 0);
    if (r != IMAP_OK_COMPLETED) {
        free(name);
        name = NULL;
    }
    return name;
}

struct find_specialuse_data {
    const char *use;
    const char *userid;
    char *mboxname;
};

static int find_specialuse_cb(const mbentry_t *mbentry, void *rock)
{
    struct find_specialuse_data *d = (struct find_specialuse_data *)rock;
    struct buf attrib = BUF_INITIALIZER;

    annotatemore_lookup(mbentry->name, "/specialuse", d->userid, &attrib);

    if (attrib.len) {
        strarray_t *uses = strarray_split(buf_cstring(&attrib), " ", 0);
        if (strarray_find_case(uses, d->use, 0) >= 0)
            d->mboxname = xstrdup(mbentry->name);
        strarray_free(uses);
    }

    buf_free(&attrib);

    if (d->mboxname) return CYRUSDB_DONE;
    return 0;
}


static char *jmapmbox_find_specialuse(jmap_req_t *req, const char *use)
{
    /* \\Inbox is magical */
    if (!strcasecmp(use, "\\Inbox"))
        return mboxname_user_mbox(req->accountid, NULL);

    struct find_specialuse_data rock = { use, req->userid, NULL };
    jmap_mboxlist(req, find_specialuse_cb, &rock, 0);
    return rock.mboxname;
}

static char *jmapmbox_role(jmap_req_t *req, const mbname_t *mbname)
{
    struct buf buf = BUF_INITIALIZER;
    const char *role = NULL;
    char *ret = NULL;

    /* Inbox is special. */
    if (!strarray_size(mbname_boxes(mbname)))
        return xstrdup("inbox");

    /* Is it an outbox? */
    if (mboxname_isoutbox(mbname_intname(mbname)))
        return xstrdup("outbox");

    /* XXX How to determine the templates role? */

    /* Does this mailbox have an IMAP special use role? */
    annotatemore_lookup(mbname_intname(mbname), "/specialuse", req->userid, &buf);
    if (buf.len) {
        strarray_t *uses = strarray_split(buf_cstring(&buf), " ", STRARRAY_TRIM);
        if (uses->count) {
            /* In IMAP, a mailbox may have multiple roles. But in JMAP we only
             * return the first specialuse flag. */
            const char *use = strarray_nth(uses, 0);
            if (!strcmp(use, "\\Archive")) {
                role = "archive";
            } else if (!strcmp(use, "\\Drafts")) {
                role = "drafts";
            } else if (!strcmp(use, "\\Junk")) {
                role = "spam";
            } else if (!strcmp(use, "\\Sent")) {
                role = "sent";
            } else if (!strcmp(use, "\\Trash")) {
                role = "trash";
            }
        }
        strarray_free(uses);
    }

    /* Otherwise, does it have the x-role annotation set? */
    if (!role) {
        buf_reset(&buf);
        annotatemore_lookup(mbname_intname(mbname), IMAP_ANNOT_NS "x-role", req->userid, &buf);
        if (buf.len) {
            role = buf_cstring(&buf);
        }
    }

    /* Make the caller own role. */
    if (role) ret = xstrdup(role);

    buf_free(&buf);
    return ret;
}

static char *jmapmbox_name(jmap_req_t *req, const mbname_t *mbname)
{
    struct buf attrib = BUF_INITIALIZER;

    int r = annotatemore_lookup(mbname_intname(mbname), IMAP_ANNOT_NS "displayname",
            req->userid, &attrib);
    if (!r && attrib.len) {
        /* We got a mailbox with a displayname annotation. Use it. */
        char *name = buf_release(&attrib);
        buf_free(&attrib);
        return name;
    }
    buf_free(&attrib);

    /* No displayname annotation. Most probably this mailbox was
     * created via IMAP. In any case, determine name from the the
     * last segment of the mailboxname hierarchy. */
    char *extname;

    const strarray_t *boxes = mbname_boxes(mbname);
    if (strarray_size(boxes)) {
        extname = xstrdup(strarray_nth(boxes, strarray_size(boxes)-1));
        /* Decode extname from IMAP UTF-7 to UTF-8. Or fall back to extname. */
        charset_t cs = charset_lookupname("imap-utf-7");
        char *decoded = charset_to_utf8(extname, strlen(extname), cs, ENCODING_NONE);
        if (decoded) {
            free(extname);
            extname = decoded;
        }
        charset_free(&cs);
    } else {
        extname = xstrdup("Inbox");
    }

    return extname;
}

static json_t *jmapmbox_from_mbentry(jmap_req_t *req,
                                     const mbentry_t *mbentry,
                                     hash_table *roles,
                                     hash_table *props)
{
    unsigned statusitems = STATUS_MESSAGES | STATUS_UNSEEN;
    struct statusdata sdata;
    int rights;
    int is_inbox = 0, parent_is_inbox = 0;
    int r;
    mbname_t *mbname = mbname_from_intname(mbentry->name);

    json_t *obj = NULL;

    /* Determine rights */
    rights = jmap_myrights(req, mbentry);

    /* INBOX requires special treatment */
    switch (strarray_size(mbname_boxes(mbname))) {
    case 0:
        is_inbox = 1;
        break;
    case 1:
        parent_is_inbox = 1;
        break;
    default:
        break;
    }

    char *role = jmapmbox_role(req, mbname);

    /* Lookup status. */
    r = status_lookup(mbname_intname(mbname), req->userid, statusitems, &sdata);
    if (r) goto done;

    /* Build JMAP mailbox response. */
    obj = json_pack("{}");
    json_object_set_new(obj, "id", json_string(mbentry->uniqueid));
    if (_wantprop(props, "name")) {
        char *name = jmapmbox_name(req, mbname);
        if (!name) goto done;
        json_object_set_new(obj, "name", json_string(name));
        free(name);
    }
    if (_wantprop(props, "mustBeOnlyMailbox")) {
        if (!strcmpsafe(role, "trash"))
            json_object_set_new(obj, "mustBeOnlyMailbox", json_true());
        else if (!strcmpsafe(role, "spam"))
            json_object_set_new(obj, "mustBeOnlyMailbox", json_true());
        else
            json_object_set_new(obj, "mustBeOnlyMailbox", json_false());
    }

    if (_wantprop(props, "mayReadItems")) {
        json_object_set_new(obj, "mayReadItems", json_boolean(rights & ACL_READ));
    }
    if (_wantprop(props, "mayAddItems")) {
        json_object_set_new(obj, "mayAddItems", json_boolean(rights & ACL_INSERT));
    }
    if (_wantprop(props, "mayRemoveItems")) {
        json_object_set_new(obj, "mayRemoveItems", json_boolean(rights & ACL_DELETEMSG));
    }
    if (_wantprop(props, "mayCreateChild")) {
        json_object_set_new(obj, "mayCreateChild", json_boolean(rights & ACL_CREATE));
    }

    if (_wantprop(props, "totalMessages")) {
        json_object_set_new(obj, "totalMessages", json_integer(sdata.messages));
    }
    if (_wantprop(props, "unreadMessages")) {
        json_object_set_new(obj, "unreadMessages", json_integer(sdata.unseen));
    }

    if (_wantprop(props, "totalThreads") || _wantprop(props, "unreadThreads")) {
        conv_status_t xconv = CONV_STATUS_INIT;
        if ((r = conversation_getstatus(req->cstate, mbname_intname(mbname), &xconv))) {
            syslog(LOG_ERR, "conversation_getstatus(%s): %s", mbname_intname(mbname),
                    error_message(r));
            goto done;
        }
        if (_wantprop(props, "totalThreads")) {
            json_object_set_new(obj, "totalThreads", json_integer(xconv.exists));
        }
        if (_wantprop(props, "unreadThreads")) {
            json_object_set_new(obj, "unreadThreads", json_integer(xconv.unseen));
        }
    }
    if (_wantprop(props, "mayRename") || _wantprop(props, "parentId")) {
        mbentry_t *parent = NULL;
        mboxlist_findparent(mbname_intname(mbname), &parent);
        if (_wantprop(props, "parentId")) {
            json_object_set_new(obj, "parentId", (is_inbox || parent_is_inbox || !parent) ?
                                json_null() : json_string(parent->uniqueid));
        }
        if (_wantprop(props, "mayRename")) {
            int mayRename = 0;
            if (!is_inbox && (rights & ACL_DELETEMBOX)) {
                int parent_rights = jmap_myrights(req, parent);
                mayRename = parent_rights & ACL_CREATE;
            }
            json_object_set_new(obj, "mayRename", json_boolean(mayRename));
        }
        mboxlist_entry_free(&parent);
    }
    if (_wantprop(props, "mayDelete")) {
        int mayDelete = (rights & ACL_DELETEMBOX) && !is_inbox;
        json_object_set_new(obj, "mayDelete", json_boolean(mayDelete));
    }
    if (_wantprop(props, "role")) {
        if (role && !hash_lookup(role, roles)) {
            /* In JMAP, only one mailbox have a role. First one wins. */
            json_object_set_new(obj, "role", json_string(role));
            hash_insert(role, (void*)1, roles);
        } else {
            json_object_set_new(obj, "role", json_null());
        }
    }
    if (_wantprop(props, "sortOrder")) {
        struct buf attrib = BUF_INITIALIZER;
        int sortOrder = 0;
        /* Ignore lookup errors here. */
        annotatemore_lookup(mbname_intname(mbname), IMAP_ANNOT_NS "sortOrder", req->userid, &attrib);
        if (attrib.len) {
            uint64_t t = str2uint64(buf_cstring(&attrib));
            if (t < INT_MAX) {
                sortOrder = (int) t;
            } else {
                syslog(LOG_ERR, "%s: bogus sortOrder annotation value", mbname_intname(mbname));
            }
        }
        else {
            /* calculate based on role.  From FastMail:
               [  1, '*Inbox',     'inbox',   0,    1, [ 'INBOX' ], { PreviewModeId => 2, MessagesPerPage => 20 } ],
               [  2, 'Trash',      'trash',   0,    7, [ 'Trash', 'Deleted Items' ], { HasEmpty => 1 } ],
               [  3, 'Sent',       'sent',    0,    5, [ 'Sent', 'Sent Items' ], { DefSort => 2 } ],
               [  4, 'Drafts',     'drafts',  0,    4, [ 'Drafts' ] ],
               [  5, '*User',      undef,     0,   10, [ ] ],
               [  6, 'Junk',       'spam',    0,    6, [ 'Spam', 'Junk Mail', 'Junk E-Mail' ], { HasEmpty => 1 } ],
               [  7, 'XChats',     undef,     0,    8, [ 'Chats' ] ],
               [  8, 'Archive',    'archive', 0,    3, [ 'Archive' ] ],
               [  9, 'XNotes',     undef,     1,   10, [ 'Notes' ] ],
               [ 10, 'XTemplates', undef,     0,    9, [ 'Templates' ] ],
               [ 12, '*Shared',    undef,     0, 1000, [ 'user' ] ],
               [ 11, '*Restored',  undef,     0, 2000, [ 'RESTORED' ] ],
              */

            if (!role)
                sortOrder = 10;
            else if (!strcmp(role, "inbox"))
                sortOrder = 1;
            else if (!strcmp(role, "outbox"))
                sortOrder = 2;
            else if (!strcmp(role, "archive"))
                sortOrder = 3;
            else if (!strcmp(role, "drafts"))
                sortOrder = 4;
            else if (!strcmp(role, "sent"))
                sortOrder = 5;
            else if (!strcmp(role, "spam"))
                sortOrder = 6;
            else if (!strcmp(role, "trash"))
                sortOrder = 7;
            else
                sortOrder = 8;
        }
        json_object_set_new(obj, "sortOrder", json_integer(sortOrder));
        buf_free(&attrib);
    }

done:
    if (r) {
        syslog(LOG_ERR, "jmapmbox_from_mbentry: %s", error_message(r));
    }
    free(role);
    mbname_free(&mbname);
    return obj;
}

static json_t *jmap_fmtstate(modseq_t modseq)
{
    struct buf buf = BUF_INITIALIZER;
    json_t *state = NULL;
    buf_printf(&buf, MODSEQ_FMT, modseq);
    state = json_string(buf_cstring(&buf));
    buf_free(&buf);
    return state;
}

struct getmailboxes_cb_data {
    jmap_req_t *req;
    json_t *list;
    hash_table *roles;
    hash_table *props;
    hash_table *ids;
    hash_table *want;
    size_t seen;
};

static int getmailboxes_cb(const mbentry_t *mbentry, void *rock)
{
    struct getmailboxes_cb_data *data = (struct getmailboxes_cb_data *) rock;
    json_t *list = (json_t *) data->list, *obj;
    jmap_req_t *req = data->req;
    int r = 0, rights;

    /* Don't list special-purpose mailboxes. */
    if ((mbentry->mbtype & MBTYPE_DELETED) ||
        (mbentry->mbtype & MBTYPE_MOVING) ||
        (mbentry->mbtype & MBTYPE_REMOTE) ||  /* XXX ?*/
        (mbentry->mbtype & MBTYPE_RESERVE) || /* XXX ?*/
        (mbentry->mbtype & MBTYPES_NONIMAP)) {
        goto done;
    }

    /* It's a legit mailbox, so count it */
    data->seen++;

    /* Do we need to process this mailbox? */
    if (data->want && !hash_lookup(mbentry->uniqueid, data->want))
        return 0;

    /* Are we done with looking up mailboxes by id? */
    if (data->want && !hash_numrecords(data->want))
        return IMAP_OK_COMPLETED;

    /* Check ACL on mailbox for current user */
    rights = jmap_myrights(req, mbentry);
    if ((rights & (ACL_LOOKUP | ACL_READ)) != (ACL_LOOKUP | ACL_READ)) {
        goto done;
    }

    /* Convert mbox to JMAP object. */
    obj = jmapmbox_from_mbentry(req, mbentry, data->roles, data->props);
    if (!obj) {
        syslog(LOG_INFO, "could not convert mailbox %s to JMAP", mbentry->name);
        r = IMAP_INTERNAL;
        goto done;
    }
    json_array_append_new(list, obj);

    /* Move this mailbox of the lookup list */
    if (data->want) {
        hash_del(mbentry->uniqueid, data->want);
    }

  done:
    return r;
}

static void getmailboxes_notfound(const char *id,
                                  void *data __attribute__((unused)),
                                  void *rock)
{
    json_t **listptr = rock;
    if (*listptr == json_null())
        *listptr = json_pack("[]");
    json_array_append_new(*listptr, json_string(id));
}

static int getMailboxes(jmap_req_t *req)
{
    json_t *found = json_pack("[]"), *notfound = json_null();
    struct getmailboxes_cb_data data = {
        req,
        found, /* list */
        (hash_table *) xmalloc(sizeof(hash_table)), /* roles */
        NULL, /* props */
        NULL, /* ids */
        NULL, /* want */
        0     /* seen */
    };
    construct_hash_table(data.roles, 8, 0);

    /* Determine which properties to fetch. */
    json_t *properties = json_object_get(req->args, "properties");
    if (properties && json_array_size(properties)) {
        int i;
        int size = json_array_size(properties);

        data.props = xzmalloc(sizeof(struct hash_table));
        construct_hash_table(data.props, size + 1, 0);
        for (i = 0; i < size; i++) {
            const char *pn = json_string_value(json_array_get(properties, i));
            if (pn == NULL) {
                json_t *err = json_pack("{s:s, s:[s]}",
                        "type", "invalidArguments", "arguments", "properties");
                json_array_append_new(req->response, json_pack("[s,o,s]",
                            "error", err, req->tag));
                goto done;
            }
            hash_insert(pn, (void*)1, data.props);
        }
    }

    /* Does the client request specific mailboxes? */
    json_t *want = json_object_get(req->args, "ids");
    if (JNOTNULL(want)) {
        /* Keep note of which mailboxes the client is interested in, but
         * don't look them up by id within the loop. We are better off to
         * use the same code as if the client didn't ask for any specific
         * mailboxes. As soon as there's no more work to do, getmailboxes_cb
         * will end traversal. */
        size_t i;
        json_t *val;
        data.want = (hash_table *) xmalloc(sizeof(hash_table));
        construct_hash_table(data.want, json_array_size(want) + 1, 0);
        json_array_foreach(want, i, val) {
            const char *id = json_string_value(val);
            if (id == NULL) {
                json_t *err = json_pack("{s:s, s:[s]}",
                        "type", "invalidArguments", "arguments", "ids");
                json_array_append_new(req->response, json_pack("[s,o,s]",
                            "error", err, req->tag));
                goto done;
            }
            if (id[0] == '#') {
                const char *newid = hash_lookup(id + 1, &req->idmap->mailboxes);
                if (!newid) {
                    if (notfound == json_null()) {
                        notfound = json_pack("[]");
                    }
                    json_array_append_new(notfound, json_string(id));
                    continue;
                }
                id = newid;
            }
            hash_insert(id, (void*) 1, data.want);
        }
    }

    /* Lookup and process the mailboxes */
    jmap_mboxlist(req, getmailboxes_cb, &data, 0);

   /* Report if any requested mailbox has not been found */
    if (data.want) {
        hash_enumerate(data.want, getmailboxes_notfound, &notfound);
    }

    /* Build response */
    json_t *item = json_pack("[s {s:s s:O s:O s:o} s]",
                     "mailboxes",
                     "accountId", req->accountid,
                     "list", found,
                     "notFound", notfound,
                     "state", jmap_getstate(req, 0/*mbtype*/),
                     req->tag);
    json_array_append_new(req->response, item);

done:
    json_decref(found);
    json_decref(notfound);
    if (data.roles) {
        free_hash_table(data.roles, NULL);
        free(data.roles);
    }
    if (data.props) {
        free_hash_table(data.props, NULL);
        free(data.props);
    }
    if (data.want) {
        free_hash_table(data.want, NULL);
        free(data.want);
    }
    return 0;
}

struct jmapmbox_newname_data {
    const char *mboxname;
    int highest;
    size_t len;
};

static int jmapmbox_newname_cb(const mbentry_t *mbentry, void *rock) {
    struct jmapmbox_newname_data *data = (struct jmapmbox_newname_data *) rock;
    const char *s, *lo, *hi;
    int n;

    if (!data->len) {
        data->len = strlen(data->mboxname);
        assert(data->len > 0);
    }
    if (strncmp(mbentry->name, data->mboxname, data->len)) {
        return 0;
    }

    /* Skip any grand-children. */
    s = mbentry->name + data->len;
    if (strchr(s, jmap_namespace.hier_sep)) {
        return 0;
    }

    /* Does this mailbox match exactly our mboxname? */
    if (*s == 0) {
        data->highest = 1;
        return 0;
    }

    /* If it doesn't end with pattern "_\d+", skip it. */
    if (*s++ != '_') {
        return 0;
    }

    /* Parse _\d+$ pattern */
    hi = lo = s;
    while (isdigit(*s++)) {
        hi++;
    }
    if (lo == hi || *hi != 0){
        return 0;
    }

    if ((n = atoi(lo)) && n > data->highest) {
        data->highest = n;
    }

    return 0;
}

/* Combine the UTF-8 encoded JMAP mailbox name and its parent IMAP mailbox
 * name to a unique IMAP mailbox name.
 *
 * Parentname must already be encoded in IMAP UTF-7. A parent by this name
 * must already exist. If a mailbox with the combined mailbox name already
 * exists, the new mailbox name is made unique to avoid IMAP name collisions.
 *
 * For example, if the name has been determined to be x and a mailbox with
 * this name already exists, then look for all mailboxes named x_\d+. The
 * new mailbox name will be x_<max+1> with max being he highest number found
 * for any such named mailbox.
 *
 * Return the malloced, combined name, or NULL on error. */
char *jmapmbox_newname(const char *name, const char *parentname)
{
    charset_t cs = CHARSET_UNKNOWN_CHARSET;
    char *mboxname = NULL;
    int r;

    cs = charset_lookupname("utf-8");
    if (cs == CHARSET_UNKNOWN_CHARSET) {
        /* huh? */
        syslog(LOG_INFO, "charset utf-8 is unknown");
        goto done;
    }

    /* Encode mailbox name in IMAP UTF-7 */
    char *s = charset_to_imaputf7(name, strlen(name), cs, ENCODING_NONE);
    if (!s) {
        syslog(LOG_ERR, "Could not convert mailbox name to IMAP UTF-7.");
        goto done;
    }
    mbname_t *mbname = mbname_from_intname(parentname);
    mbname_push_boxes(mbname, s);
    free(s);
    mboxname = xstrdup(mbname_intname(mbname));
    mbname_free(&mbname);

    /* Avoid any name collisions */
    struct jmapmbox_newname_data rock;
    memset(&rock, 0, sizeof(struct jmapmbox_newname_data));
    rock.mboxname = mboxname;
    r = mboxlist_mboxtree(parentname, &jmapmbox_newname_cb, &rock,
                          MBOXTREE_SKIP_ROOT);
    if (r) {
        syslog(LOG_ERR, "mboxlist_mboxtree(%s): %s",
                parentname, error_message(r));
        free(mboxname);
        mboxname = NULL;
        goto done;
    }
    if (rock.highest) {
        struct buf buf = BUF_INITIALIZER;
        buf_printf(&buf, "%s_%d", mboxname, rock.highest + 1);
        free(mboxname);
        mboxname = buf_newcstring(&buf);
        buf_free(&buf);
    }

done:
    charset_free(&cs);
    return mboxname;
}

struct jmapmbox_findxrole_data {
    const char *xrole;
    const char *userid;
    char *mboxname;
};

static int jmapmbox_findxrole_cb(const mbentry_t *mbentry, void *rock)
{
    struct jmapmbox_findxrole_data *d = (struct jmapmbox_findxrole_data *)rock;
    struct buf attrib = BUF_INITIALIZER;

    annotatemore_lookup(mbentry->name, IMAP_ANNOT_NS "x-role", d->userid, &attrib);

    if (attrib.len && !strcmp(buf_cstring(&attrib), d->xrole)) {
        d->mboxname = xstrdup(mbentry->name);
    }

    buf_free(&attrib);

    if (d->mboxname) return CYRUSDB_DONE;
    return 0;
}

static char *jmapmbox_findxrole(jmap_req_t *req, const char *xrole)
{
    struct jmapmbox_findxrole_data rock = { xrole, req->userid, NULL };
    /* INBOX can never have an x-role. */
    jmap_mboxlist(req, jmapmbox_findxrole_cb, &rock, 0);
    return rock.mboxname;
}

static int jmapmbox_isparent_cb(const mbentry_t *mbentry __attribute__ ((unused)), void *rock) {
    int *has_child = (int *) rock;
    *has_child = 1;
    return IMAP_OK_COMPLETED;
}

static int jmapmbox_isparent(const char *mboxname)
{
    int has_child = 0;
    mboxlist_mboxtree(mboxname, jmapmbox_isparent_cb, &has_child, MBOXTREE_SKIP_ROOT);
    return has_child;
}

struct setmailboxes_args {
    const char *id;
    const char *name;
    char *parentid;
    const char *role;
    const char *specialuse;
    int sortorder;
};

static void setmailboxes_free_args(struct setmailboxes_args *args)
{
    free(args->parentid);
}

static void setmailboxes_read_args(jmap_req_t *req,
                                   json_t *jargs,
                                   struct setmailboxes_args *args,
                                   int is_create,
                                   json_t *invalid)
{
    int pe;

    /* Initialize arguments */
    memset(args, 0, sizeof(struct setmailboxes_args));
    args->sortorder = -1;

    /* id */
    pe = readprop(jargs, "id", 0, invalid, "s", &args->id);
    if (pe > 0 && is_create) {
        json_array_append_new(invalid, json_string("id"));
    }

    /* name */
    pe = readprop(jargs, "name", is_create, invalid, "s", &args->name);
    if (pe > 0 && !(*(args->name))) {
        json_array_append_new(invalid, json_string("name"));
    }

    /* parentId */
    json_t *jparentid = json_object_get(jargs, "parentId");
    if (JNOTNULL(jparentid)) {
        pe = readprop(jargs, "parentId", 1, invalid, "s", &args->parentid);
        if (pe > 0 && *(args->parentid) == '#') {
            args->parentid = hash_lookup(args->parentid + 1, &req->idmap->mailboxes);
            if (!args->parentid) {
                json_array_append_new(invalid, json_string("parentId"));
            }
        }
        if (args->parentid) {
            args->parentid = xstrdup(args->parentid);
        }
    } else if (jparentid == json_null()) {
        mbentry_t *inboxentry = NULL;
        mboxlist_lookup(req->inboxname, &inboxentry, NULL);
        args->parentid = xstrdup(inboxentry->uniqueid);
        mboxlist_entry_free(&inboxentry);
    } else if (is_create && !jparentid) {
        json_array_append_new(invalid, json_string("parentId"));
    }
    if (args->parentid) {
        char *tmp = jmapmbox_find_uniqueid(req, args->parentid);
        if (!tmp) json_array_append_new(invalid, json_string("parentId"));
        free(tmp);
    }

    /* role */
    if (JNOTNULL(json_object_get(jargs, "role"))) {
        pe = readprop(jargs, "role", is_create, invalid, "s", &args->role);
        if (pe > 0 && (!is_create || !strcmp(args->role, "inbox"))) {
            /* Roles are immutable and inbox must not be created */
            json_array_append_new(invalid, json_string("role"));
        }
    }

    /* sortOrder */
    if (readprop(jargs, "sortOrder", 0, invalid, "i", &args->sortorder) > 0) {
        if (args->sortorder < 0 || args->sortorder >= INT_MAX) {
            json_array_append_new(invalid, json_string("sortOrder"));
        }
    }

    /* mayXXX. These are immutable, but we ignore them during update. */
    if (json_object_get(jargs, "mustBeOnlyMailbox") && is_create) {
        json_array_append_new(invalid, json_string("mustBeOnlyMailbox"));
    }
    if (json_object_get(jargs, "mayReadItems") && is_create) {
        json_array_append_new(invalid, json_string("mayReadItems"));
    }
    if (json_object_get(jargs, "mayAddItems") && is_create) {
        json_array_append_new(invalid, json_string("mayAddItems"));
    }
    if (json_object_get(jargs, "mayRemoveItems") && is_create) {
        json_array_append_new(invalid, json_string("mayRemoveItems"));
    }
    if (json_object_get(jargs, "mayRename") && is_create) {
        json_array_append_new(invalid, json_string("mayRename"));
    }
    if (json_object_get(jargs, "mayDelete") && is_create) {
        json_array_append_new(invalid, json_string("mayDelete"));
    }
    if (json_object_get(jargs, "totalMessages") && is_create) {
        json_array_append_new(invalid, json_string("totalMessages"));
    }
    if (json_object_get(jargs, "unreadMessages") && is_create) {
        json_array_append_new(invalid, json_string("unreadMessages"));
    }
    if (json_object_get(jargs, "totalThreads") && is_create) {
        json_array_append_new(invalid, json_string("totalThreads"));
    }
    if (json_object_get(jargs, "unreadThreads") && is_create) {
        json_array_append_new(invalid, json_string("unreadThreads"));
    }
}

static int setmailboxes_write_annots(jmap_req_t *req,
                                     struct setmailboxes_args *args,
                                     const char *mboxname)
{
    int r = 0;
    struct buf buf = BUF_INITIALIZER;

    if (args->name) {
        /* Set displayname annotation on mailbox. */
        buf_setcstr(&buf, args->name);
        static const char *displayname_annot = IMAP_ANNOT_NS "displayname";
        r = annotatemore_write(mboxname, displayname_annot, req->userid, &buf);
        if (r) {
            syslog(LOG_ERR, "failed to write annotation %s: %s",
                    displayname_annot, error_message(r));
            goto done;
        }
        buf_reset(&buf);
    }

    /* Set specialuse or x-role. specialuse takes precedence. */
    if (args->specialuse) {
        buf_setcstr(&buf, args->specialuse);
        static const char *annot = "/specialuse";
        r = annotatemore_write(mboxname, annot, req->userid, &buf);
        if (r) {
            syslog(LOG_ERR, "failed to write annotation %s: %s",
                    annot, error_message(r));
            goto done;
        }
        buf_reset(&buf);
    }
    else if (args->role) {
        buf_setcstr(&buf, args->role);
        static const char *annot = IMAP_ANNOT_NS "x-role";
        r = annotatemore_write(mboxname, annot, req->userid, &buf);
        if (r) {
            syslog(LOG_ERR, "failed to write annotation %s: %s",
                    annot, error_message(r));
            goto done;
        }
        buf_reset(&buf);
    }

    if (args->sortorder >= 0) {
        /* Set sortOrder annotation on mailbox. */
        buf_printf(&buf, "%d", args->sortorder);
        static const char *sortorder_annot = IMAP_ANNOT_NS "sortOrder";
        r = annotatemore_write(mboxname, sortorder_annot, req->userid, &buf);
        if (r) {
            syslog(LOG_ERR, "failed to write annotation %s: %s",
                    sortorder_annot, error_message(r));
            goto done;
        }
    }

done:
    buf_free(&buf);
    return r;
}

static int setmailboxes_create(jmap_req_t *req,
                               struct setmailboxes_args *args,
                               json_t *invalid,
                               char **mboxid,
                               json_t **err __attribute__((unused)))
{
    char *mboxname = NULL, *parentname = NULL;
    int r = 0, rights = 0;
    mbentry_t *mbinbox = NULL, *mbparent = NULL, *mbentry = NULL;

    mboxlist_lookup(req->inboxname, &mbinbox, NULL);

    /* Check parent ACL */
    parentname = jmapmbox_find_uniqueid(req, args->parentid);
    if (!parentname) {
        json_array_append_new(invalid, json_string("parentId"));
        goto done;
    }
    r = mboxlist_lookup(parentname, &mbparent, NULL);
    if (r) {
        syslog(LOG_ERR, "failed to lookup parent mailbox %s: %s",
                parentname, error_message(r));
        goto done;
    }
    rights = jmap_myrights(req, mbparent);
    if (!(rights & ACL_CREATE)) {
        json_array_append_new(invalid, json_string("parentId"));
        goto done;
    }

    /* Sanity-check arguments */
    if (args->role) {
        if (!strcmp(args->role, "outbox")) {
            if (strcmp(args->parentid, mbinbox->uniqueid)) {
                /* Outbox may only be created on top-level. */
                json_array_append_new(invalid, json_string("role"));
            }
            else {
                /* Check that no outbox exists. */
                /* XXX mboxname_isoutbox checks for top-level mailbox 'Outbox' */
                char *outboxname = mboxname_user_mbox(req->accountid, "Outbox");
                mbentry_t *mbentry = NULL;
                if (mboxlist_lookup(outboxname, &mbentry, NULL) != IMAP_MAILBOX_NONEXISTENT)
                    json_array_append_new(invalid, json_string("role"));
                if (mbentry) mboxlist_entry_free(&mbentry);
                free(outboxname);
            }
        } else {
            /* Is it one of the known special use mailboxes? */
            if (!strcmp(args->role, "archive")) {
                args->specialuse = "\\Archive";
            } else if (!strcmp(args->role, "drafts")) {
                args->specialuse = "\\Drafts";
            } else if (!strcmp(args->role, "spam")) {
                args->specialuse = "\\Junk";
            } else if (!strcmp(args->role, "sent")) {
                args->specialuse = "\\Sent";
            } else if (!strcmp(args->role, "trash")) {
                args->specialuse = "\\Trash";
            } else if (strncmp(args->role, "x-", 2)) {
                /* Does it start with an "x-"? If not, reject it. */
                json_array_append_new(invalid, json_string("role"));
            }
        }
        char *exists = NULL;
        if (args->specialuse) {
            /* Check that no such IMAP specialuse mailbox already exists. */
            exists = jmapmbox_find_specialuse(req, args->specialuse);
        } else if (!json_array_size(invalid)) {
            /* Check that no mailbox with this x-role exists. */
            exists = jmapmbox_findxrole(req, args->role);
        }
        if (exists) {
            json_array_append_new(invalid, json_string("role"));
        }
        free(exists);
    }
    if (json_array_size(invalid)) {
        r = 0;
        goto done;
    }

    /* Determine name for the soon-to-be created mailbox. */
    if (args->role && !strcmp(args->role, "outbox")) {
        /* XXX mboxname_isoutbox checks for top-level mailbox 'Outbox' */
        mboxname = mboxname_user_mbox(req->accountid, "Outbox");
    } else {
        /* Encode the mailbox name for IMAP. */
        mboxname = jmapmbox_newname(args->name, parentname);
        if (!mboxname) {
            syslog(LOG_ERR, "could not encode mailbox name");
            r = IMAP_INTERNAL;
            goto done;
        }
    }

    /* Create mailbox using parent ACL */
    r = mboxlist_createsync(mboxname, 0 /* MBTYPE */,
            NULL /* partition */,
            req->userid, req->authstate,
            0 /* options */, 0 /* uidvalidity */,
            0 /* highestmodseq */, mbparent->acl,
            NULL /* uniqueid */, 0 /* local_only */,
            NULL /* mboxptr */);
    if (r) {
        syslog(LOG_ERR, "IOERROR: failed to create %s (%s)",
                mboxname, error_message(r));
        goto done;
    }

    /* Write annotations */
    r = setmailboxes_write_annots(req, args, mboxname);
    if (r) goto done;

    /* Lookup and return the new mailbox id */
    r = mboxlist_lookup(mboxname, &mbentry, NULL);
    if (r) goto done;
    *mboxid = xstrdup(mbentry->uniqueid);

done:
    free(mboxname);
    free(parentname);
    mboxlist_entry_free(&mbinbox);
    mboxlist_entry_free(&mbparent);
    mboxlist_entry_free(&mbentry);

    return r;
}

static int setmailboxes_update(jmap_req_t *req,
                               struct setmailboxes_args *args,
                               json_t *invalid,
                               const char *mboxid,
                               json_t **err)
{
    char *mboxname = NULL, *parentname = NULL;
    int r = 0, rights = 0;
    mbentry_t *mbinbox = NULL, *mbentry = NULL, *mbparent = NULL;
    mboxlist_lookup(req->inboxname, &mbinbox, NULL);

    /* Sanity-check arguments */
    if (args->id && strcmp(args->id, mboxid)) {
        json_array_append_new(invalid, json_string("id"));
    }
    if (json_array_size(invalid)) return 0;

    /* Determine current mailbox and parent names */
    if (strcmp(mboxid, mbinbox->uniqueid)) {
        mboxname = jmapmbox_find_uniqueid(req, mboxid);
        if (!mboxname) {
            *err = json_pack("{s:s}", "type", "notFound");
            goto done;
        }
        r = mboxlist_findparent(mboxname, &mbparent);
        if (r) {
            syslog(LOG_INFO, "mboxlist_findparent(%s) failed: %s",
                    mboxname, error_message(r));
            goto done;
        }
        parentname = xstrdup(mbparent->name);
    } else {
        parentname = NULL;
        mboxname = xstrdup(mbinbox->name);
        mboxlist_lookup(mboxname, &mbparent, NULL);
    }
    mboxlist_lookup(mboxname, &mbentry, NULL);

    /* Check ACL */
    rights = jmap_myrights(req, mbentry);
    if (!(rights & ACL_WRITE)) {
        *err = json_pack("{s:s}", "type", "readOnly");
        goto done;
    }

    /* Do we need to move this mailbox to a new parent? */
    int force_rename = 0;

    if (args->parentid) {
        /* Compare old parent with new parent. */
        char *newparentname = NULL;
        if (strcmpsafe(args->parentid, mbinbox->uniqueid)) {
            newparentname = jmapmbox_find_uniqueid(req, args->parentid);
        } else {
            newparentname = xstrdup(mbinbox->name);
        }
        if (!newparentname) {
            json_array_append_new(invalid, json_string("parentId"));
            goto done;
        }

        /* Is this a move ot a new parent? */
        if (strcmpsafe(parentname, newparentname)) {
            /* Check ACL of mailbox */
            if (!(rights & ACL_DELETEMBOX)) {
                *err = json_pack("{s:s}", "type", "readOnly");
                goto done;
            }

            /* Reset pointers to parent */
            free(parentname);
            mboxlist_entry_free(&mbparent);
            parentname = newparentname;
            mboxlist_lookup(mboxname, &mbparent, NULL);
            force_rename = 1;

            /* Check ACL of new parent */
            int parent_rights = jmap_myrights(req, mbparent);
            if (!(parent_rights & ACL_CREATE)) {
                json_array_append_new(invalid, json_string("parentId"));
                goto done;
            }
        } else {
            free(newparentname);
        }
    }

    /* Do we need to rename the mailbox? But only if it isn't the INBOX! */
    if ((args->name || force_rename) && strcmpsafe(mboxname, mbinbox->name)) {
        mbname_t *mbname = mbname_from_intname(mboxname);
        char *oldname = jmapmbox_name(req, mbname);
        mbname_free(&mbname);
        char *name = xstrdup(args->name ? args->name : oldname);

        /* Do old and new mailbox names differ? */
        if (force_rename || strcmpsafe(oldname, name)) {
            char *newmboxname, *oldmboxname;

            /* Determine the unique IMAP mailbox name. */
            newmboxname = jmapmbox_newname(name, parentname);
            if (!newmboxname) {
                syslog(LOG_ERR, "jmapmbox_newname returns NULL: can't rename %s", mboxname);
                r = IMAP_INTERNAL;
                free(oldname);
                goto done;
            }
            oldmboxname = mboxname;

            /* Rename the mailbox. */
            struct mboxevent *mboxevent = mboxevent_new(EVENT_MAILBOX_RENAME);
            r = mboxlist_renamemailbox(oldmboxname, newmboxname,
                    NULL /* partition */, 0 /* uidvalidity */,
                    httpd_userisadmin, req->userid, httpd_authstate,
                    mboxevent,
                    0 /* local_only */, 0 /* forceuser */, 0 /* ignorequota */);
            mboxevent_free(&mboxevent);
            if (r) {
                syslog(LOG_ERR, "mboxlist_renamemailbox(old=%s new=%s): %s",
                        oldmboxname, newmboxname, error_message(r));
                free(newmboxname);
                free(oldname);
                goto done;
            }
            free(oldmboxname);
            mboxname = newmboxname;
        }
        free(oldname);
        free(name);
    }

    /* Write annotations */
    r = setmailboxes_write_annots(req, args, mboxname);
    if (r) goto done;

done:
    free(mboxname);
    free(parentname);
    mboxlist_entry_free(&mbentry);
    mboxlist_entry_free(&mbinbox);
    mboxlist_entry_free(&mbparent);

    return r;
}

static int setmailboxes_destroy(jmap_req_t *req,
                                const char *mboxid,
                                json_t **err)
{
    int r = 0, rights = 0;
    char *mboxname = NULL;
    mbentry_t *mbinbox = NULL, *mbentry = NULL;
    mboxlist_lookup(req->inboxname, &mbinbox, NULL);

    /* Do not allow to remove INBOX. */
    if (!strcmpsafe(mboxid, mbinbox->uniqueid)) {
        *err = json_pack("{s:s}", "type", "forbidden");
        goto done;
    }

    /* Lookup mailbox by id. */
    mboxname = jmapmbox_find_uniqueid(req, mboxid);
    if (!mboxname) {
        *err = json_pack("{s:s}", "type", "notFound");
        goto done;
    }

    /* Check ACL */
    mboxlist_lookup(mboxname, &mbentry, NULL);
    rights = jmap_myrights(req, mbentry);
    if (!(rights & ACL_DELETEMBOX)) {
        *err = json_pack("{s:s}", "type", "forbidden");
        goto done;
    }

    /* Check if the mailbox has any children. */
    if (jmapmbox_isparent(mboxname)) {
        *err = json_pack("{s:s}", "type", "mailboxHasChild");
        goto done;
    }

    /* Check if the mailbox has any messages */
    struct mailbox *mbox = NULL;
    struct mailbox_iter *iter = NULL;

    r = jmap_openmbox(req, mboxname, &mbox, 0);
    if (r) goto done;
    iter = mailbox_iter_init(mbox, 0, ITER_SKIP_EXPUNGED);
    if (mailbox_iter_step(iter) != NULL)
        *err = json_pack("{s:s}", "type", "mailboxHasMessage");
    mailbox_iter_done(&iter);
    jmap_closembox(req, &mbox);
    if (*err) goto done;

    /* Destroy mailbox. */
    struct mboxevent *mboxevent = mboxevent_new(EVENT_MAILBOX_DELETE);
    if (mboxlist_delayed_delete_isenabled()) {
        r = mboxlist_delayed_deletemailbox(mboxname,
                httpd_userisadmin || httpd_userisproxyadmin,
                req->userid, req->authstate, mboxevent,
                1 /* checkacl */, 0 /* local_only */, 0 /* force */);
    }
    else {
        r = mboxlist_deletemailbox(mboxname,
                httpd_userisadmin || httpd_userisproxyadmin,
                req->userid, req->authstate, mboxevent,
                1 /* checkacl */, 0 /* local_only */, 0 /* force */);
    }
    mboxevent_free(&mboxevent);
    if (r == IMAP_PERMISSION_DENIED) {
        *err = json_pack("{s:s}", "type", "forbidden");
        r = 0;
        goto done;
    }
    else if (r == IMAP_MAILBOX_NONEXISTENT) {
        *err = json_pack("{s:s}", "type", "notFound");
        r = 0;
        goto done;
    }
    else if (r) {
        syslog(LOG_ERR, "failed to delete mailbox(%s): %s",
                mboxname, error_message(r));
        goto done;
    }
    jmap_myrights_delete(req, mboxname);

done:
    mboxlist_entry_free(&mbinbox);
    mboxlist_entry_free(&mbentry);
    free(mboxname);
    return r;
}

static int setMailboxes(jmap_req_t *req)
{
    int r = 0;
    json_t *set = NULL;
    char *mboxname = NULL;
    char *parentname = NULL;
    json_t *state, *create, *update, *destroy;
    int bump_modseq = 0;

    mbentry_t *inboxentry = NULL;
    mboxlist_lookup(req->inboxname, &inboxentry, NULL);

    state = json_object_get(req->args, "ifInState");
    if (JNOTNULL(state)) {
        if (jmap_cmpstate(req, state, 0/*mbtype*/)) {
            json_array_append_new(req->response, json_pack("[s, {s:s}, s]",
                        "error", "type", "stateMismatch", req->tag));
            goto done;
        }
    }
    set = json_pack("{s:s}", "accountId", req->accountid);
    json_object_set_new(set, "oldState", state);

    create = json_object_get(req->args, "create");
    if (create) {
        json_t *created = json_pack("{}");
        json_t *notCreated = json_pack("{}");
        const char *key;
        json_t *arg;

        strarray_t todo = STRARRAY_INITIALIZER;

        /* sort keys topologically */
        json_object_foreach(create, key, arg) {
            /* Validate key. */
            if (!strlen(key)) {
                json_t *err= json_pack("{s:s}", "type", "invalidArguments");
                json_object_set_new(notCreated, key, err);
                continue;
            }

            strarray_append(&todo, key);
        }

        while (strarray_size(&todo)) {
            int didsome = 0;
            int i;

            for (i = 0; i < strarray_size(&todo); i++) {
                key = strarray_nth(&todo, i);
                arg = json_object_get(create, key);

                // check that parentId reference exists
                // FIXME -- why?
                const char *parentId = json_string_value(json_object_get(arg, "parentId"));
                if (parentId && *parentId == '#' && !hash_lookup(parentId + 1, &req->idmap->mailboxes))
                    continue;

                didsome = 1;

                json_t *invalid = json_pack("[]");
                char *uid = NULL;
                json_t *err = NULL;
                struct setmailboxes_args args;

                /* Process arguments */
                setmailboxes_read_args(req, arg, &args, 1, invalid);
                if (!json_array_size(invalid)) {
                    r = setmailboxes_create(req, &args, invalid, &uid, &err);
                }
                setmailboxes_free_args(&args);

                /* Handle errors */
                if (r) {
                    goto done;
                }
                else if (err) {
                    json_object_set_new(notCreated, key, err);
                    json_decref(invalid);
                    free(strarray_remove(&todo, i--));
                    continue;
                } else if (json_array_size(invalid)) {
                    json_t *err = json_pack("{s:s, s:o}",
                            "type", "invalidProperties", "properties", invalid);
                    json_object_set_new(notCreated, key, err);
                    free(strarray_remove(&todo, i--));
                    continue;
                }
                json_decref(invalid);

                /* Done */
                json_object_set_new(created, key, json_pack("{s:s}", "id", uid));
                hash_insert(key, uid, &req->idmap->mailboxes);
                free(strarray_remove(&todo, i--));
            }

            if (!didsome)
                return IMAP_INTERNAL; // XXX - nice error for missing parent?
        }

        if (json_object_size(created)) {
            json_object_set(set, "created", created);
            bump_modseq = 1;
        }
        json_decref(created);

        if (json_object_size(notCreated)) {
            json_object_set(set, "notCreated", notCreated);
        }
        json_decref(notCreated);

        strarray_fini(&todo);
    }

    update = json_object_get(req->args, "update");
    if (update) {
        json_t *updated = json_pack("{}");
        json_t *notUpdated = json_pack("{}");
        const char *uid;
        json_t *arg;

        json_object_foreach(update, uid, arg) {
            json_t *invalid = json_pack("[]");
            json_t *err = NULL;
            struct setmailboxes_args args;

            /* Process arguments */
            setmailboxes_read_args(req, arg, &args, 0, invalid);
            if (!json_array_size(invalid)) {
                r = setmailboxes_update(req, &args, invalid, uid, &err);
            }
            setmailboxes_free_args(&args);

            /* Handle errors */
            if (r) {
                goto done;
            }
            else if (err) {
                json_object_set_new(notUpdated, uid, err);
                json_decref(invalid);
                continue;
            }
            else if (json_array_size(invalid)) {
                json_t *err = json_pack("{s:s, s:o}",
                        "type", "invalidProperties", "properties", invalid);
                json_object_set_new(notUpdated, uid, err);
                continue;
            }
            json_decref(invalid);

            /* Done */
            json_object_set_new(updated, uid, json_null());
        }

        if (json_object_size(updated)) {
            json_object_set(set, "updated", updated);
            bump_modseq = 1;
        }
        json_decref(updated);

        if (json_object_size(notUpdated)) {
            json_object_set(set, "notUpdated", notUpdated);
        }
        json_decref(notUpdated);
    }

    destroy = json_object_get(req->args, "destroy");
    if (destroy) {
        json_t *destroyed = json_pack("[]");
        json_t *notDestroyed = json_pack("{}");

        size_t index;
        json_t *juid;
        json_array_foreach(destroy, index, juid) {

            /* Validate uid. */
            const char *uid = json_string_value(juid);
            if (!uid) {
                continue;
            }
            if (uid && uid[0] == '#') {
                const char *newuid = hash_lookup(uid + 1, &req->idmap->mailboxes);
                if (!newuid) {
                    json_t *err = json_pack("{s:s}", "type", "notFound");
                    json_object_set_new(notDestroyed, uid, err);
                    continue;
                }
                uid = newuid;
            }

            json_t *err = NULL;
            r = setmailboxes_destroy(req, uid, &err);
            if (r)  {
                goto done;
            }
            else if (err) {
                json_object_set_new(notDestroyed, uid, err);
                continue;
            }

            /* Report mailbox as destroyed. */
            json_array_append_new(destroyed, json_string(uid));
        }
        if (json_array_size(destroyed)) {
            json_object_set(set, "destroyed", destroyed);
            bump_modseq = 1;
        }
        json_decref(destroyed);
        if (json_object_size(notDestroyed)) {
            json_object_set(set, "notDestroyed", notDestroyed);
        }
        json_decref(notDestroyed);
    }

    if (bump_modseq) jmap_bumpstate(req, 0);
    json_object_set_new(set, "newState", jmap_getstate(req, 0/*mbtype*/));

    json_incref(set);
    json_t *item = json_pack("[]");
    json_array_append_new(item, json_string("mailboxesSet"));
    json_array_append_new(item, set);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

done:
    free(mboxname);
    free(parentname);
    mboxlist_entry_free(&inboxentry);
    if (set) json_decref(set);
    return r;
}

struct jmapmbox_updates_data {
    json_t *changed;        /* maps mailbox ids to {id:foldermodseq} */
    json_t *removed;        /* maps mailbox ids to {id:foldermodseq} */
    modseq_t frommodseq;
    int *only_counts_changed;
    jmap_req_t *req;
};

static int jmapmbox_updates_cb(const mbentry_t *mbentry, void *rock)
{
    struct jmapmbox_updates_data *data = rock;
    json_t *updates, *update;
    struct statusdata sdata;
    modseq_t modseq, mbmodseq;

    /* Ignore anything but regular mailboxes */
    if (mbentry->mbtype & ~(MBTYPE_DELETED)) {
        return 0;
    }

    /* Lookup status. */
    if (!(mbentry->mbtype & MBTYPE_DELETED)) {
        int r = status_lookup(mbentry->name, data->req->userid,
                              STATUS_HIGHESTMODSEQ, &sdata);
        if (r) return r;
        mbmodseq = sdata.highestmodseq;
    } else {
        mbmodseq = mbentry->foldermodseq;
    }

    /* Ignore old changes */
    if (mbmodseq <= data->frommodseq) {
        return 0;
    }

    /* Is this a more recent update for an id that we have already seen? */
    if ((update = json_object_get(data->removed, mbentry->uniqueid))) {
        modseq = (modseq_t)json_integer_value(json_object_get(update, "modseq"));
        if (modseq <= mbmodseq) {
            json_object_del(data->removed, mbentry->uniqueid);
        } else {
            return 0;
        }
    }
    if ((update = json_object_get(data->changed, mbentry->uniqueid))) {
        modseq = (modseq_t)json_integer_value(json_object_get(update, "modseq"));
        if (modseq <= mbmodseq) {
            json_object_del(data->changed, mbentry->uniqueid);
        } else {
            return 0;
        }
    }

    /* Did any of the mailbox metadata change? */
    if (mbentry->foldermodseq > data->frommodseq) {
        *(data->only_counts_changed) = 0;
    }

    /* OK, report that update */
    update = json_pack("{s:s s:i}", "id", mbentry->uniqueid, "modseq", mbmodseq);
    if (mbentry->mbtype & MBTYPE_DELETED) {
        updates = data->removed;
    } else {
        updates = data->changed;
    }
    json_object_set_new(updates, mbentry->uniqueid, update);

    return 0;
}

static int jmapmbox_updates_cmp(const void **pa, const void **pb)
{
    const json_t *a = *pa, *b = *pb;
    modseq_t ma, mb;

    ma = (modseq_t) json_integer_value(json_object_get(a, "modseq"));
    mb = (modseq_t) json_integer_value(json_object_get(b, "modseq"));

    if (ma < mb)
        return -1;
    if (ma > mb)
        return 1;
    return 0;
}

static int jmapmbox_updates(jmap_req_t *req, modseq_t frommodseq,
                            size_t limit,
                            json_t **changed, json_t **removed,
                            int *has_more, json_t **newstate,
                            int *only_counts_changed)
{
    *only_counts_changed = 1;

    ptrarray_t updates = PTRARRAY_INITIALIZER;
    struct jmapmbox_updates_data data = {
        json_pack("{}"),
        json_pack("{}"),
        frommodseq,
        only_counts_changed,
        req
    };
    modseq_t windowmodseq;
    const char *id;
    json_t *val;
    int r, i;


    /* Search for updates */
    r = jmap_mboxlist(req, jmapmbox_updates_cb, &data,
                      MBOXTREE_TOMBSTONES | MBOXTREE_DELETED);
    if (r) goto done;

    /* Sort updates by modseq */
    json_object_foreach(data.changed, id, val) {
        ptrarray_add(&updates, val);
    }
    json_object_foreach(data.removed, id, val) {
        ptrarray_add(&updates, val);
    }
    ptrarray_sort(&updates, jmapmbox_updates_cmp);

    /* Build result */
    *changed = json_pack("[]");
    *removed = json_pack("[]");
    *has_more = 0;
    windowmodseq = 0;
    for (i = 0; i < updates.count; i++) {
        json_t *update = ptrarray_nth(&updates, i);
        const char *id = json_string_value(json_object_get(update, "id"));
        modseq_t modseq = json_integer_value(json_object_get(update, "modseq"));

        if (limit && ((size_t) i) >= limit) {
            *has_more = 1;
            break;
        }

        if (windowmodseq < modseq)
            windowmodseq = modseq;

        if (json_object_get(data.changed, id)) {
            json_array_append_new(*changed, json_string(id));
        } else {
            json_array_append_new(*removed, json_string(id));
        }
    }


    if (!json_array_size(*changed) && !json_array_size(*removed)) {
        *only_counts_changed = 0;
    }
    *newstate = jmap_fmtstate(*has_more ? windowmodseq : jmap_highestmodseq(req, 0/*mbtype*/));

done:
    if (data.changed) json_decref(data.changed);
    if (data.removed) json_decref(data.removed);
    ptrarray_fini(&updates);
    return r;
}

static int getMailboxUpdates(jmap_req_t *req)
{
    int r = 0, pe;
    int fetch = 0, has_more = 0, only_counts_changed = 0;
    json_t *changed, *removed, *invalid, *item, *res;
    json_int_t max_changes = 0;
    json_t *oldstate, *newstate;
    const char *since;

    /* Parse and validate arguments. */
    invalid = json_pack("[]");

    /* sinceState */
    pe = readprop(req->args, "sinceState", 1, invalid, "s", &since);
    if (pe > 0 && !atomodseq_t(since)) {
        json_array_append_new(invalid, json_string("sinceState"));
    }
    /* maxChanges */
    pe = readprop(req->args, "maxChanges", 0, invalid, "I", &max_changes);
    if (pe > 0 && max_changes < 0) {
        json_array_append_new(invalid, json_string("maxChanges"));
    }
    /* fetch */
    readprop(req->args, "fetchRecords", 0, invalid, "b", &fetch);

    /* Bail out for argument errors */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(invalid);

    /* Search for updates */
    r = jmapmbox_updates(req, atomodseq_t(since), max_changes,
                         &changed, &removed, &has_more, &newstate,
                         &only_counts_changed);
    if (r) goto done;
    oldstate = json_string(since);

    /* Prepare response */
    res = json_pack("{}");
    json_object_set_new(res, "accountId", json_string(req->accountid));
    json_object_set_new(res, "oldState", oldstate);
    json_object_set_new(res, "newState", newstate);
    json_object_set_new(res, "hasMoreUpdates", json_boolean(has_more));
    json_object_set_new(res, "changed", changed);
    json_object_set_new(res, "removed", removed);
    json_object_set_new(res, "onlyCountsChanged", json_boolean(only_counts_changed));

    item = json_pack("[]");
    json_array_append_new(item, json_string("mailboxUpdates"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

    if (fetch) {
        if (json_array_size(changed)) {
            struct jmap_req subreq = *req;
            subreq.args = json_pack("{}");
            json_object_set(subreq.args, "ids", changed);

            json_t *props = json_object_get(req->args, "fetchRecordProperties");
            if (props) {
                json_object_set(subreq.args, "properties", props);
            }
            else if (only_counts_changed) {
                json_object_set_new(subreq.args, "properties",
                        json_pack("[s,s,s,s]",
                            "totalMessages", "unreadMessages",
                            "totalthreads",  "unreadThreads"));
            }

            r = getMailboxes(&subreq);
            json_decref(subreq.args);
            if (r) goto done;
        }
    }

done:
    return r;
}

static json_t *emailer_from_addr(const struct address *a)
{
    json_t *emailers = json_pack("[]");
    struct buf buf = BUF_INITIALIZER;

    while (a) {
        json_t *e = json_pack("{}");
        const char *mailbox = a->mailbox ? a->mailbox : "";
        const char *domain = a->domain ? a->domain : "";

        if (!strcmp(domain, "unspecified-domain")) {
            domain = "";
        }
        buf_printf(&buf, "%s@%s", mailbox, domain);

        if (a->name) {
            char *dec = charset_decode_mimeheader(a->name, CHARSET_SNIPPET);
            if (dec) {
                json_object_set_new(e, "name", json_string(dec));
            }
            free(dec);
        } else {
            json_object_set_new(e, "name", json_string(""));
        }

        json_object_set_new(e, "email", json_string(buf_cstring(&buf)));

        json_array_append_new(emailers, e);
        buf_reset(&buf);
        a = a->next;
    }

    if (!json_array_size(emailers)) {
        json_decref(emailers);
        emailers = json_null();
    }

    buf_free(&buf);
    return emailers;
}

/* Generate a preview of text of at most len bytes, excluding the zero
 * byte.
 *
 * Consecutive whitespaces, including newlines, are collapsed to a single
 * blank. If text is longer than len and len is greater than 4, then return
 * a string  ending in '...' and holding as many complete UTF-8 characters,
 * that the total byte count of non-zero characters is at most len.
 *
 * The input string must be properly encoded UTF-8 */
static char *extract_preview(const char *text, size_t len)
{
    unsigned char *dst, *d, *t;
    size_t n;

    if (!text) {
        return NULL;
    }

    /* Replace all whitespace with single blanks. */
    dst = (unsigned char *) xzmalloc(len+1);
    for (t = (unsigned char *) text, d = dst; *t && d < (dst+len); ++t, ++d) {
        *d = isspace(*t) ? ' ' : *t;
        if (isspace(*t)) {
            while(isspace(*++t))
                ;
            --t;
        }
    }
    n = d - dst;

    /* Anything left to do? */
    if (n < len || len <= 4) {
        return (char*) dst;
    }

    /* Append trailing ellipsis. */
    dst[--n] = '.';
    dst[--n] = '.';
    dst[--n] = '.';
    while (n && (dst[n] & 0xc0) == 0x80) {
        dst[n+2] = 0;
        dst[--n] = '.';
    }
    if (dst[n] >= 0x80) {
        dst[n+2] = 0;
        dst[--n] = '.';
    }
    return (char *) dst;
}

static void extract_plain_cb(const struct buf *buf, void *rock)
{
    struct buf *dst = (struct buf*) rock;
    const char *p;
    int seenspace = 0;

    /* Just merge multiple space into one. That's similar to
     * charset_extract's MERGE_SPACE but since we don't want
     * it to canonify the text into search form */
    for (p = buf_base(buf); p < buf_base(buf) + buf_len(buf) && *p; p++) {
        if (*p == ' ') {
            if (seenspace) continue;
            seenspace = 1;
        } else {
            seenspace = 0;
        }
        buf_appendmap(dst, p, 1);
    }
}

static char *extract_plain(const char *html) {
    struct buf src = BUF_INITIALIZER;
    struct buf dst = BUF_INITIALIZER;
    charset_t utf8 = charset_lookupname("utf8");
    char *text;
    char *tmp, *q;
    const char *p;

    /* Replace <br> and <p> with newlines */
    q = tmp = xstrdup(html);
    p = html;
    while (*p) {
        if (!strncmp(p, "<br>", 4) || !strncmp(p, "</p>", 4)) {
            *q++ = '\n';
            p += 4;
        }
        else if (!strncmp(p, "p>", 3)) {
            p += 3;
        } else {
            *q++ = *p++;
        }
    }
    *q = 0;

    /* Strip html tags */
    buf_init_ro(&src, tmp, q - tmp);
    buf_setcstr(&dst, "");
    charset_extract(&extract_plain_cb, &dst,
            &src, utf8, ENCODING_NONE, "HTML", CHARSET_SNIPPET);
    buf_cstring(&dst);

    /* Trim text */
    buf_trim(&dst);
    text = buf_releasenull(&dst);
    if (!strlen(text)) {
        free(text);
        text = NULL;
    }

    buf_free(&src);
    free(tmp);
    charset_free(&utf8);

    return text;
}

struct jmapmsg_mailboxes_data {
    jmap_req_t *req;
    json_t *mboxs;
};

static int jmapmsg_mailboxes_cb(const conv_guidrec_t *rec, void *rock)
{
    struct jmapmsg_mailboxes_data *data = (struct jmapmsg_mailboxes_data*) rock;
    json_t *mboxs = data->mboxs;
    jmap_req_t *req = data->req;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    uint32_t flags;
    int r;

    if (rec->part) return 0;

    r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
    if (r) return r;

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (r) goto done;

    r = msgrecord_get_systemflags(mr, &flags);
    if (r) goto done;

    if (!r && !(flags & (FLAG_EXPUNGED|FLAG_DELETED))) {
        json_object_set_new(mboxs, mbox->uniqueid, json_string(mbox->name));
    }


done:
    if (mr) msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);
    return r;
}

static json_t* jmapmsg_mailboxes(jmap_req_t *req, const char *msgid)
{
    struct jmapmsg_mailboxes_data data = { req, json_pack("{}") };

    conversations_guid_foreach(req->cstate, msgid+1, jmapmsg_mailboxes_cb, &data);

    return data.mboxs;
}

struct attachment {
    struct body *body;
};

struct msgbodies {
    struct body *text;
    struct body *html;
    ptrarray_t atts;
    ptrarray_t msgs;
};

#define MSGBODIES_INITIALIZER \
    { NULL, NULL, PTRARRAY_INITIALIZER, PTRARRAY_INITIALIZER }

static int find_msgbodies(struct body *root, struct buf *msg_buf,
                          struct msgbodies *bodies)
{
    /* Dissect a message into its best text and html bodies, attachments
     * and embedded messages. Based on the IMAPTalk find_message function.
     * https://github.com/robmueller/mail-imaptalk/blob/master/IMAPTalk.pm
     *
     * XXX Contrary to the IMAPTalk implementation, this function doesn't
     * generate textlist/htmllist fields, so html-to-text body conversion
     * is only supported marginally.
     */

    ptrarray_t *work = ptrarray_new();
    int i;

    struct partrec {
        int inside_alt;
        int inside_enc;
        int inside_rel;
        int partno;
        struct body *part;
        struct body *parent;
    } *rec;

    rec = xzmalloc(sizeof(struct partrec));
    rec->part = root;
    rec->partno = 1;
    ptrarray_push(work, rec);

    while ((rec = ptrarray_shift(work))) {
        char *disp = NULL, *dispfile = NULL;
        struct body *part = rec->part;
        struct param *param;
        int is_inline = 0;
        int is_attach = 1;

        /* Determine content disposition */
        if (part->disposition) {
            disp = ucase(xstrdup(part->disposition));
        }
        for (param = part->disposition_params; param; param = param->next) {
            if (!strncasecmp(param->attribute, "filename", 8)) {
                dispfile = ucase(xstrdup(param->value));
                break;
            }
        }

        /* Search for inline text */
        if ((!strcmp(part->type, "TEXT")) &&
            (!strcmp(part->subtype, "PLAIN") ||
             !strcmp(part->subtype, "TEXT")  ||
             !strcmp(part->subtype, "ENRICHED") ||
             !strcmp(part->subtype, "HTML")) &&
            ((!disp || strcmp(disp, "ATTACHMENT")) && !dispfile)) {
            /* Text that isn't an attachment or has a filename */
            is_inline = 1;
        }
        if ((!strcmp(part->type, "APPLICATION")) &&
            (!strcmp(part->subtype, "OCTET-STREAM")) &&
            (rec->inside_enc && strstr(dispfile, "ENCRYPTED"))) {
            /* PGP octet-stream inside an pgp-encrypted part */
            is_inline = 1;
        }

        if (is_inline) {
            int is_html = !strcasecmp(part->subtype, "HTML");
            struct body **bodyp = is_html ? &bodies->html : &bodies->text;
            is_attach = 0;

            if (*bodyp == NULL) {
                /* Haven't yet found a body for this type */
                if (!is_html || rec->partno <= 1 || !rec->parent ||
                    strcmp(rec->parent->type, "MULTIPART") ||
                    strcmp(rec->parent->subtype, "MIXED")) {

                    /* Don't treat html parts in a multipart/mixed as an
                       alternative representation unless the first part */
                    *bodyp = part;
                }
            } else if ((*bodyp)->content_size <= 10 && part->content_size > 10) {
                /* Override very small parts e.g. five blank lines */
                *bodyp = part;
            } else if (msg_buf) {
                /* Override parts with zero lines with multi-lines */
                const char *base = msg_buf->s + (*bodyp)->content_offset;
                size_t len = (*bodyp)->content_size;

                if (!memchr(base, '\n', len)) {
                    base = msg_buf->s + part->content_offset;
                    len = part->content_size;
                    if (memchr(base, '\n', len)) {
                        *bodyp = part;
                    }
                }
            }
        }
        else if (!strcmp(part->type, "MULTIPART")) {
            int prio = 0;
            is_attach = 0;

            /* Determine the multipart type and priority */
            if (!strcmp(part->subtype, "SIGNED")) {
                prio = 1;
            }
            else if (!strcmp(part->subtype, "ALTERNATIVE")) {
                rec->inside_alt = 1;
                prio = 1;
            }
            else if (!strcmp(part->subtype, "RELATED")) {
                rec->inside_rel = 1;
                prio = 1;
            }
            else if (!disp || strcmp(disp, "ATTACHMENT")) {
                prio = 1;
            }
            else if (!strcmp(part->subtype, "ENCRYPTED")) {
                rec->inside_enc = 1;
            }

            /* Prioritize signed/alternative/related sub-parts, otherwise
             * look at it once we've seen all other parts at current level */
            for (i = 0; i < part->numparts; i++) {
                struct partrec *subrec;

                subrec = xzmalloc(sizeof(struct partrec));
                *subrec = *rec;
                subrec->parent = part;

                if (prio) {
                    subrec->partno = part->numparts - i;
                    subrec->part = part->subpart + subrec->partno - 1;
                    ptrarray_unshift(work, subrec);
                } else  {
                    subrec->partno = i + 1;
                    subrec->part = part->subpart + subrec->partno - 1;
                    ptrarray_push(work, subrec);
                }
            }
        }

        if (is_attach) {
            if (!strcmp(part->type, "MESSAGE") &&
                !strcmp(part->subtype, "RFC822") &&
                part != root) {
                ptrarray_push(&bodies->msgs, part);
            } else {
                ptrarray_push(&bodies->atts, part);
            }
        }

        if (disp) free(disp);
        if (dispfile) free(dispfile);
        free(rec);
    }

    assert(work->count == 0);
    ptrarray_free(work);

    return 0;
}

static int extract_headers(const char *key, const char *val, void *rock)
{
    json_t *headers = (json_t*) rock;
    json_t *curval;
    char *decodedval = NULL;
    char *lckey = xstrdup(key);
    char *p;
    for (p = lckey; *p; p++) {
        *p = tolower(*p);
    }

    if (isspace(*val)) val++;

    decodedval = charset_decode_mimeheader(val, CHARSET_SNIPPET);
    if (!decodedval) goto done;

    if ((curval = json_object_get(headers, lckey))) {
        char *newval = strconcat(json_string_value(curval), "\n", decodedval, NULL);
        json_object_set_new(headers, lckey, json_string(newval));
        free(newval);
    } else {
        json_object_set_new(headers, lckey, json_string(decodedval));
    }

done:
    free(lckey);
    free(decodedval);
    return  0;
}

static int extract_annotations(const char *mboxname __attribute__((unused)),
                               uint32_t uid __attribute__((unused)),
                               const char *entry,
                               const char *userid __attribute__((unused)),
                               const struct buf *value,
                               const struct annotate_metadata *mdata __attribute__((unused)),
                               void *rock)
{
    json_t *annotations = (json_t *)rock;

    const char *prefix = "/vendor/jmapio/";
    size_t prefixlen = strlen(prefix);

    if (!strncmp(entry, prefix, prefixlen)) {
        json_object_set_new(annotations, entry + prefixlen, json_string(buf_cstring(value)));
    }

    return 0;
}

static char *jmap_msgid(const struct message_guid *guid)
{
    char *msgid = xzmalloc(26);
    msgid[0] = 'M';
    memcpy(msgid+1, message_guid_encode(guid), 24);
    return msgid;
}

static char *jmap_thrid(conversation_id_t cid)
{
    char *thrid = xzmalloc(18);
    thrid[0] = 'T';
    memcpy(thrid+1, conversation_id_encode(cid), 16);
    return thrid;
}

static conversation_id_t jmap_decode_thrid(const char *thrid)
{
    conversation_id_t cid = 0;
    if (thrid[0] == 'T')
        conversation_id_decode(&cid, thrid+1);
    return cid;
}

static json_t *jmapmsg_annot_read(const jmap_req_t *req, msgrecord_t *mr,
                                  const char *annot, int structured)
{
    struct buf buf = BUF_INITIALIZER;
    json_t *annotvalue = NULL;

    if (!strncmp(annot, "/shared/", 8)) {
        msgrecord_annot_lookup(mr, annot+7, /*userid*/"", &buf);
    }
    else if (!strncmp(annot, "/private/", 9)) {
        msgrecord_annot_lookup(mr, annot+7, req->userid, &buf);
    }
    else {
        msgrecord_annot_lookup(mr, annot+7, "", &buf);
    }
    if (buf_len(&buf)) {
        if (structured) {
            json_error_t jerr;
            annotvalue = json_loads(buf_base(&buf), JSON_DECODE_ANY, &jerr);
            /* XXX - log error? */
        }
        else {
            annotvalue = json_string(buf_cstring(&buf));
        }

        if (!annotvalue) {
            syslog(LOG_ERR, "jmap: annotation %s has bogus value", annot);
        }
    }

    buf_free(&buf);

    return annotvalue;
}

static int jmapmsg_from_body(jmap_req_t *req, hash_table *props,
                             struct body *body, struct buf *msg_buf,
                             msgrecord_t *mr, MsgType type,
                             json_t **msgp)
{
    struct msgbodies bodies = MSGBODIES_INITIALIZER;
    json_t *msg = NULL;
    json_t *headers = json_pack("{}");
    json_t *annotations = json_pack("{}");
    struct buf buf = BUF_INITIALIZER;
    char *text = NULL, *html = NULL;
    int r;

    /* Dissect message into its parts */
    r = find_msgbodies(body, msg_buf, &bodies);
    if (r) goto done;

    /* Always read the message headers */
    r = message_foreach_header(msg_buf->s + body->header_offset,
                               body->header_size, extract_headers, headers);
    if (r) goto done;

    r = msgrecord_annot_findall(mr, "*", extract_annotations, annotations);
    if (r) goto done;

    msg = json_pack("{}");

    /* headers */
    if (_wantprop(props, "headers")) {
        json_object_set(msg, "headers", headers);
    }
    else {
        json_t *wantheaders = json_pack("{}");
        struct buf buf = BUF_INITIALIZER;
        buf_setcstr(&buf, "headers.");
        const char *key;
        json_t *val;
        json_object_foreach(headers, key, val) {
            buf_truncate(&buf, 8);
            buf_appendcstr(&buf, key);
            if (_wantprop(props, buf_cstring(&buf))) {
                json_object_set(wantheaders, key, val);
            }
        }

        buf_free(&buf);
        if (json_object_size(wantheaders))
            json_object_set_new(msg, "headers", wantheaders);
        else
            json_decref(wantheaders);
    }

    /* annotations */
    if (_wantprop(props, "annotations")) {
        json_object_set(msg, "annotations", annotations);
    }
    else {
        json_t *wantannotations = json_pack("{}");
        buf_setcstr(&buf, "annotations.");
        const char *key;
        json_t *val;
        json_object_foreach(annotations, key, val) {
            buf_truncate(&buf, 8);
            buf_appendcstr(&buf, key);
            if (_wantprop(props, buf_cstring(&buf))) {
                json_object_set(wantannotations, key, val);
            }
        }
        if (json_object_size(wantannotations))
            json_object_set_new(msg, "annotations", wantannotations);
        else
            json_decref(wantannotations);
    }


    /* sender */
    if (_wantprop(props, "sender")) {
        const char *key, *s = NULL;
        json_t *val, *sender = json_null();

        json_object_foreach(headers, key, val) {
            if (!strcasecmp(key, "Sender")) {
                s = json_string_value(val);
                break;
            }
        }
        if (s) {
            struct address *addr = NULL;
            parseaddr_list(s, &addr);
            if (addr) {
                json_t *senders = emailer_from_addr(addr);
                if (json_array_size(senders)) {
                    sender = json_array_get(senders, 0);
                    json_incref(sender);
                }
                json_decref(senders);
            }
            parseaddr_free(addr);
        }
        json_object_set_new(msg, "sender", sender);
    }
    /* from */
    if (_wantprop(props, "from")) {
        json_object_set_new(msg, "from", emailer_from_addr(body->from));
    }
    /* to */
    if (_wantprop(props, "to")) {
        json_object_set_new(msg, "to", emailer_from_addr(body->to));
    }
    /* cc */
    if (_wantprop(props, "cc")) {
        json_object_set_new(msg, "cc", emailer_from_addr(body->cc));
    }
    /*  bcc */
    if (_wantprop(props, "bcc")) {
        json_object_set_new(msg, "bcc", emailer_from_addr(body->bcc));
    }
    /* replyTo */
    if (_wantprop(props, "replyTo")) {
        json_t *reply_to = json_null();
        if (json_object_get(headers, "reply-to")) {
            reply_to = emailer_from_addr(body->reply_to);
        }
        json_object_set_new(msg, "replyTo", reply_to);
    }
    /* subject */
    if (_wantprop(props, "subject")) {
        char *subject = NULL;
        if (body->subject) {
            subject = charset_decode_mimeheader(body->subject, CHARSET_SNIPPET);
        }
        json_object_set_new(msg, "subject", json_string(subject ? subject : ""));
        free(subject);
    }
    /* date */
    if (_wantprop(props, "date")) {
        char datestr[RFC3339_DATETIME_MAX];
        time_t t;

        r = msgrecord_get_internaldate(mr, &t);
        if (r) return r;

        if (type == MSG_IS_ATTACHED)
            time_from_rfc5322(body->date, &t, DATETIME_FULL);

        time_to_rfc3339(t, datestr, RFC3339_DATETIME_MAX);
        json_object_set_new(msg, "date", json_string(datestr));
    }

    if (_wantprop(props, "textBody") ||
        _wantprop(props, "htmlBody") ||
        (_wantprop(props, "preview") && !config_getstring(IMAPOPT_JMAP_PREVIEW_ANNOT)) ||
        _wantprop(props, "body")) {

        if (bodies.text) {
            charset_t cs = charset_lookupname(bodies.text->charset_id);
            text = charset_to_utf8(msg_buf->s + bodies.text->content_offset,
                    bodies.text->content_size, cs, bodies.text->charset_enc);
            charset_free(&cs);
        }
        if (bodies.html) {
            charset_t cs = charset_lookupname(bodies.html->charset_id);
            html = charset_to_utf8(msg_buf->s + bodies.html->content_offset,
                    bodies.html->content_size, cs, bodies.html->charset_enc);
            charset_free(&cs);
        }
    }

    /* textBody */
    if (_wantprop(props, "textBody") || (_wantprop(props, "body") && !html)) {
        if (!text && html) {
            text = extract_plain(html);
        }
        json_object_set_new(msg, "textBody", json_string(text ? text : ""));
    }
    /* htmlBody */
    if (_wantprop(props, "htmlBody") || (_wantprop(props, "body") && html)) {
        json_object_set_new(msg, "htmlBody", html ? json_string(html) : json_null());
    }

    if (_wantprop(props, "hasAttachment")) {
        int b = 0;
        if (type == MSG_IS_ATTACHED) {
            b = bodies.atts.count + bodies.msgs.count;
        } else {
            msgrecord_hasflag(mr, JMAP_HAS_ATTACHMENT_FLAG, &b);
        }
        json_object_set_new(msg, "hasAttachment", json_boolean(b));
    }

    /* attachments */
    if (_wantprop(props, "attachments")) {
        int i;
        json_t *atts = json_pack("[]");
        json_t *inlinedcids = NULL, *imgsizes = NULL;
        const char *annot;

        /* Load the message annotation with the hash of cid: urls */
        if ((annot = config_getstring(IMAPOPT_JMAP_INLINEDCIDS_ANNOT))) {
            inlinedcids = jmapmsg_annot_read(req, mr, annot, /*structured*/1);
        }

        /* Load the message annotation with the hash of image dimensions */
        if ((annot = config_getstring(IMAPOPT_JMAP_IMAGESIZE_ANNOT))) {
            imgsizes = jmapmsg_annot_read(req, mr, annot, /*structured*/1);
        }

        for (i = 0; i < bodies.atts.count; i++) {
            struct body *part = ptrarray_nth(&bodies.atts, i);
            struct param *param;
            json_t *att;
            charset_t ascii = charset_lookupname("us-ascii");
            const char *cid;
            strarray_t headers = STRARRAY_INITIALIZER;
            char *freeme;

            char *blobid = jmap_blobid(&part->content_guid);
            att = json_pack("{s:s}", "blobId", blobid);
            free(blobid);

            /* type */
            buf_setcstr(&buf, part->type);
            if (part->subtype) {
                buf_appendcstr(&buf, "/");
                buf_appendcstr(&buf, part->subtype);
            }
            json_object_set_new(att, "type", json_string(buf_lcase(&buf)));

            /* name */
            const char *fname = NULL;
            int is_extended = 0;
            for (param = part->disposition_params; param; param = param->next) {
                if (!strncasecmp(param->attribute, "filename", 8)) {
                    is_extended = param->attribute[8] == '*';
                    fname = param->value;
                    break;
                }
            }
            if (!fname) {
                for (param = part->params; param; param = param->next) {
                    if (!strncasecmp(param->attribute, "name", 4)) {
                        is_extended = param->attribute[4] == '*';
                        fname = param->value;
                        break;
                    }
                }
            }
            if (fname && is_extended) {
                char *s = charset_parse_mimexvalue(fname, NULL);
                json_object_set_new(att, "name", s ? json_string(s) : json_null());
                free(s);
            }
            else if (fname) {
                int mime_flags = charset_flags & CHARSET_MIME_UTF8;
                char *s = charset_parse_mimeheader(fname, mime_flags);
                json_object_set_new(att, "name", s ? json_string(s) : json_null());
                free(s);
            }
            else {
                json_object_set_new(att, "name", json_null());
            }

            /* size */
            if (part->charset_enc) {
                buf_reset(&buf);
                charset_decode(&buf, msg_buf->s + part->content_offset,
                               part->content_size, part->charset_enc);
                json_object_set_new(att, "size", json_integer(buf_len(&buf)));
            } else {
                json_object_set_new(att, "size", json_integer(part->content_size));
            }

            /* cid */
            strarray_add(&headers, "Content-ID");
            freeme = xstrndup(msg_buf->s + part->header_offset, part->header_size);
            message_pruneheader(freeme, &headers, NULL);
            if ((cid = strchr(freeme, ':'))) {
                char *unfolded;
                if ((unfolded = charset_unfold(cid + 1, strlen(cid), 0))) {
                    buf_setcstr(&buf, unfolded);
                    free(unfolded);
                    buf_trim(&buf);
                    cid = buf_cstring(&buf);
                } else {
                    cid = NULL;
                }
            }
            json_object_set_new(att, "cid", cid ? json_string(cid) : json_null());
            free(freeme);

            /* isInline */
            if (inlinedcids && cid && json_object_get(inlinedcids, cid)) {
                json_object_set_new(att, "isInline", json_true());
            }
            else {
                json_object_set_new(att, "isInline", json_false());
            }

            /* width, height */
            json_t *width = json_null(), *height = json_null(), *dim;
            if (imgsizes && (dim = json_object_get(imgsizes, part->part_id))) {
                if (json_array_size(dim) >= 2) {
                    width = json_incref(json_array_get(dim, 0));
                    height = json_incref(json_array_get(dim, 1));
                }
            }
            json_object_set_new(att, "width", width);
            json_object_set_new(att, "height", height);

            charset_free(&ascii);
            strarray_fini(&headers);
            json_array_append_new(atts, att);
        }
        if (!json_array_size(atts)) {
            json_decref(atts);
            atts = json_null();
        }
        json_object_set_new(msg, "attachments", atts);

        if (inlinedcids) json_decref(inlinedcids);
    }

    /* attachedMessages */
    if (_wantprop(props, "attachedMessages")) {
        int i;
        json_t *msgs = json_pack("{}");

        for (i = 0; i < bodies.msgs.count; i++) {
            struct body *part = ptrarray_nth(&bodies.msgs, i);
            json_t *submsg = NULL;

            r = jmapmsg_from_body(req, props, part->subpart, msg_buf,
                                  mr, MSG_IS_ATTACHED, &submsg);
            if (r) goto done;

            char *blobid = jmap_blobid(&part->content_guid);
            json_object_set_new(msgs, blobid, submsg);
            free(blobid);
        }
        if (!json_object_size(msgs)) {
            json_decref(msgs);
            msgs = json_null();
        }
        json_object_set_new(msg, "attachedMessages", msgs);
    }

    if (type == MSG_IS_ROOT) {
        uint32_t flags;
        bit64 cid;
        uint32_t size;

        r = msgrecord_get_systemflags(mr, &flags);
        if (r) goto done;

        r = msgrecord_get_cid(mr, &cid);
        if (r) goto done;

        r = msgrecord_get_size(mr, &size);
        if (r) goto done;

        /* id */
        struct message_guid guid;

        r = msgrecord_get_guid(mr, &guid);
        if (r) goto done;

        char *msgid = jmap_msgid(&guid);
        json_object_set_new(msg, "id", json_string(msgid));

        /* blobId */
        if (_wantprop(props, "blobId")) {
            char *blobid = jmap_blobid(&guid);
            json_object_set_new(msg, "blobId", json_string(blobid));
            free(blobid);
        }
        /* threadId */
        if (_wantprop(props, "threadId")) {
            if (cid) {
                char *thrid = jmap_thrid(cid);
                json_object_set_new(msg, "threadId", json_string(thrid));
                free(thrid);
            }
            else {
                json_object_set_new(msg, "threadId", json_null());
            }
        }
        /* mailboxIds */
        if (_wantprop(props, "mailboxIds")) {
            json_t *mailboxes, *val, *ids = json_pack("[]");
            const char *mboxid;
            mailboxes = jmapmsg_mailboxes(req, msgid);
            json_object_foreach(mailboxes, mboxid, val) {
                json_array_append_new(ids, json_string(mboxid));
            }
            json_decref(mailboxes);
            json_object_set_new(msg, "mailboxIds", ids);
        }

        /* inReplyToMessageId */
        if (_wantprop(props, "inReplyToMessageId")) {
            json_t *reply_id = json_null();
            if (flags & FLAG_DRAFT) {
                const char *key;
                json_t *val;

                json_object_foreach(headers, key, val) {
                    if (!strcasecmp(key, JMAP_INREPLYTO_HEADER)) {
                        reply_id = val;
                        break;
                    }
                }
            }
            json_object_set(msg, "inReplyToMessageId", reply_id);
        }
        /* isUnread */
        if (_wantprop(props, "isUnread")) {
            json_object_set_new(msg, "isUnread", json_boolean(!(flags & FLAG_SEEN)));
        }
        /* isFlagged */
        if (_wantprop(props, "isFlagged")) {
            json_object_set_new(msg, "isFlagged", json_boolean(flags & FLAG_FLAGGED));
        }
        /* isAnswered */
        if (_wantprop(props, "isAnswered")) {
            json_object_set_new(msg, "isAnswered", json_boolean(flags & FLAG_ANSWERED));
        }
        /* isDraft */
        if (_wantprop(props, "isDraft")) {
            json_object_set_new(msg, "isDraft", json_boolean(flags & FLAG_DRAFT));
        }

        /* size */
        if (_wantprop(props, "size")) {
            json_object_set_new(msg, "size", json_integer(size));
        }

        /* preview */
        if (_wantprop(props, "preview")) {
            const char *annot = config_getstring(IMAPOPT_JMAP_PREVIEW_ANNOT);
            if (annot) {
                json_t *preview = jmapmsg_annot_read(req, mr, annot, /*structured*/0);
                json_object_set_new(msg, "preview", preview ? preview : json_string(""));
            }
            else {
                /* Generate our own preview */
                char *preview = extract_preview(text,
                        config_getint(IMAPOPT_JMAP_PREVIEW_LENGTH));
                json_object_set_new(msg, "preview", json_string(preview));
                free(preview);
            }
        }
        free(msgid);
    }

    r = 0;

done:
    json_decref(annotations);
    json_decref(headers);
    buf_free(&buf);
    if (text) free(text);
    if (html) free(html);
    ptrarray_fini(&bodies.atts);
    ptrarray_fini(&bodies.msgs);
    if (r) {
        if (msg) json_decref(msg);
        msg = NULL;
    }
    *msgp = msg;
    return r;
}

struct jmapmsg_find_data {
    jmap_req_t *req;
    char *mboxname;
    uint32_t uid;
};

static int jmapmsg_find_cb(const conv_guidrec_t *rec, void *rock)
{
    struct jmapmsg_find_data *d = (struct jmapmsg_find_data*) rock;
    jmap_req_t *req = d->req;
    int r = 0;

    if (rec->part) return 0;

    if (!d->mboxname || jmap_isopenmbox(req, rec->mboxname)) {
        struct mailbox *mbox = NULL;
        msgrecord_t *mr = NULL;
        uint32_t flags;
        mbentry_t *mbentry = NULL;

        /* Make sure we are allowed to read this mailbox */
        if (strcmp(req->accountid, req->userid)) {
            if (mboxlist_lookup(rec->mboxname, &mbentry, NULL))
                return 0;
            int rights = jmap_myrights(req, mbentry);
            mboxlist_entry_free(&mbentry);
            if (!(rights & ACL_READ))
                return 0;
        }

        /* Prefer to use messages in already opened mailboxes */

        r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
        if (r) return r;

        r = msgrecord_find(mbox, rec->uid, &mr);
        if (!r) {
            r = msgrecord_get_systemflags(mr, &flags);
            if (!r && !(flags & (FLAG_EXPUNGED|FLAG_DELETED))) {
                if (d->mboxname) {
                    free(d->mboxname);
                    r = IMAP_OK_COMPLETED;
                }
                d->mboxname = xstrdup(rec->mboxname);
                d->uid = rec->uid;
            }
            msgrecord_unref(&mr);
        }

        jmap_closembox(req, &mbox);
    }

    return r;
}

static int jmapmsg_find(jmap_req_t *req, const char *msgid,
                        char **mboxnameptr, uint32_t *uid)
{
    struct jmapmsg_find_data data = { req, NULL, 0 };
    int r;

    /* must be prefixed with 'M' */
    if (msgid[0] != 'M')
        return IMAP_NOTFOUND;
    /* this is on a 24 character prefix only */
    if (strlen(msgid) != 25)
        return IMAP_NOTFOUND;

    r = conversations_guid_foreach(req->cstate, msgid+1, jmapmsg_find_cb, &data);
    if (r == IMAP_OK_COMPLETED) {
        r = 0;
    }
    else if (!data.mboxname) {
        r = IMAP_NOTFOUND;
    }
    *mboxnameptr = data.mboxname;
    *uid = data.uid;
    return r;
}

struct jmapmsg_count_data {
    jmap_req_t *req;
    size_t count;
};

static int jmapmsg_count_cb(const conv_guidrec_t *rec, void *rock)
{
    struct jmapmsg_count_data *d = (struct jmapmsg_count_data*) rock;
    jmap_req_t *req = d->req;
    msgrecord_t *mr = NULL;
    struct mailbox *mbox = NULL;
    uint32_t flags;
    int r = 0;

    if (rec->part) return 0;

    r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
    if (r) return r;

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (!r) {
        r = msgrecord_get_systemflags(mr, &flags);
        if (!r && !(flags & (FLAG_EXPUNGED|FLAG_DELETED))) {
            d->count++;
        }
        msgrecord_unref(&mr);
    }

    jmap_closembox(req, &mbox);

    return r;
}

static int jmapmsg_count(jmap_req_t *req, const char *msgid, size_t *count)
{
    struct jmapmsg_count_data data = { req, 0 };
    int r;

    if (msgid[0] != 'M')
        return IMAP_NOTFOUND;

    r = conversations_guid_foreach(req->cstate, msgid+1, jmapmsg_count_cb, &data);
    if (r == IMAP_OK_COMPLETED) {
        r = 0;
    } else if (!data.count) {
        r = IMAP_NOTFOUND;
    }
    *count = data.count;
    return r;
}

struct jmapmsg_isexpunged_data {
    jmap_req_t *req;
    int is_expunged;
};

static int jmapmsg_isexpunged_cb(const conv_guidrec_t *rec, void *rock)
{
    struct jmapmsg_isexpunged_data *d = (struct jmapmsg_isexpunged_data*) rock;
    jmap_req_t *req = d->req;
    msgrecord_t *mr = NULL;
    struct mailbox *mbox = NULL;
    uint32_t flags;
    int r = 0;

    if (rec->part) return 0;

    r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
    if (r) return r;

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (!r) {
        r = msgrecord_get_systemflags(mr, &flags);
        if (!r && !(flags & (FLAG_EXPUNGED|FLAG_DELETED))) {
            d->is_expunged = 0;
            r = IMAP_OK_COMPLETED;
        }
        msgrecord_unref(&mr);
    }

    jmap_closembox(req, &mbox);

    return r;
}

static int jmapmsg_isexpunged(jmap_req_t *req, const char *msgid, int *is_expunged)
{
    struct jmapmsg_isexpunged_data data = { req, 1 };
    int r;

    if (msgid[0] != 'M')
        return IMAP_NOTFOUND;

    if (strlen(msgid) != 25)
        return IMAP_NOTFOUND;

    r = conversations_guid_foreach(req->cstate, msgid+1, jmapmsg_isexpunged_cb, &data);
    if (r == IMAP_OK_COMPLETED) {
        r = 0;
    }

    *is_expunged = data.is_expunged;
    return r;
}

static int jmapmsg_from_record(jmap_req_t *req, hash_table *props,
                               msgrecord_t *mr, json_t **msgp)
{
    struct body *body = NULL;
    struct buf msg_buf = BUF_INITIALIZER;
    int r;

    r = msgrecord_get_body(mr, &msg_buf);
    if (r) return r;

    /* Parse message body structure */
    r = msgrecord_get_bodystructure(mr, &body);
    if (r) return r;

    r = jmapmsg_from_body(req, props, body, &msg_buf, mr, MSG_IS_ROOT,
                          msgp);
    message_free_body(body);
    free(body);
    buf_free(&msg_buf);

    return r;
}

static void match_string(search_expr_t *parent, const char *s, const char *name)
{
    charset_t utf8 = charset_lookupname("utf-8");
    search_expr_t *e;
    const search_attr_t *attr = search_attr_find(name);
    enum search_op op;

    assert(attr);

    op = search_attr_is_fuzzable(attr) ? SEOP_FUZZYMATCH : SEOP_MATCH;

    e = search_expr_new(parent, op);
    e->attr = attr;
    e->value.s = charset_convert(s, utf8, charset_flags);
    if (!e->value.s) {
        e->op = SEOP_FALSE;
        e->attr = NULL;
    }

    charset_free(&utf8);
}

static void match_mailbox(jmap_req_t *req, search_expr_t *parent,
                          json_t *mailbox, int is_not)
{
    search_expr_t *e;
    const char *s = json_string_value(mailbox);
    char *mboxname = jmapmbox_find_uniqueid(req, s);
    if (!mboxname) {
        /* XXX - add a "never match" terminal */
        return;
    }

    if (is_not) {
        parent = search_expr_new(parent, SEOP_NOT);
    }

    e = search_expr_new(parent, SEOP_MATCH);
    e->attr = search_attr_find("folder");
    e->value.s = mboxname; // takes ownership
}

static search_expr_t *buildsearch(jmap_req_t *req, json_t *filter,
                                  search_expr_t *parent)
{
    search_expr_t *this, *e;
    json_t *val;
    const char *s;
    size_t i;
    time_t t;

    if (!JNOTNULL(filter)) {
        return search_expr_new(parent, SEOP_TRUE);
    }

    if ((s = json_string_value(json_object_get(filter, "operator")))) {
        enum search_op op = SEOP_UNKNOWN;

        if (!strcmp("AND", s)) {
            op = SEOP_AND;
        } else if (!strcmp("OR", s)) {
            op = SEOP_OR;
        } else if (!strcmp("NOT", s)) {
            op = SEOP_NOT;
        }

        this = search_expr_new(parent, op);
        e = op == SEOP_NOT ? search_expr_new(this, SEOP_OR) : this;

        json_array_foreach(json_object_get(filter, "conditions"), i, val) {
            buildsearch(req, val, e);
        }
    } else {
        this = search_expr_new(parent, SEOP_AND);

        /* zero properties evaluate to true */
        search_expr_new(this, SEOP_TRUE);

        if ((s = json_string_value(json_object_get(filter, "after")))) {
            time_from_iso8601(s, &t);
            e = search_expr_new(this, SEOP_GE);
            e->attr = search_attr_find("internaldate");
            e->value.u = t;
        }
        if ((s = json_string_value(json_object_get(filter, "before")))) {
            time_from_iso8601(s, &t);
            e = search_expr_new(this, SEOP_LE);
            e->attr = search_attr_find("internaldate");
            e->value.u = t;
        }
        if ((s = json_string_value(json_object_get(filter, "body")))) {
            match_string(this, s, "body");
        }
        if ((s = json_string_value(json_object_get(filter, "cc")))) {
            match_string(this, s, "cc");
        }
        if ((s = json_string_value(json_object_get(filter, "from")))) {
            match_string(this, s, "from");
        }
        if (JNOTNULL((val = json_object_get(filter, "hasAttachment")))) {
            e = val == json_false() ? search_expr_new(this, SEOP_NOT) : this;
            e = search_expr_new(e, SEOP_MATCH);
            e->attr = search_attr_find("keyword");
            e->value.s = xstrdup(JMAP_HAS_ATTACHMENT_FLAG);
        }
        if ((s = json_string_value(json_object_get(filter, "attachmentName")))) {
            match_string(this, s, "attachmentname");
        }
        if (JNOTNULL((val = json_object_get(filter, "header")))) {
            const char *k, *v;
            charset_t utf8 = charset_lookupname("utf-8");
            search_expr_t *e;

            if (json_array_size(val) == 2) {
                k = json_string_value(json_array_get(val, 0));
                v = json_string_value(json_array_get(val, 1));
            } else {
                k = json_string_value(json_array_get(val, 0));
                v = ""; /* Empty string matches any value */
            }

            e = search_expr_new(this, SEOP_MATCH);
            e->attr = search_attr_find_field(k);
            e->value.s = charset_convert(v, utf8, charset_flags);
            if (!e->value.s) {
                e->op = SEOP_FALSE;
                e->attr = NULL;
            }
            charset_free(&utf8);
        }
        if ((val = json_object_get(filter, "inMailbox"))) {
            match_mailbox(req, this, val, /*is_not*/0);
        }
        if ((val = json_object_get(filter, "inMailboxOtherThan"))) {
            match_mailbox(req, this, val, /*is_not*/1);
        }
        if (JNOTNULL((val = json_object_get(filter, "isAnswered")))) {
            e = val == json_true() ? this : search_expr_new(this, SEOP_NOT);
            e = search_expr_new(e, SEOP_MATCH);
            e->attr = search_attr_find("systemflags");
            e->value.u = FLAG_ANSWERED;
        }
        if (JNOTNULL((val = json_object_get(filter, "isDraft")))) {
            e = val == json_true() ? this : search_expr_new(this, SEOP_NOT);
            e = search_expr_new(e, SEOP_MATCH);
            e->attr = search_attr_find("systemflags");
            e->value.u = FLAG_DRAFT;
        }
        if (JNOTNULL((val = json_object_get(filter, "isFlagged")))) {
            e = val == json_true() ? this : search_expr_new(this, SEOP_NOT);
            e = search_expr_new(e, SEOP_MATCH);
            e->attr = search_attr_find("systemflags");
            e->value.u = FLAG_FLAGGED;
        }
        if (JNOTNULL((val = json_object_get(filter, "isUnread")))) {
            e = val == json_true() ? search_expr_new(this, SEOP_NOT) : this;
            e = search_expr_new(e, SEOP_MATCH);
            e->attr = search_attr_find("indexflags");
            e->value.u = MESSAGE_SEEN;
        }

        if (JNOTNULL((val = json_object_get(filter, "maxSize")))) {
            e = search_expr_new(this, SEOP_LE);
            e->attr = search_attr_find("size");
            e->value.u = json_integer_value(val);
        }
        if (JNOTNULL((val = json_object_get(filter, "minSize")))) {
            e = search_expr_new(this, SEOP_GE);
            e->attr = search_attr_find("size");
            e->value.u = json_integer_value(val);
        }
        if ((s = json_string_value(json_object_get(filter, "sinceMessageState")))) {
            /* non-standard */
            e = search_expr_new(this, SEOP_GT);
            e->attr = search_attr_find("modseq");
            e->value.u = atomodseq_t(s);
        }
        if ((s = json_string_value(json_object_get(filter, "subject")))) {
            match_string(this, s, "subject");
        }
        if ((s = json_string_value(json_object_get(filter, "text")))) {
            match_string(this, s, "text");
        }
        if (JNOTNULL((val = json_object_get(filter, "threadIsFlagged")))) {
            e = val == json_true() ? this : search_expr_new(this, SEOP_NOT);
            e = search_expr_new(e, SEOP_MATCH);
            e->attr = search_attr_find("convflags");
            e->value.s = xstrdup("\\flagged");
        }
        if (JNOTNULL((val = json_object_get(filter, "threadIsUnread")))) {
            e = val == json_true() ? search_expr_new(this, SEOP_NOT): this;
            e = search_expr_new(e, SEOP_MATCH);
            e->attr = search_attr_find("convflags");
            e->value.s = xstrdup("\\seen");
        }
        if ((s = json_string_value(json_object_get(filter, "to")))) {
            match_string(this, s, "to");
        }
    }

    return this;
}

static void validatefilter(json_t *filter, const char *prefix, json_t *invalid)
{
    struct buf buf = BUF_INITIALIZER;
    json_t *arg, *val;
    const char *s;
    json_int_t num;
    int b;
    size_t i;

    if (!JNOTNULL(filter) || json_typeof(filter) != JSON_OBJECT) {
        json_array_append_new(invalid, json_string(prefix));
    }

    if (readprop_full(filter, prefix, "operator", 0, invalid, "s", &s) > 0) {
        if (strcmp("AND", s) && strcmp("OR", s) && strcmp("NOT", s)) {
            buf_printf(&buf, "%s.%s", prefix, "operator");
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            buf_reset(&buf);
        }

        arg = json_object_get(filter, "conditions");
        if (!json_array_size(arg)) {
            buf_printf(&buf, "%s.conditions", prefix);
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            buf_reset(&buf);
        }
        json_array_foreach(arg, i, val) {
            buf_printf(&buf, "%s.conditions[%zu]", prefix, i);
            validatefilter(val, buf_cstring(&buf), invalid);
            buf_reset(&buf);
        }

    } else {
        if (readprop_full(filter, prefix, "before", 0, invalid, "s", &s) > 0) {
            struct tm tm;
            const char *p = strptime(s, "%Y-%m-%dT%H:%M:%SZ", &tm);
            if (!p || *p) {
                buf_printf(&buf, "%s.before", prefix);
                json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                buf_reset(&buf);
            }
        }

        if (readprop_full(filter, prefix, "after", 0, invalid, "s", &s) > 0) {
            struct tm tm;
            const char *p = strptime(s, "%Y-%m-%dT%H:%M:%SZ", &tm);
            if (!p || *p) {
                buf_printf(&buf, "%s.after", prefix);
                json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                buf_reset(&buf);
            }
        }

        readprop_full(filter, prefix, "inMailbox", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "inMailboxOtherThan", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "minSize", 0, invalid, "I", &num);
        readprop_full(filter, prefix, "maxSize", 0, invalid, "I", &num);
        readprop_full(filter, prefix, "threadIsFlagged", 0, invalid, "b", &b);
        readprop_full(filter, prefix, "threadIsUnread", 0, invalid, "b", &b);
        readprop_full(filter, prefix, "isFlagged", 0, invalid, "b", &b);
        readprop_full(filter, prefix, "isUnread", 0, invalid, "b", &b);
        readprop_full(filter, prefix, "isAnswered", 0, invalid, "b", &b);
        readprop_full(filter, prefix, "isDraft", 0, invalid, "b", &b);
        readprop_full(filter, prefix, "hasAttachment", 0, invalid, "b", &b);
        readprop_full(filter, prefix, "attachmentName", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "text", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "from", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "to", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "cc", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "bcc", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "subject", 0, invalid, "s", &s);
        readprop_full(filter, prefix, "body", 0, invalid, "s", &s);

        arg = json_object_get(filter, "header");
        if (JNOTNULL(arg)) {
            switch (json_array_size(arg)) {
                case 2:
                    s = json_string_value(json_array_get(arg, 1));
                    if (!s || !strlen(s)) {
                        buf_printf(&buf, "%s.header[1]", prefix);
                        json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                        buf_reset(&buf);
                    }
                    /* fallthrough */
                case 1:
                    s = json_string_value(json_array_get(arg, 0));
                    if (!s || !strlen(s)) {
                        buf_printf(&buf, "%s.header[0]", prefix);
                        json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                        buf_reset(&buf);
                    }
                    break;
                default:
                    buf_printf(&buf, "%s.header", prefix);
                    json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                    buf_reset(&buf);
            }
        }
    }

    buf_free(&buf);
}

static struct sortcrit *buildsort(json_t *sort)
{
    json_t *val;
    const char *s, *p;
    size_t i;
    struct sortcrit *sortcrit;
    struct buf prop = BUF_INITIALIZER;

    if (!JNOTNULL(sort) || json_array_size(sort) == 0) {
        sortcrit = xzmalloc(2 * sizeof(struct sortcrit));
        sortcrit[0].flags |= SORT_REVERSE;
        sortcrit[0].key = SORT_ARRIVAL;
        sortcrit[1].key = SORT_SEQUENCE;
        return sortcrit;
    }

    sortcrit = xzmalloc((json_array_size(sort) + 1) * sizeof(struct sortcrit));

    json_array_foreach(sort, i, val) {
        s = json_string_value(val);
        p = strchr(s, ' ');
        buf_setmap(&prop, s, p - s);
        buf_cstring(&prop);

        if (!strcmp(p + 1, "desc")) {
            sortcrit[i].flags |= SORT_REVERSE;
        }

        /* Note: add any new sort criteria also to validatesort() */

        if (!strcmp(prop.s, "date")) {
            sortcrit[i].key = SORT_ARRIVAL;
        }
        if (!strcmp(prop.s, "from")) {
            sortcrit[i].key = SORT_FROM;
        }
        if (!strcmp(prop.s, "id")) {
            sortcrit[i].key = SORT_GUID;
        }
        if (!strcmp(prop.s, "isFlagged")) {
            sortcrit[i].key = SORT_HASFLAG;
            sortcrit[i].args.flag.name = xstrdup("\\flagged");
        }
        if (!strcmp(prop.s, "isUnread")) {
            sortcrit[i].key = SORT_HASFLAG;
            sortcrit[i].args.flag.name = xstrdup("\\seen");
            sortcrit[i].flags ^= SORT_REVERSE;
        }
        if (!strcmp(prop.s, "messageState")) {
            sortcrit[i].key = SORT_MODSEQ;
        }
        if (!strcmp(prop.s, "size")) {
            sortcrit[i].key = SORT_SIZE;
        }
        if (!strcmp(prop.s, "subject")) {
            sortcrit[i].key = SORT_SUBJECT;
        }
        if (!strcmp(prop.s, "threadIsFlagged")) {
            sortcrit[i].key = SORT_HASCONVFLAG;
            sortcrit[i].args.flag.name = xstrdup("\\flagged");
        }
        if (!strcmp(prop.s, "threadIsUnread")) {
            sortcrit[i].key = SORT_HASCONVFLAG;
            sortcrit[i].args.flag.name = xstrdup("\\seen");
            sortcrit[i].flags ^= SORT_REVERSE;
        }
        if (!strcmp(prop.s, "to")) {
            sortcrit[i].key = SORT_TO;
        }
    }

    sortcrit[json_array_size(sort)].key = SORT_SEQUENCE;

    buf_free(&prop);
    return sortcrit;
}

static void validatesort(json_t *sort, json_t *invalid, json_t *unsupported)
{
    struct buf buf = BUF_INITIALIZER;
    struct buf prop = BUF_INITIALIZER;
    json_t *val;
    const char *s, *p;
    size_t i;

    if (!JNOTNULL(sort)) {
        return;
    }

    if (json_typeof(sort) != JSON_ARRAY) {
        json_array_append_new(invalid, json_string("sort"));
        return;
    }

    json_array_foreach(sort, i, val) {
        buf_reset(&buf);
        buf_printf(&buf, "sort[%zu]", i);

        if ((s = json_string_value(val)) == NULL) {
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            continue;
        }

        p = strchr(s, ' ');
        if (!p || (strcmp(p + 1, "asc") && strcmp(p + 1, "desc"))) {
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            continue;
        }

        buf_setmap(&prop, s, p - s);
        buf_cstring(&prop);

        if (!strcmp(prop.s, "date"))
            continue;
        if (!strcmp(prop.s, "from"))
            continue;
        if (!strcmp(prop.s, "id"))
            continue;
        if (!strcmp(prop.s, "isFlagged"))
            continue;
        if (!strcmp(prop.s, "isUnread"))
            continue;
        if (!strcmp(prop.s, "messageState"))
            continue;
        if (!strcmp(prop.s, "size"))
            continue;
        if (!strcmp(prop.s, "subject"))
            continue;
        if (!strcmp(prop.s, "threadIsFlagged"))
            continue;
        if (!strcmp(prop.s, "threadIsUnread"))
            continue;
        if (!strcmp(prop.s, "to"))
            continue;

        json_array_append_new(unsupported, json_string(buf_cstring(&buf)));
    }

    buf_free(&buf);
    buf_free(&prop);
}

struct getmsglist_window {
    /* input arguments */
    int collapse;
    size_t position;
    const char *anchor;
    int anchor_off;
    size_t limit;

    /* output arguments */
    modseq_t highestmodseq;

    /* internal state */
    size_t mdcount;
    size_t anchor_pos;
};

static int jmapmsg_search(jmap_req_t *req, json_t *filter, json_t *sort,
                          struct getmsglist_window *window, int want_expunged,
                          size_t *total, size_t *total_threads,
                          json_t **messageids, json_t **expungedids,
                          json_t **threadids)
{
    hash_table ids = HASH_TABLE_INITIALIZER;
    hashu64_table cids = HASHU64_TABLE_INITIALIZER;
    struct index_state *state = NULL;
    search_query_t *query = NULL;
    struct sortcrit *sortcrit = NULL;
    struct searchargs *searchargs = NULL;
    struct index_init init;
    char *msgid = NULL;
    int i, r;

    assert(!want_expunged || expungedids);

    *total = 0;
    *messageids = json_pack("[]");
    *threadids = json_pack("[]");
    if (want_expunged) *expungedids = json_pack("[]");

    /* Build searchargs */
    searchargs = new_searchargs(NULL/*tag*/, GETSEARCH_CHARSET_FIRST,
                                &jmap_namespace, req->accountid, req->authstate, 0);
    searchargs->root = buildsearch(req, filter, NULL);

    /* Run the search query */
    memset(&init, 0, sizeof(init));
    init.userid = req->accountid;
    init.authstate = req->authstate;
    init.want_expunged = want_expunged;

    r = index_open(req->inboxname, &init, &state);
    if (r) goto done;

    query = search_query_new(state, searchargs);
    query->sortcrit = sortcrit = buildsort(sort);
    query->multiple = 1;
    query->need_ids = 1;
    query->verbose = 1;
    query->want_expunged = want_expunged;

    r = search_query_run(query);
    if (r) goto done;

    /* Initialize window state */
    window->mdcount = query->merged_msgdata.count;
    window->anchor_pos = (size_t)-1;
    window->highestmodseq = 0;

    memset(&ids, 0, sizeof(hash_table));
    construct_hash_table(&ids, window->mdcount + 1, 0);

    memset(&cids, 0, sizeof(hashu64_table));
    construct_hashu64_table(&cids, query->merged_msgdata.count/4+4,0);

    *total_threads = 0;

    for (i = 0 ; i < query->merged_msgdata.count ; i++) {
        MsgData *md = ptrarray_nth(&query->merged_msgdata, i);
        search_folder_t *folder = md->folder;
        json_t *msg = NULL;
        size_t idcount = json_array_size(*messageids);

        if (!folder) continue;

        /* Ignore expunged messages, if not requested by caller */
        int is_expunged = md->system_flags & (FLAG_EXPUNGED|FLAG_DELETED);
        if (is_expunged && !want_expunged)
            goto doneloop;

        /* Make sure we don't report any hidden messages */
        int rights = jmap_myrights_byname(req, folder->mboxname);
        if (!(rights & ACL_READ))
            goto doneloop;

        free(msgid);
        msgid = jmap_msgid(&md->guid);

        /* Have we seen this message already? */
        if (hash_lookup(msgid, &ids))
            goto doneloop;

        /* Add the message the list of reported messages */
        hash_insert(msgid, (void*)1, &ids);

        /* Collapse threads, if requested */
        if (window->collapse && hashu64_lookup(md->cid, &cids))
            goto doneloop;

        /* OK, that's a legit message */
        (*total)++;

        /* Keep track of conversation ids, inside and outside the window */
        if (!hashu64_lookup(md->cid, &cids)) {
            (*total_threads)++;
            hashu64_insert(md->cid, (void*)1, &cids);
        }

        /* Check if the message is in the search window */
        if (window->anchor) {
            if (!strcmp(msgid, window->anchor)) {
                /* This message is the anchor. Recalculate the search result */
                json_t *anchored_ids = json_pack("[]");
                json_t *anchored_cids = json_pack("[]");
                size_t j;

                /* Set countdown to enter the anchor window */
                if (window->anchor_off < 0) {
                    window->anchor_pos = -window->anchor_off;
                } else {
                    window->anchor_pos = 0;
                }

                /* Readjust the message and thread list */
                for (j = idcount - window->anchor_off; j < idcount; j++) {
                    json_array_append(anchored_ids, json_array_get(*messageids, j));
                    json_array_append(anchored_cids, json_array_get(*threadids, j));
                }
                /* Adjust the window position for this anchor. This is
                 * "[...] the 0-based index of the first result in the
                 * threadIds array within the complete list". */
                window->position = json_array_size(*threadids) -
                                   json_array_size(anchored_cids);

                json_decref(*messageids);
                *messageids = anchored_ids;
                json_decref(*threadids);
                *threadids = anchored_cids;

                /* Reset message counter */
                idcount = json_array_size(*messageids);
            }
            if (window->anchor_pos != (size_t)-1 && window->anchor_pos) {
                /* Found the anchor but haven't yet entered its window */
                window->anchor_pos--;
                /* But this message still counts to the window position */
                window->position++;
                goto doneloop;
            }
        }
        else if (window->position && *total < window->position + 1) {
            goto doneloop;
        }

        if (window->limit && idcount && window->limit <= idcount)
            goto doneloop;

        /* Keep track of the highest modseq */
        if (window->highestmodseq < md->modseq)
            window->highestmodseq = md->modseq;

        /* Check if the message is expunged in all mailboxes */
        r = jmapmsg_isexpunged(req, msgid, &is_expunged);
        if (r) goto done;

        /* Add the message id to the result */
        if (is_expunged && expungedids) {
            json_array_append_new(*expungedids, json_string(msgid));
        } else {
            json_array_append_new(*messageids, json_string(msgid));
        }

        /* Add the thread id */
        if (window->collapse)
            hashu64_insert(md->cid, (void*)1, &cids);
        char *thrid = jmap_thrid(md->cid);
        json_array_append_new(*threadids, json_string(thrid));
        free(thrid);


doneloop:
        if (msg) json_decref(msg);
    }

done:
    free(msgid);
    free_hash_table(&ids, NULL);
    free_hashu64_table(&cids, NULL);
    if (sortcrit) freesortcrit(sortcrit);
    if (query) search_query_free(query);
    if (searchargs) freesearchargs(searchargs);
    if (state) {
        state->mailbox = NULL;
        index_close(&state);
    }
    if (r) {
        json_decref(*messageids);
        *messageids = NULL;
        json_decref(*threadids);
        *threadids = NULL;
    }
    return r;
}

static int getMessageList(jmap_req_t *req)
{
    int r;
    int fetchthreads = 0, fetchmsgs = 0, fetchsnippets = 0;
    json_t *filter, *sort;
    json_t *messageids = NULL, *threadids = NULL, *collapse = NULL, *item, *res;
    struct getmsglist_window window;
    json_int_t i = 0;
    size_t total, total_threads;

    /* Parse and validate arguments. */
    json_t *invalid = json_pack("[]");
    json_t *unsupported = json_pack("[]");

    /* filter */
    filter = json_object_get(req->args, "filter");
    if (JNOTNULL(filter)) {
        validatefilter(filter, "filter", invalid);
    }

    /* sort */
    sort = json_object_get(req->args, "sort");
    if (JNOTNULL(sort)) {
        validatesort(sort, invalid, unsupported);
    }

    /* windowing */
    memset(&window, 0, sizeof(struct getmsglist_window));
    if ((collapse = json_object_get(req->args, "collapseThreads"))) {
        readprop(req->args, "collapseThreads", 0, invalid, "b", &window.collapse);
    }
    readprop(req->args, "anchor", 0, invalid, "s", &window.anchor);
    readprop(req->args, "anchorOffset", 0, invalid, "i", &window.anchor_off);

    if (readprop(req->args, "position", 0, invalid, "I", &i) > 0) {
        if (i < 0) json_array_append_new(invalid, json_string("position"));
        window.position = i;
    }

    if (readprop(req->args, "limit", 0, invalid, "I", &i) > 0) {
        if (i < 0) json_array_append_new(invalid, json_string("limit"));
        window.limit = i;
    }

    readprop(req->args, "fetchThreads", 0, invalid, "b", &fetchthreads);
    readprop(req->args, "fetchMessages", 0, invalid, "b", &fetchmsgs);
    readprop(req->args, "fetchSearchSnippets", 0, invalid, "b", &fetchsnippets);


    /* Bail out for argument errors */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(invalid);

    if (json_array_size(unsupported)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "unsupportedSort", "sort", unsupported);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(unsupported);

    r = jmapmsg_search(req, filter, sort, &window, 0, &total, &total_threads,
                       &messageids, /*expungedids*/NULL, &threadids);
    if (r == IMAP_NOTFOUND) {
        json_t *err = json_pack("{s:s}", "type", "cannotDoFilter");
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    else if (r) goto done;

    /* Prepare response. */
    res = json_pack("{}");
    json_object_set_new(res, "accountId", json_string(req->accountid));
    if (JNOTNULL(collapse)) {
        json_object_set_new(res, "collapseThreads", json_boolean(window.collapse));
    } else {
        json_object_set_new(res, "collapseThreads", json_null());
    }
    json_object_set_new(res, "state", jmap_getstate(req, 0/*mbtype*/));
    json_object_set_new(res, "canCalculateUpdates", json_false()); /* TODO getMessageListUpdates */
    json_object_set_new(res, "position", json_integer(window.position));
    json_object_set_new(res, "total", json_integer(total));
    json_object_set(res, "filter", filter);
    json_object_set(res, "sort", sort);
    json_object_set(res, "messageIds", messageids);
    json_object_set(res, "threadIds", threadids);

    item = json_pack("[]");
    json_array_append_new(item, json_string("messageList"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

    // fetchmsgs is implicit in fetchthreads, because we fetch them all
    if (fetchthreads) {
        if (json_array_size(threadids)) {
            struct jmap_req subreq = *req;
            subreq.args = json_pack("{}");
            json_object_set(subreq.args, "ids", threadids);
            json_object_set_new(subreq.args, "accountId", json_string(req->accountid));
            if (fetchmsgs) json_object_set_new(subreq.args, "fetchMessages", json_true());
            json_t *props = json_object_get(req->args, "fetchMessageProperties");
            if (props) json_object_set(subreq.args, "fetchMessageProperties", props);
            r = getThreads(&subreq);
            json_decref(subreq.args);
            if (r) goto done;
        }
    }
    else if (fetchmsgs) {
        if (json_array_size(messageids)) {
            struct jmap_req subreq = *req;
            subreq.args = json_pack("{}");
            json_object_set(subreq.args, "ids", messageids);
            json_object_set_new(subreq.args, "accountId", json_string(req->accountid));
            json_t *props = json_object_get(req->args, "fetchMessageProperties");
            if (props) json_object_set(subreq.args, "properties", props);
            r = getMessages(&subreq);
            json_decref(subreq.args);
            if (r) goto done;
        }
    }

    if (fetchsnippets) {
        if (json_array_size(messageids)) {
            struct jmap_req subreq = *req;
            subreq.args = json_pack("{}");
            json_object_set_new(subreq.args, "accountId", json_string(req->accountid));
            json_object_set(subreq.args, "messageIds", messageids);
            json_object_set(subreq.args, "filter", filter);
            r = getSearchSnippets(&subreq);
            json_decref(subreq.args);
            if (r) goto done;
        }
    }

done:
    if (messageids) json_decref(messageids);
    if (threadids) json_decref(threadids);
    return r;
}

static int getMessageUpdates(jmap_req_t *req)
{
    int r = 0, pe;
    int fetch = 0;
    json_t *filter = NULL, *sort = NULL;
    json_t *changed = NULL, *removed = NULL, *invalid = NULL, *item = NULL, *res = NULL, *threads = NULL;
    json_int_t max = 0;
    size_t total, total_threads;
    int has_more = 0;
    json_t *oldstate, *newstate;
    struct getmsglist_window window;
    const char *since;

    /* Parse and validate arguments. */
    invalid = json_pack("[]");

    /* sinceState */
    pe = readprop(req->args, "sinceState", 1, invalid, "s", &since);
    if (pe > 0 && !atomodseq_t(since)) {
        json_array_append_new(invalid, json_string("sinceState"));
    }
    /* maxChanges */
    memset(&window, 0, sizeof(struct getmsglist_window));
    readprop(req->args, "maxChanges", 0, invalid, "I", &max);
    if (max < 0) json_array_append_new(invalid, json_string("maxChanges"));
    window.limit = max;
    /* fetch */
    readprop(req->args, "fetchRecords", 0, invalid, "b", &fetch);

    /* Bail out for argument errors */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }

    json_decref(invalid);

    /* FIXME need to store deletemodseq in counters */

    /* Search for updates */
    filter = json_pack("{s:s}", "sinceMessageState", since);
    sort = json_pack("[s]", "messageState asc");

    r = jmapmsg_search(req, filter, sort, &window, /*want_expunge*/1,
                       &total, &total_threads, &changed, &removed, &threads);
    if (r == IMAP_NOTFOUND) {
        json_t *err = json_pack("{s:s}", "type", "cannotDoFilter");
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    else if (r) goto done;

    has_more = (json_array_size(changed) + json_array_size(removed)) < total;
    oldstate = json_string(since);
    if (has_more || json_array_size(changed) || json_array_size(removed)) {
        newstate = jmap_fmtstate(window.highestmodseq);
    }
    else {
        newstate = json_incref(oldstate);
    }

    /* Prepare response. */
    res = json_pack("{}");
    json_object_set_new(res, "accountId", json_string(req->accountid));
    json_object_set_new(res, "oldState", oldstate);
    json_object_set_new(res, "newState", newstate);
    json_object_set_new(res, "hasMoreUpdates", json_boolean(has_more));
    json_object_set(res, "changed", changed);
    json_object_set(res, "removed", removed);

    item = json_pack("[]");
    json_array_append_new(item, json_string("messageUpdates"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

    if (fetch) {
        struct jmap_req subreq = *req;
        subreq.args = json_pack("{}");
        json_object_set(subreq.args, "ids", changed);
        json_t *props = json_object_get(req->args, "fetchRecordProperties");
        if (props) json_object_set(subreq.args, "properties", props);
        json_object_set(subreq.args, "accountId", json_string(req->accountid));
        r = getMessages(&subreq);
        json_decref(subreq.args);
        if (r) goto done;
    }

done:
    if (sort) json_decref(sort);
    if (filter) json_decref(filter);
    if (threads) json_decref(threads);
    if (changed) json_decref(changed);
    if (removed) json_decref(removed);
    return r;
}

static int getThreadUpdates(jmap_req_t *req)
{
    int pe, fetch = 0, has_more = 0, r = 0;
    json_int_t max = 0;
    json_t *invalid, *item, *res, *oldstate, *newstate;
    json_t *changed = NULL;
    json_t *removed = NULL;
    json_t *threads = NULL;
    json_t *val;
    size_t total, total_threads, i;
    struct getmsglist_window window;
    const char *since;
    conversation_t *conv = NULL;

    /* Parse and validate arguments. */
    invalid = json_pack("[]");

    /* sinceState */
    pe = readprop(req->args, "sinceState", 1, invalid, "s", &since);
    if (pe > 0 && !atomodseq_t(since)) {
        json_array_append_new(invalid, json_string("sinceState"));
    }
    /* maxChanges */
    memset(&window, 0, sizeof(struct getmsglist_window));
    readprop(req->args, "maxChanges", 0, invalid, "I", &max);
    if (max < 0) json_array_append_new(invalid, json_string("maxChanges"));
    window.limit = max;

    /* fetch */
    readprop(req->args, "fetchRecords", 0, invalid, "b", &fetch);

    /* Bail out for argument errors */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(invalid);

    /* FIXME need deletedmodseq in counters */

    /* Search for message updates and collapse threads */
    json_t *filter = json_pack("{s:s}", "sinceMessageState", since);
    json_t *sort = json_pack("[s]", "messageState asc");
    window.collapse = 1;
    r = jmapmsg_search(req, filter, sort, &window, /*want_expunge*/1,
                       &total, &total_threads, &changed, &removed, &threads);
    json_decref(filter);
    json_decref(sort);
    if (r == IMAP_NOTFOUND) {
        json_t *err = json_pack("{s:s}", "type", "cannotDoFilter");
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    else if (r) goto done;

    /* Split the collapsed threads into changed and removed - the values from
       jmapmsg_search will be msgids */
    if (changed) json_decref(changed);
    if (removed) json_decref(removed);
    changed = json_pack("[]");
    removed = json_pack("[]");

    json_array_foreach(threads, i, val) {
        const char *threadid = json_string_value(val);
        conversation_id_t cid = jmap_decode_thrid(threadid);
        if (!cid) continue;

        r = conversation_load(req->cstate, cid, &conv);
        if (!conv) continue;
        if (r) {
            if (r == CYRUSDB_NOTFOUND) {
                continue;
            } else {
                goto done;
            }
        }

        json_array_append(conv->thread ? changed : removed, val);

        conversation_free(conv);
        conv = NULL;
    }

    has_more = (json_array_size(changed) + json_array_size(removed)) < total_threads;

    if (has_more) {
        newstate = jmap_fmtstate(window.highestmodseq);
    } else {
        newstate = jmap_fmtstate(jmap_highestmodseq(req, 0/*mbtype*/));
    }
    oldstate = json_string(since);

    /* Prepare response. */
    res = json_pack("{}");
    json_object_set_new(res, "accountId", json_string(req->accountid));
    json_object_set_new(res, "oldState", oldstate);
    json_object_set_new(res, "newState", newstate);
    json_object_set_new(res, "hasMoreUpdates", json_boolean(has_more));
    json_object_set(res, "changed", changed);
    json_object_set(res, "removed", removed);

    item = json_pack("[]");
    json_array_append_new(item, json_string("threadUpdates"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

    if (fetch) {
        if (json_array_size(changed)) {
            struct jmap_req subreq = *req;
            subreq.args = json_pack("{}");
            json_object_set(subreq.args, "ids", changed);
            json_t *props = json_object_get(req->args, "fetchRecordProperties");
            if (props) json_object_set(subreq.args, "properties", props);
            json_object_set(subreq.args, "fetchMessages", json_false());
            r = getThreads(&subreq);
            json_decref(subreq.args);
            if (r) goto done;
        }
    }

done:
    if (conv) conversation_free(conv);
    if (changed) json_decref(changed);
    if (removed) json_decref(removed);
    if (threads) json_decref(threads);
    return r;
}

static int makesnippet(struct mailbox *mbox __attribute__((unused)),
                       uint32_t uid __attribute__((unused)),
                       int part, const char *s, void *rock)
{
    const char *propname = NULL;
    json_t *snippet = rock;


    if (part == SEARCH_PART_SUBJECT) {
        propname = "subject";
    }
    else if (part == SEARCH_PART_BODY) {
        propname = "preview";
    }

    if (propname) {
        json_object_set_new(snippet, propname, json_string(s));
    }

    return 0;
}

static int jmapmsg_snippets(jmap_req_t *req, json_t *filter, json_t *messageids,
                            json_t **snippets, json_t **notfound)
{
    struct index_state *state = NULL;
    void *intquery = NULL;
    search_builder_t *bx = NULL;
    search_text_receiver_t *rx = NULL;
    struct mailbox *mbox = NULL;
    struct searchargs *searchargs = NULL;
    struct index_init init;
    const char *msgid;
    json_t *snippet = NULL;
    int r = 0;
    json_t *val;
    size_t i;
    char *mboxname = NULL;
    static search_snippet_markup_t markup = { "<mark>", "</mark>", "..." };

    *snippets = json_pack("[]");
    *notfound = json_pack("[]");

    /* Build searchargs */
    searchargs = new_searchargs(NULL/*tag*/, GETSEARCH_CHARSET_FIRST,
                                &jmap_namespace, req->userid, req->authstate, 0);
    searchargs->root = buildsearch(req, filter, NULL);

    /* Build the search query */
    memset(&init, 0, sizeof(init));
    init.userid = req->userid;
    init.authstate = req->authstate;

    r = index_open(req->inboxname, &init, &state);
    if (r) goto done;

    bx = search_begin_search(state->mailbox, SEARCH_MULTIPLE);
    if (!bx) {
        r = IMAP_INTERNAL;
        goto done;
    }

    search_build_query(bx, searchargs->root);
    if (!bx->get_internalised) {
        r = IMAP_INTERNAL;
        goto done;
    }
    intquery = bx->get_internalised(bx);
    search_end_search(bx);
    if (!intquery) {
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Set up snippet callback context */
    snippet = json_pack("{}");
    rx = search_begin_snippets(intquery, 0, &markup, makesnippet, snippet);
    if (!rx) {
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Convert the snippets */
    json_array_foreach(messageids, i, val) {
        message_t *msg;
        msgrecord_t *mr = NULL;
        uint32_t uid;

        msgid = json_string_value(val);

        r = jmapmsg_find(req, msgid, &mboxname, &uid);
        if (r) {
            if (r == IMAP_NOTFOUND) {
                json_array_append_new(*notfound, json_string(msgid));
            }
            r = 0;
            continue;
        }

        r = jmap_openmbox(req, mboxname, &mbox, 0);
        if (r) goto done;

        r = rx->begin_mailbox(rx, mbox, /*incremental*/0);

        r = msgrecord_find(mbox, uid, &mr);
        if (r) goto doneloop;

        r = msgrecord_get_message(mr, &msg);
        if (r) goto doneloop;

        json_object_set_new(snippet, "messageId", json_string(msgid));
        json_object_set_new(snippet, "subject", json_null());
        json_object_set_new(snippet, "preview", json_null());
        index_getsearchtext(msg, rx, /*snippet*/1);
        json_array_append_new(*snippets, json_deep_copy(snippet));
        json_object_clear(snippet);
        msgrecord_unref(&mr);

        r = rx->end_mailbox(rx, mbox);
        if (r) goto done;

doneloop:
        if (mr) msgrecord_unref(&mr);
        jmap_closembox(req, &mbox);
        free(mboxname);
        mboxname = NULL;
    }

    if (!json_array_size(*notfound)) {
        json_decref(*notfound);
        *notfound = json_null();
    }

done:
    if (rx) search_end_snippets(rx);
    if (snippet) json_decref(snippet);
    if (intquery) search_free_internalised(intquery);
    if (mboxname) free(mboxname);
    if (mbox) jmap_closembox(req, &mbox);
    if (searchargs) freesearchargs(searchargs);
    if (state) {
        state->mailbox = NULL;
        index_close(&state);
    }

    return r;
}

static int filter_contains_text(json_t *filter)
{
    if (JNOTNULL(filter)) {
        json_t *val;
        size_t i;

        if (JNOTNULL(json_object_get(filter, "text"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "subject"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "body"))) {
            return 1;
        }

        /* We don't generate snippets for headers, but we
         * might find header text in the body or subject again. */
        if (JNOTNULL(json_object_get(filter, "header"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "from"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "to"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "cc"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "bcc"))) {
            return 1;
        }

        json_array_foreach(json_object_get(filter, "conditions"), i, val) {
            if (filter_contains_text(val)) {
                return 1;
            }
        }
    }
    return 0;
}

static int getSearchSnippets(jmap_req_t *req)
{
    int r = 0;
    json_t *filter, *messageids, *val, *snippets, *notfound, *res, *item;
    const char *s;
    struct buf buf = BUF_INITIALIZER;
    size_t i;

    /* Parse and validate arguments. */
    json_t *invalid = json_pack("[]");

    /* filter */
    filter = json_object_get(req->args, "filter");
    if (JNOTNULL(filter)) {
        validatefilter(filter, "filter", invalid);
    }

    /* messageIds */
    messageids = json_object_get(req->args, "messageIds");
    json_array_foreach(messageids, i, val) {
        if (!(s = json_string_value(val)) || !strlen(s)) {
            buf_printf(&buf, "messageIds[%zu]", i);
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            buf_reset(&buf);
        }
    }
    if (!json_is_array(messageids)) {
        json_array_append_new(invalid, json_string("messageIds"));
    }

    /* Bail out for argument errors */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(invalid);

    if (json_array_size(messageids) && filter_contains_text(filter)) {
        /* Render snippets */
        r = jmapmsg_snippets(req, filter, messageids, &snippets, &notfound);
        if (r) goto done;
    } else {
        /* Trivial, snippets cant' match */
        snippets = json_pack("[]");
        notfound = json_null();

        json_array_foreach(messageids, i, val) {
            json_array_append_new(snippets, json_pack("{s:s s:n s:n}",
                        "messageId", json_string_value(val),
                        "subject", "preview"));
        }
    }

    /* Prepare response. */
    res = json_pack("{}");
    json_object_set_new(res, "accountId", json_string(req->accountid));
    json_object_set_new(res, "list", snippets);
    json_object_set_new(res, "notFound", notfound);
    json_object_set(res, "filter", filter);

    item = json_pack("[]");
    json_array_append_new(item, json_string("searchSnippets"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

done:
    buf_free(&buf);
    return r;
}

static int jmapmsg_threads(jmap_req_t *req, json_t *threadids,
                           json_t **threads, json_t **notfound)
{
    conversation_t *conv = NULL;
    json_t *val;
    size_t i;
    int r = 0;

    *threads = json_pack("[]");
    *notfound = json_pack("[]");

    json_array_foreach(threadids, i, val) {
        conversation_id_t cid = 0;
        conv_thread_t *thread;

        const char *threadid = json_string_value(val);
        cid = jmap_decode_thrid(threadid);

        if (cid) r = conversation_load(req->cstate, cid, &conv);
        if (r) goto done;
        if (!conv) {
            json_array_append_new(*notfound, json_string(threadid));
            continue;
        }

        json_t *ids = json_pack("[]");
        for (thread = conv->thread; thread; thread = thread->next) {
            char *msgid = jmap_msgid(&thread->guid);
            json_array_append_new(ids, json_string(msgid));
            free(msgid);
        }

        json_t *jthread = json_pack("{s:s s:o}", "id", threadid, "messageIds", ids);
        json_array_append_new(*threads, jthread);

        conversation_free(conv);
        conv = NULL;
    }

    if (!json_array_size(*notfound)) {
        json_decref(*notfound);
        *notfound = json_null();
    }

    r = 0;

done:
    if (conv) conversation_free(conv);
    if (r) {
        json_decref(*threads);
        *threads = NULL;
        json_decref(*notfound);
        *notfound = NULL;
    }

    return r;
}

static int getThreads(jmap_req_t *req)
{
    int r, fetchmsgs = 0;
    json_t *res, *item, *val, *threadids, *threads, *notfound;
    const char *s;
    struct buf buf = BUF_INITIALIZER;
    size_t i;

    /* Parse and validate arguments. */
    json_t *invalid = json_pack("[]");

    /* ids */
    threadids = json_object_get(req->args, "ids");
    json_array_foreach(threadids, i, val) {
        if (!(s = json_string_value(val)) || !strlen(s)) {
            buf_printf(&buf, "ids[%zu]", i);
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            buf_reset(&buf);
        }
    }
    if (JNOTNULL(threadids) && !json_is_array(threadids)) {
        json_array_append_new(invalid, json_string("ids"));
    }

    readprop(req->args, "fetchMessages", 0, invalid, "b", &fetchmsgs);

    /* Bail out for argument errors */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(invalid);

    /* Find threads */
    r = jmapmsg_threads(req, threadids, &threads, &notfound);
    if (r) goto done;

    /* Prepare response. */
    res = json_pack("{}");
    json_object_set_new(res, "state", jmap_getstate(req, 0/*mbtype*/));
    json_object_set_new(res, "accountId", json_string(req->accountid));
    json_object_set_new(res, "list", threads);
    json_object_set_new(res, "notFound", notfound);

    item = json_pack("[]");
    json_array_append_new(item, json_string("threads"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

    if (fetchmsgs) {
        struct jmap_req subreq = *req;
        subreq.args = json_pack("{}");
        json_t *messageids = json_pack("[]");
        size_t i;
        json_t *item;
        json_array_foreach(threads, i, item) {
            size_t j;
            json_t *id;
            json_array_foreach(json_object_get(item, "messageIds"), j, id) {
                json_array_append(messageids, id);
            }
        }
        json_object_set_new(subreq.args, "ids", messageids);
        json_t *props = json_object_get(req->args, "fetchMessageProperties");
        if (props) json_object_set(subreq.args, "properties", props);
        json_object_set_new(subreq.args, "accountId", json_string(req->accountid));
        r = getMessages(&subreq);
        json_decref(subreq.args);
        if (r) goto done;
    }

done:
    buf_free(&buf);
    return r;
}

static int getMessages(jmap_req_t *req)
{
    int r = 0;
    json_t *list = json_pack("[]");
    json_t *notfound = json_pack("[]");
    json_t *invalid = json_pack("[]");
    size_t i;
    json_t *ids, *val, *properties, *res, *item;
    hash_table *props = NULL;

    /* ids */
    ids = json_object_get(req->args, "ids");
    if (ids && json_is_array(ids)) {
        json_array_foreach(ids, i, val) {
            if (json_typeof(val) != JSON_STRING) {
                struct buf buf = BUF_INITIALIZER;
                buf_printf(&buf, "ids[%llu]", (unsigned long long) i);
                json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                buf_reset(&buf);
                continue;
            }
        }
    } else if (JNOTNULL(ids)) {
        json_array_append_new(invalid, json_string("ids"));
    }

    /* properties */
    properties = json_object_get(req->args, "properties");
    if (properties && json_array_size(properties)) {
        props = xzmalloc(sizeof(struct hash_table));
        construct_hash_table(props, json_array_size(properties) + 1, 0);
        json_array_foreach(properties, i, val) {
            if (json_string_value(val)) {
                hash_insert(json_string_value(val), (void*)1, props);
            }
        }
    }

    /* Bail out for any property errors. */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(invalid);

    /* Lookup and convert ids */
    json_array_foreach(ids, i, val) {
        const char *id = json_string_value(val);
        char *mboxname = NULL;
        msgrecord_t *mr = NULL;
        uint32_t uid;
        json_t *msg = NULL;
        struct mailbox *mbox = NULL;

        if (id[0] == '#') {
            const char *newid = hash_lookup(id + 1, &req->idmap->messages);
            if (!newid) {
                r = IMAP_NOTFOUND;
                goto doneloop;
            }
            id = newid;
        }

        r = jmapmsg_find(req, id, &mboxname, &uid);
        if (r) goto doneloop;

        r = jmap_openmbox(req, mboxname, &mbox, 0);
        if (r) goto done;

        r = msgrecord_find(mbox, uid, &mr);
        if (!r) jmapmsg_from_record(req, props, mr, &msg);

        jmap_closembox(req, &mbox);

doneloop:
        if (r == IMAP_NOTFOUND) {
            json_array_append_new(notfound, json_string(id));
            r = 0;
            continue;
        }
        if (mboxname) free(mboxname);
        if (msg) {
            json_array_append_new(list, msg);
        } else {
            json_array_append_new(notfound, json_string(id));
        }
        if (mr) msgrecord_unref(&mr);
        if (r) goto done;
    }

    if (!json_array_size(notfound)) {
        json_decref(notfound);
        notfound = json_null();
    }

    res = json_pack("{}");
    json_object_set_new(res, "state", jmap_getstate(req, 0/*mbtype*/));
    json_object_set_new(res, "accountId", json_string(req->accountid));
    json_object_set(res, "list", list);
    json_object_set(res, "notFound", notfound);

    item = json_pack("[]");
    json_array_append_new(item, json_string("messages"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));

    json_array_append_new(req->response, item);

done:
    if (props) {
        free_hash_table(props, NULL);
        free(props);
    }
    json_decref(list);
    json_decref(notfound);
    return r;
}

static int jmap_validate_emailer(json_t *emailer,
                                 const char *prefix,
                                 int parseaddr,
                                 json_t *invalid)
{
    struct buf buf = BUF_INITIALIZER;
    int r = 1;
    json_t *val;
    int valid = 1;

    val = json_object_get(emailer, "name");
    if (!val || json_typeof(val) != JSON_STRING) {
        buf_printf(&buf, "%s.%s", prefix, "name");
        json_array_append_new(invalid, json_string(buf_cstring(&buf)));
        buf_reset(&buf);
        r = 0;
    }
    val = json_object_get(emailer, "email");
    if (val && parseaddr && json_string_value(val)) {
        struct address *addr = NULL;
        parseaddr_list(json_string_value(val), &addr);
        if (!addr || addr->invalid || !addr->mailbox || !addr->domain || addr->next) {
            valid = 0;
        }
        parseaddr_free(addr);
    }
    if (!val || json_typeof(val) != JSON_STRING || !valid) {
        buf_printf(&buf, "%s.%s", prefix, "email");
        json_array_append_new(invalid, json_string(buf_cstring(&buf)));
        buf_reset(&buf);
        r = 0;
    }

    buf_free(&buf);
    return r;
}

static int jmapmsg_get_messageid(jmap_req_t *req, const char *id, char **messageid)
{
    char *mboxname = NULL;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    uint32_t uid;
    struct buf buf = BUF_INITIALIZER;
    uint32_t flags;
    int r;

    r = jmapmsg_find(req, id, &mboxname, &uid);
    if (r) goto done;

    r = jmap_openmbox(req, mboxname, &mbox, 0);
    if (r) goto done;

    r = msgrecord_find(mbox, uid, &mr);
    if (!r) {

        r = msgrecord_get_systemflags(mr, &flags);
        if (r || (flags & (FLAG_EXPUNGED|FLAG_DELETED))) {
            if (!r) r = IMAP_NOTFOUND;
            goto done;
        }

        message_t *m = NULL;
        r = msgrecord_get_message(mr, &m);
        if (r) goto done;

        r = message_get_messageid(m, &buf);
        if (r) goto done;
    }

    buf_cstring(&buf);
    *messageid = buf_release(&buf);

done:
    if (mr) msgrecord_unref(&mr);
    if (mbox) jmap_closembox(req, &mbox);
    if (mboxname) free(mboxname);
    buf_free(&buf);
    return r;
}

static int jmapmsg_to_mime(jmap_req_t *req, FILE *out, json_t *msg);

#define JMAPMSG_HEADER_TO_MIME(k, v) \
    { \
       const char *_v = (v); \
       char *s = charset_encode_mimeheader(_v, strlen(_v)); \
       fprintf(out, "%s: %s\r\n", k, s); \
       free(s); \
    }

#define JMAPMSG_EMAILER_TO_MIME(b, m) \
    { \
        json_t *_m = (m); \
        const char *name = json_string_value(json_object_get(_m, "name")); \
        const char *email = json_string_value(json_object_get(_m, "email")); \
        if (strlen(name) && email) { \
            char *xname = charset_encode_mimeheader(name, strlen(name)); \
            buf_printf(b, "%s <%s>", xname, email); \
            free(xname); \
        } else if (email) { \
            buf_appendcstr(b, email); \
        } \
    }

static char* _make_boundary()
{
    char *boundary, *p, *q;

    boundary = xstrdup(makeuuid());
    for (p = boundary, q = boundary; *p; p++) {
        if (*p != '-') *q++ = *p;
    }
    *q = 0;

    return boundary;
}

static const char* split_plain(const char *s, size_t limit)
{
    const char *p = s + limit;
    while (p > s && !isspace(*p))
        p--;
    if (p == s)
        p = s + limit;
    return p;
}

static const char* split_html(const char *s, size_t limit)
{
    const char *p = s + limit;
    while (p > s && !isspace(*p) && *p != '<')
        p--;
    if (p == s)
        p = s + limit;
    return p;
}

static int writetext(const char *s, FILE *out,
                     const char* (*split)(const char *s, size_t limit))
{
    /*
     * RFC 5322 - 2.1.1.  Line Length Limits
     * There are two limits that this specification places on the number of
     * characters in a line.  Each line of characters MUST be no more than
     * 998 characters, and SHOULD be no more than 78 characters, excluding
     * the CRLF.
     */
    const char *p = s;
    const char *top = p + strlen(p);

    while (p < top) {
        const char *q = strchr(p, '\n');
        q = q ? q + 1 : top;

        if (q - p > 998) {
            /* Could split on 1000 bytes but let's have some leeway */
            q = split(p, 998);
        }

        if (fwrite(p, 1, q - p, out) < ((size_t)(q - p)))
            return -1;
        if (q < top && fputc('\n', out) == EOF)
            return -1;

        p = q;
    }

    return 0;
}

static int is_7bit_safe(const char *base, size_t len, const char *boundary __attribute__((unused)))
{
    /* XXX check if boundary exists in base - base+len ? */

    size_t linelen = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        if (base[i] == '\n') linelen = 0;
        else linelen++;

        // any long lines, reject
        if (linelen > 80) return 0;

        // any 8bit, reject
        if (base[i] & 0x80) return 0;

        // xxx - boundary match ?
    }

    return 1;
}

#define MIME_MAX_HEADER_LENGTH 78

static int writeparam(FILE *out, const char *name, const char *value,
                      int quote, int is_extended)
{
    /* Normalize arguments */
    if (quote) quote = 1;
    if (is_extended) is_extended = 1;

    if (strlen(name) + strlen(value) + 4 + quote*2 < MIME_MAX_HEADER_LENGTH) {
        /* It all fits in one line, great! */
        return fprintf(out, ";\r\n\t%s=%s%s%s",
                name,
                quote ? "\"" : "",
                value,
                quote ? "\"" : "");
    }
    else if (!is_extended && strchr(value, '\r')) {
        /* The non-extended value already includes continuations  */
        const char *p = value, *top = value + strlen(value);
        int section = 0;

        do {
            const char *q = strchr(p, '\r');
            if (!q) q = top;
            fprintf(out, ";\r\n\t%s*%d=", name, section);
            if (quote) fputc('"', out);
            fwrite(p, 1, q - p, out);
            if (quote) fputc('"', out);
            p = q + 3;
            section++;
        } while (p < top);

        return 0;
    }
    else {
        /* We have to break the values by ourselves into continuations */
        const char *p = value, *top = value + strlen(value);
        int section = 0;
        struct buf buf = BUF_INITIALIZER;

        while (p < top) {
            buf_printf(&buf, ";\r\n\t%s%s%d%s=", name,
                    is_extended ? "" : "*",
                    section,
                    is_extended ? "*" : "");

            size_t n = fwrite(buf_base(&buf), 1, buf_len(&buf), out);
            if (!n) return -1;
            buf_reset(&buf);

            if (n > MIME_MAX_HEADER_LENGTH) {
                /* We already overran the maximum length by just writing the
                 * parameter name. Let's insert a continuation so we can
                 * write any bytes of the parameter value */
                fprintf(out, "\r\n\t");
                n = 3;
            }

            const char *q, *eol = p + MIME_MAX_HEADER_LENGTH - n - quote*2;
            if (quote) fputc('"', out);
            for (q = p; q < top && q < eol; q++) {
                fputc(*q, out);
            }
            if (quote) fputc('"', out);
            p = q;
            section++;
        }
        buf_free(&buf);
    }

    return 0;
}

static int writeattach(jmap_req_t *req, json_t *att, const char *boundary, FILE *out)
{
    struct mailbox *mbox = NULL;
    struct body *body = NULL;
    const struct body *part = NULL;
    struct buf msg_buf = BUF_INITIALIZER;
    const char *blobid, *type, *cid, *name;
    strarray_t headers = STRARRAY_INITIALIZER;
    msgrecord_t *mr;
    char *ctenc = NULL, *content_type = NULL;
    uint32_t size;
    int r, name_is_ascii = 1;

    type = json_string_value(json_object_get(att, "type"));
    blobid = json_string_value(json_object_get(att, "blobId"));
    cid = json_string_value(json_object_get(att, "cid"));
    name = json_string_value(json_object_get(att, "name"));

    if (name) {
        const unsigned char *p;
        for (p = (const unsigned char*) name; *p; p++) {
            if (!isascii(*p)) {
                name_is_ascii = 0;
                break;
            }
        }
    }

    /* Find part containing blob */
    r = jmap_findblob(req, blobid, &mbox, &mr, &body, &part);
    if (r) goto done;

    /* Map the message into memory */
    r = msgrecord_get_body(mr, &msg_buf);
    if (r) goto done;

    r = msgrecord_get_size(mr, &size);
    if (r) goto done;

    if (boundary) {
        fprintf(out, "\r\n--%s\r\n", boundary);
    }

    /* Write headers */

    /* Content-Type */
    if (type) {
        content_type = xstrdup(type);
    }
    else if (part) {
        content_type = strconcat(part->type, "/", part->subtype, NULL);
        lcase(content_type);
    }
    else {
        content_type = xstrdup("message/rfc822");
    }

    fputs("Content-Type: ", out);
    fputs(content_type, out);

    if (name) {
        /* RFC 2045 dropped the "name" parameter value for Content-Type,
         * but the quasi-standard is to QP-encode any attachment name in
         * this parameter value */
        char *qpname = charset_encode_mimeheader(name, strlen(name));
        writeparam(out, "name", qpname, 1, 0);
        free(qpname);
    }
    if (part && part->params) {
        /* Copy any additional Content-Type parameters from original body */
        struct param *param;
        for (param = part->params; param; param = param->next) {
            char *param_name = lcase(xstrdup(param->attribute));
            if (strcmp(param_name, "name")) {
                const char *p;
                int need_quote = 0;
                for (p = param->value; p && *p; p++) {
                    if (strchr(MIME_TSPECIALS, *p)) {
                        need_quote = 1;
                        break;
                    }
                }
                writeparam(out, param_name, param->value, need_quote, 0);
            }
            free(param_name);
        }
    }
    fputs("\r\n", out);
    free(content_type);

    /* Content-ID */
    if (cid) {
        JMAPMSG_HEADER_TO_MIME("Content-ID", cid);
    }

    /* Content-Disposition */
    fputs("Content-Disposition: attachment", out);
    if (name) {
        /* XXX break excessively long parameter values */
        if (!name_is_ascii) {
            char *s = charset_encode_mimexvalue(name, NULL);
            writeparam(out, "filename*", s, 0, 1);
            free(s);
        } else {
            writeparam(out, "filename", name, 1, 0);
        }
    }
    fputs("\r\n", out);

    /* Raw file */
    if (!part) {
        fputs("\r\n", out);
        fwrite(msg_buf.s, 1, size, out);
    }
    /* MESSAGE parts mustn't be re-encoded, and maybe no encoding is required... */
    else if (!strcasecmp(part->type, "MESSAGE") ||
             is_7bit_safe(msg_buf.s + part->content_offset, part->content_size, boundary)) {

        strarray_add(&headers, "Content-Transfer-Encoding");
        ctenc = xstrndup(msg_buf.s + part->header_offset, part->header_size);
        message_pruneheader(ctenc, &headers, NULL);
        fwrite(ctenc, 1, strlen(ctenc), out);
        strarray_truncate(&headers, 0);
        free(ctenc);
        ctenc = NULL;

        fputs("\r\n", out);

        fwrite(msg_buf.s + part->content_offset, 1, part->content_size, out);
    }
    else if (!strcasecmp(part->type, "TEXT")) {
        size_t qplen;
        char *freeme = charset_qpencode_mimebody(msg_buf.s + part->content_offset,
                                                 body->content_size, &qplen);

        JMAPMSG_HEADER_TO_MIME("Content-Transfer-Encoding", "quoted-printable");

        fputs("\r\n", out);

        fwrite(freeme, 1, qplen, out);

        free(freeme);
    }
    else {
        size_t b64_size;

        /* Determine encoded size */
        charset_encode_mimebody(NULL, part->content_size, NULL,
                                &b64_size, NULL);

        /* Realloc buffer to accomodate encoding overhead */
        char *freeme = xmalloc(b64_size);

        /* Encode content into buffer at current position */
        charset_encode_mimebody(msg_buf.s + part->content_offset,
                                body->content_size,
                                freeme, NULL, NULL);

        JMAPMSG_HEADER_TO_MIME("Content-Transfer-Encoding", "base64");

        fputs("\r\n", out);

        fwrite(freeme, 1, b64_size, out);

        free(freeme);
    }

    r = 0;


done:
    if (mr) msgrecord_unref(&mr);
    if (mbox) jmap_closembox(req, &mbox);
    if (body) {
        message_free_body(body);
        free(body);
    }
    free(ctenc);
    buf_free(&msg_buf);
    strarray_fini(&headers);
    return r;
}

static int jmapmsg_to_mimebody(jmap_req_t *req, json_t *msg,
                             const char *boundary, FILE *out)
{
    char *freeme = NULL, *myboundary = NULL;
    size_t i;
    struct buf buf = BUF_INITIALIZER;
    int r;
    json_t *attachments = NULL, *cid_attachments = NULL, *attached_msgs = NULL;
    json_t *val = NULL, *text = NULL, *html = NULL, *mymsg = NULL;

    /* Make. a shallow copy of msg as scratchpad. */
    mymsg = json_copy(msg);

    /* Determine text bodies */
    text = json_object_get(mymsg, "textBody");
    html = json_object_get(mymsg, "htmlBody");
    json_incref(text);
    json_incref(html);

    /* Determine attached messages */
    attached_msgs = json_object_get(mymsg, "attachedMessages");
    json_incref(attached_msgs);

    /* Split attachments into ones with and without cid. If there's no
     * htmlBody defined, all attachments end up in a multipart. */
    cid_attachments = json_pack("[]");
    attachments = json_pack("[]");
    json_array_foreach(json_object_get(mymsg, "attachments"), i, val) {
        if (html && JNOTNULL(json_object_get(val, "cid")) &&
                    json_object_get(val, "isInline") == json_true()) {
            json_array_append(cid_attachments, val);
        } else {
            json_array_append(attachments, val);
        }
    }
    if (!json_array_size(cid_attachments)) {
        json_decref(cid_attachments);
        cid_attachments = NULL;
    }
    if (!json_array_size(attachments)) {
        json_decref(attachments);
        attachments = NULL;
    }

    if (boundary) {
        fprintf(out, "\r\n--%s\r\n", boundary);
    }

    if (json_array_size(attachments) || json_object_size(attached_msgs)) {
        /* Content-Type is multipart/mixed */
        json_t *submsg;
        const char *subid;
        myboundary = _make_boundary();

        buf_setcstr(&buf, "multipart/mixed; boundary=");
        buf_appendcstr(&buf, myboundary);
        JMAPMSG_HEADER_TO_MIME("Content-Type", buf_cstring(&buf));

        /* Remove any non-cid attachments and attached messages. We'll
         * write them after the trimmed down message is serialised. */
        json_object_del(mymsg, "attachments");
        json_object_del(mymsg, "attachedMessages");

        /* If there's attachments with CIDs pass them on so the
         * htmlBody can be serialised together with is attachments. */
        if (cid_attachments) {
            json_object_set(mymsg, "attachments", cid_attachments);
        }

        /* Write any remaining message bodies before the attachments */
        r = jmapmsg_to_mimebody(req, mymsg, myboundary, out);
        if (r) goto done;

        /* Write attachments */
        json_array_foreach(attachments, i, val) {
            r = writeattach(req, val, myboundary, out);
            if (r) goto done;
        }

        /* Write embedded RFC822 messages */
        json_object_foreach(attached_msgs, subid, submsg) {
            fprintf(out, "\r\n--%s\r\n", myboundary);
            fputs("Content-Type: message/rfc822;charset=UTF-8\r\n\r\n", out);
            r = jmapmsg_to_mime(req, out, submsg);
            if (r) goto done;
        }

        fprintf(out, "\r\n--%s--\r\n", myboundary);
    }
    else if (JNOTNULL(text) && JNOTNULL(html)) {
        /* Content-Type is multipart/alternative */
        myboundary = _make_boundary();

        buf_setcstr(&buf, "multipart/alternative; boundary=");
        buf_appendcstr(&buf, myboundary);
        JMAPMSG_HEADER_TO_MIME("Content-Type", buf_cstring(&buf));

        /* Remove the html body and any attachments it refers to. We'll write
         * them after the plain text body has been serialised. */
        json_object_del(mymsg, "htmlBody");
        json_object_del(mymsg, "attachments");

        /* Write the plain text body */
        r = jmapmsg_to_mimebody(req, mymsg, myboundary, out);
        if (r) goto done;

        /* Write the html body, including any of its related attachments */
        json_object_del(mymsg, "textBody");
        json_object_set(mymsg, "htmlBody", html);
        if (json_array_size(cid_attachments)) {
            json_object_set(mymsg, "attachments", cid_attachments);
        }
        r = jmapmsg_to_mimebody(req, mymsg, myboundary, out);
        if (r) goto done;

        fprintf(out, "\r\n--%s--\r\n", myboundary);
    }
    else if (html && json_array_size(cid_attachments)) {
        /* Content-Type is multipart/related */
        myboundary = _make_boundary();

        buf_setcstr(&buf, "multipart/related; type=\"text/html\"; boundary=");
        buf_appendcstr(&buf, myboundary);
        JMAPMSG_HEADER_TO_MIME("Content-Type", buf_cstring(&buf));

        /* Remove the attachments to serialise the html body */
        json_object_del(mymsg, "attachments");
        r = jmapmsg_to_mimebody(req, mymsg, myboundary, out);
        if (r) goto done;

        /* Write attachments */
        json_array_foreach(cid_attachments, i, val) {
            r = writeattach(req, val, myboundary, out);
            if (r) goto done;
        }

        fprintf(out, "\r\n--%s--\r\n", myboundary);

    }
    else if (JNOTNULL(html)) {
        /* Content-Type is text/html */
        JMAPMSG_HEADER_TO_MIME("Content-Type", "text/html;charset=UTF-8");
        fputs("\r\n", out);
        writetext(json_string_value(html), out, split_html);
    }
    else if (JNOTNULL(text)) {
        /* Content-Type is text/plain */
        JMAPMSG_HEADER_TO_MIME("Content-Type", "text/plain;charset=UTF-8");
        fputs("\r\n", out);
        writetext(json_string_value(text), out, split_plain);
    }

    /* All done */
    r = 0;

done:
    if (myboundary) free(myboundary);
    if (freeme) free(freeme);
    json_decref(attachments);
    json_decref(cid_attachments);
    json_decref(attached_msgs);
    json_decref(text);
    json_decref(html);
    json_decref(mymsg);
    buf_free(&buf);
    if (r) r = HTTP_SERVER_ERROR;
    return r;
}

/* Write the JMAP Message msg in RFC-5322 compliant wire format.
 *
 * The message is assumed to not contain value errors. If 'date' is neither
 * set in the message headers nor property, the current date is set. If
 * From isn't set, the userid of the current jmap request is used as
 * email address.
 *
 * Return 0 on success or non-zero if writing to the file failed */
static int jmapmsg_to_mime(jmap_req_t *req, FILE *out, json_t *msg)
{
    struct jmapmsgdata {
        char *subject;
        char *to;
        char *cc;
        char *bcc;
        char *replyto;
        char *sender;
        char *from;
        char *date;
        char *messageid;
        char *mua;

        char *references;
        char *inreplyto;
        char *replyto_id;
        json_t *headers;
    } d;

    json_t *val, *prop, *mymsg;
    const char *key, *s;
    size_t i;
    struct buf buf = BUF_INITIALIZER;
    int r = 0;
    memset(&d, 0, sizeof(struct jmapmsgdata));

    /* Weed out special header values. */
    d.headers = json_pack("{}");
    json_object_foreach(json_object_get(msg, "headers"), key, val) {
        s = json_string_value(val);
        if (!s) {
            continue;
        } else if (!strcasecmp(key, "Bcc")) {
            d.bcc = xstrdup(s);
        } else if (!strcasecmp(key, "Cc")) {
            d.cc = xstrdup(s);
        } else if (!strcasecmp(key, "Content-Transfer-Encoding")) {
            /* Ignore */
        } else if (!strcasecmp(key, "Content-Type")) {
            /* Ignore */
        } else if (!strcasecmp(key, "Date")) {
            d.date = xstrdup(s);
        } else if (!strcasecmp(key, "From")) {
            d.from = xstrdup(s);
        } else if (!strcasecmp(key, "In-Reply-To")) {
            d.inreplyto = xstrdup(s);
        } else if (!strcasecmp(key, "Message-ID")) {
            /* Ignore */
        } else if (!strcasecmp(key, "MIME-Version")) {
            /* Ignore */
        } else if (!strcasecmp(key, "References")) {
            d.references = xstrdup(s);
        } else if (!strcasecmp(key, "Reply-To")) {
            d.replyto = xstrdup(s);
        } else if (!strcasecmp(key, "Sender")) {
            d.sender = xstrdup(s);
        } else if (!strcasecmp(key, "Subject")) {
            d.subject = xstrdup(s);
        } else if (!strcasecmp(key, "To")) {
            d.to = xstrdup(s);
        } else if (!strcasecmp(key, "User-Agent")) {
            d.mua = xstrdup(s);
        } else {
            json_object_set(d.headers, key, val);
        }
    }

    /* Override the From header */
    if ((prop = json_object_get(msg, "from"))) {
        json_array_foreach(prop, i, val) {
            if (i) buf_appendcstr(&buf, ", ");
            JMAPMSG_EMAILER_TO_MIME(&buf, val);
        }
        if (d.from) free(d.from);
        d.from = buf_newcstring(&buf);
        buf_reset(&buf);
    }
    if (!d.from) d.from = xstrdup(req->userid);

    /* Override the Sender header */
    if ((prop = json_object_get(msg, "sender"))) {
        JMAPMSG_EMAILER_TO_MIME(&buf, prop);
        if (d.sender) free(d.sender);
        d.sender = buf_newcstring(&buf);
        buf_reset(&buf);
    }

    /* Override the To header */
    if ((prop = json_object_get(msg, "to"))) {
        json_array_foreach(prop, i, val) {
            if (i) buf_appendcstr(&buf, ", ");
            JMAPMSG_EMAILER_TO_MIME(&buf, val);
        }
        if (d.to) free(d.to);
        d.to = buf_newcstring(&buf);
        buf_reset(&buf);
    }

    /* Override the Cc header */
    if ((prop = json_object_get(msg, "cc"))) {
        json_array_foreach(prop, i, val) {
            if (i) buf_appendcstr(&buf, ", ");
            JMAPMSG_EMAILER_TO_MIME(&buf, val);
        }
        if (d.cc) free(d.cc);
        d.cc = buf_newcstring(&buf);
        buf_reset(&buf);
    }

    /* Override the Bcc header */
    if ((prop = json_object_get(msg, "bcc"))) {
        json_array_foreach(prop, i, val) {
            if (i) buf_appendcstr(&buf, ", ");
            JMAPMSG_EMAILER_TO_MIME(&buf, val);
        }
        if (d.bcc) free(d.bcc);
        d.bcc = buf_newcstring(&buf);
        buf_reset(&buf);
    }

    /* Override the Reply-To header */
    if ((prop = json_object_get(msg, "replyTo"))) {
        json_array_foreach(prop, i, val) {
            if (i) buf_appendcstr(&buf, ", ");
            JMAPMSG_EMAILER_TO_MIME(&buf, val);
        }
        if (d.replyto) free(d.replyto);
        d.replyto = buf_newcstring(&buf);
        buf_reset(&buf);
    }

    /* Override the In-Reply-To and References headers */
    if ((prop = json_object_get(msg, "inReplyToMessageId"))) {
        if ((s = json_string_value(prop))) {
            d.replyto_id = xstrdup(s);

            if (d.references) free(d.references);
            if (d.inreplyto) free(d.inreplyto);

            r = jmapmsg_get_messageid(req, d.replyto_id, &d.references);
            if (!r) d.inreplyto = xstrdup(d.references);
        }
    }

    /* Override Subject header */
    if ((s = json_string_value(json_object_get(msg, "subject")))) {
        if (d.subject) free(d.subject);
        d.subject = xstrdup(s);
    }
    if (!d.subject) d.subject = xstrdup("");

    /* Override Date header */
    /* Precedence (highest first): "date" property, Date header, now */
    time_t date = time(NULL);
    if ((s = json_string_value(json_object_get(msg, "date")))) {
        struct tm tm;
        strptime(s, "%Y-%m-%dT%H:%M:%SZ", &tm);
        date = mktime(&tm);
    }
    if (json_object_get(msg, "date") || !d.date) {
        char fmt[RFC5322_DATETIME_MAX+1];
        memset(fmt, 0, RFC5322_DATETIME_MAX+1);
        time_to_rfc5322(date, fmt, RFC5322_DATETIME_MAX+1);
        if (d.date) free(d.date);
        d.date = xstrdup(fmt);
    }

    /* Set Message-ID header */
    buf_printf(&buf, "<%s@%s>", makeuuid(), config_servername);
    d.messageid = buf_release(&buf);

    /* Set User-Agent header */
    if (!d.mua) {
        /* Cyrus server-info is great but way to expressive. Cut of
         * anything after after the main server info */
        char *p;
        d.mua = buf_newcstring(&serverinfo);
        for (p = d.mua; *p; p++) {
            if (isspace(*p)) { *p = '\0'; break; }
        }
    }

    /* Build raw message */
    fputs("MIME-Version: 1.0\r\n", out);

    /* Mandatory headers according to RFC 5322 */
    JMAPMSG_HEADER_TO_MIME("From", d.from);
    JMAPMSG_HEADER_TO_MIME("Date", d.date);

    /* Common headers */
    if (d.to)      JMAPMSG_HEADER_TO_MIME("To", d.to);
    if (d.cc)      JMAPMSG_HEADER_TO_MIME("Cc", d.cc);
    if (d.bcc)     JMAPMSG_HEADER_TO_MIME("Bcc", d.bcc);
    if (d.sender)  JMAPMSG_HEADER_TO_MIME("Sender", d.sender);
    if (d.replyto) JMAPMSG_HEADER_TO_MIME("Reply-To", d.replyto);
    if (d.subject) JMAPMSG_HEADER_TO_MIME("Subject", d.subject);

    /* References, In-Reply-To and the custom X-JMAP header */
    if (d.inreplyto)  JMAPMSG_HEADER_TO_MIME("In-Reply-To", d.inreplyto);
    if (d.references) JMAPMSG_HEADER_TO_MIME("References", d.references);
    if (d.replyto_id) JMAPMSG_HEADER_TO_MIME(JMAP_INREPLYTO_HEADER, d.replyto_id);

    /* Custom headers */
    json_object_foreach(d.headers, key, val) {
        char *freeme, *p, *q;
        s = json_string_value(val);
        if (!s) continue;
        freeme = xstrdup(s);
        for (q = freeme, p = freeme; *p; p++) {
            if (*p == '\n' && (p == q || *(p-1) != '\r')) {
                *p = '\0';
                JMAPMSG_HEADER_TO_MIME(key, q);
                *p = '\n';
                q = p + 1;
            }
        }
        JMAPMSG_HEADER_TO_MIME(key, q);
        free(freeme);
    }

    /* Not mandatory but we'll always write these */
    JMAPMSG_HEADER_TO_MIME("Message-ID", d.messageid);
    JMAPMSG_HEADER_TO_MIME("User-Agent", d.mua);

    /* Make a shallow copy to alter */
    mymsg = json_copy(msg);

    /* Convert html body to plain text, if required */
    if (!json_object_get(mymsg, "textBody")) {
        const char *html = json_string_value(json_object_get(mymsg, "htmlBody"));
        if (html) {
            char *tmp = extract_plain(html);
            json_object_set_new(mymsg, "textBody", json_string(tmp));
            free(tmp);
        }
    }

    /* Write message body */
    r = jmapmsg_to_mimebody(req, mymsg, NULL, out);
    json_decref(mymsg);

    if (d.from) free(d.from);
    if (d.sender) free(d.sender);
    if (d.date) free(d.date);
    if (d.to) free(d.to);
    if (d.cc) free(d.cc);
    if (d.bcc) free(d.bcc);
    if (d.replyto) free(d.replyto);
    if (d.subject) free(d.subject);
    if (d.messageid) free(d.messageid);
    if (d.references) free(d.references);
    if (d.inreplyto) free(d.inreplyto);
    if (d.replyto_id) free(d.replyto_id);
    if (d.mua) free(d.mua);
    if (d.headers) json_decref(d.headers);
    buf_free(&buf);
    if (r) r = HTTP_SERVER_ERROR;
    return r;
}

#undef JMAPMSG_EMAILER_TO_MIME
#undef JMAPMSG_HEADER_TO_MIME

static void jmapmsg_validate(json_t *msg, json_t *invalid, int isdraft)
{
    int pe;
    json_t *prop;
    const char *sval;
    int bval, intval;
    struct buf buf = BUF_INITIALIZER;
    struct tm *date = xzmalloc(sizeof(struct tm));
    char *mboxname = NULL;
    char *mboxrole = NULL;
    int validateaddr = !isdraft;

    pe = readprop(msg, "isDraft", 0, invalid, "b", &bval);
    if (pe > 0 && !bval) {
        json_array_append_new(invalid, json_string("isDraft"));
    }

    if (json_object_get(msg, "id")) {
        json_array_append_new(invalid, json_string("id"));
    }

    if (json_object_get(msg, "blobId")) {
        json_array_append_new(invalid, json_string("blobId"));
    }

    if (json_object_get(msg, "threadId")) {
        json_array_append_new(invalid, json_string("threadId"));
    }

    prop = json_object_get(msg, "inReplyToMessageId");
    if (JNOTNULL(prop)) {
        if (!((sval = json_string_value(prop)) && strlen(sval))) {
            json_array_append_new(invalid, json_string("inReplyToMessageId"));
        }
    }

    pe = readprop(msg, "isUnread", 0, invalid, "b", &bval);
    if (pe > 0 && bval) {
        json_array_append_new(invalid, json_string("isUnread"));
    }

    readprop(msg, "isFlagged", 0, invalid, "b", &bval);

    pe = readprop(msg, "isAnswered", 0, invalid, "b", &bval);
    if (pe > 0 && bval) {
        json_array_append_new(invalid, json_string("isAnswered"));
    }

    if (json_object_get(msg, "hasAttachment")) {
        json_array_append_new(invalid, json_string("hasAttachment"));
    }

    prop = json_object_get(msg, "headers");
    if (json_object_size(prop)) {
        const char *key;
        json_t *val;
        json_object_foreach(prop, key, val) {
            int valid = strlen(key) && val && json_typeof(val) == JSON_STRING;
            /* Keys MUST only contain A-Z,* a-z, 0-9 and hyphens. */
            const char *c;
            for (c = key; *c && valid; c++) {
                if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
                      (*c >= '0' && *c <= '9') || (*c == '-'))) {
                    valid = 0;
                }
            }
            /* Validate mail addresses in overridden header */
            int ismailheader = (!strcasecmp(key, "From") ||
                                !strcasecmp(key, "Reply-To") ||
                                !strcasecmp(key, "Cc") ||
                                !strcasecmp(key, "Bcc") ||
                                !strcasecmp(key, "To"));
            if (valid && ismailheader && validateaddr) {
                struct address *ap, *addr = NULL;
                parseaddr_list(json_string_value(val), &addr);
                if (!addr) valid = 0;
                for (ap = addr; valid && ap; ap = ap->next) {
                    if (ap->invalid || !ap->mailbox || !ap->domain) {
                        valid = 0;
                    }
                }
                parseaddr_free(addr);
            }
            if (!valid) {
                buf_printf(&buf, "header[%s]", key);
                json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                buf_reset(&buf);
                break;
            }
        }
    } else if (prop && json_typeof(prop) != JSON_OBJECT) {
        json_array_append_new(invalid, json_string("headers"));
    }

    prop = json_object_get(msg, "from");
    if (json_array_size(prop)) {
        json_t *emailer;
        size_t i;
        json_array_foreach(prop, i, emailer) {
            buf_printf(&buf, "from[%zu]", i);
            jmap_validate_emailer(emailer, buf_cstring(&buf), validateaddr, invalid);
            buf_reset(&buf);
        }
    } else if (JNOTNULL(prop) && json_typeof(prop) != JSON_ARRAY) {
        json_array_append_new(invalid, json_string("from"));
    }

    prop = json_object_get(msg, "to");
    if (json_array_size(prop)) {
        json_t *emailer;
        size_t i;
        json_array_foreach(prop, i, emailer) {
            buf_printf(&buf, "to[%zu]", i);
            jmap_validate_emailer(emailer, buf_cstring(&buf), validateaddr, invalid);
            buf_reset(&buf);
        }
    } else if (JNOTNULL(prop) && json_typeof(prop) != JSON_ARRAY) {
        json_array_append_new(invalid, json_string("to"));
    }

    prop = json_object_get(msg, "cc");
    if (json_array_size(prop)) {
        json_t *emailer;
        size_t i;
        json_array_foreach(prop, i, emailer) {
            buf_printf(&buf, "cc[%zu]", i);
            jmap_validate_emailer(emailer, buf_cstring(&buf), validateaddr, invalid);
            buf_reset(&buf);
        }
    } else if (JNOTNULL(prop) && json_typeof(prop) != JSON_ARRAY) {
        json_array_append_new(invalid, json_string("cc"));
    }

    prop = json_object_get(msg, "bcc");
    if (json_array_size(prop)) {
        json_t *emailer;
        size_t i;
        json_array_foreach(prop, i, emailer) {
            buf_printf(&buf, "bcc[%zu]", i);
            jmap_validate_emailer(emailer, buf_cstring(&buf), validateaddr, invalid);
            buf_reset(&buf);
        }
    } else if (JNOTNULL(prop) && json_typeof(prop) != JSON_ARRAY) {
        json_array_append_new(invalid, json_string("bcc"));
    }

    prop = json_object_get(msg, "sender");
    if (JNOTNULL(prop)) {
        jmap_validate_emailer(prop, "sender", validateaddr, invalid);
    }

    prop = json_object_get(msg, "replyTo");
    if (json_array_size(prop)) {
        json_t *emailer;
        size_t i;
        json_array_foreach(prop, i, emailer) {
            buf_printf(&buf, "replyTo[%zu]", i);
            jmap_validate_emailer(emailer, buf_cstring(&buf), validateaddr, invalid);
            buf_reset(&buf);
        }
    } else if (JNOTNULL(prop) && json_typeof(prop) != JSON_ARRAY) {
        json_array_append_new(invalid, json_string("replyTo"));
    }

    pe = readprop(msg, "date", 0, invalid, "s", &sval);
    if (pe > 0) {
        const char *p = strptime(sval, "%Y-%m-%dT%H:%M:%SZ", date);
        if (!p || *p) {
            json_array_append_new(invalid, json_string("date"));
        }
    }

    if (json_object_get(msg, "size")) {
        json_array_append_new(invalid, json_string("size"));
    }

    if (json_object_get(msg, "preview")) {
        json_array_append_new(invalid, json_string("preview"));
    }

    readprop(msg, "subject", 0, invalid, "s", &sval);
    readprop(msg, "textBody", 0, invalid, "s", &sval);
    readprop(msg, "htmlBody", 0, invalid, "s", &sval);

    prop = json_object_get(msg, "attachedMessages");
    if (json_object_size(prop)) {
        json_t *submsg;
        const char *subid;
        json_object_foreach(prop, subid, submsg) {
            json_t *subinvalid = json_pack("[]");
            json_t *errprop;
            size_t j;

            jmapmsg_validate(submsg, subinvalid, 0);

            buf_printf(&buf, "attachedMessages[%s]", subid);
            json_array_foreach(subinvalid, j, errprop) {
                const char *s = json_string_value(errprop);
                buf_appendcstr(&buf, ".");
                buf_appendcstr(&buf, s);
                json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                buf_truncate(&buf, buf_len(&buf) - strlen(s) - 1);
            }
            json_decref(subinvalid);
            buf_reset(&buf);
        }
    }
    else if (JNOTNULL(prop)) {
        json_array_append_new(invalid, json_string("attachedMessages"));
    }

    prop = json_object_get(msg, "attachments");
    if (json_array_size(prop)) {
        json_t *att;
        size_t i;

        json_array_foreach(prop, i, att) {
            const char *prefix;
            buf_printf(&buf, "attachments[%zu]", i);
            prefix = buf_cstring(&buf);

            readprop_full(att, prefix, "blobId", 1, invalid, "s", &sval);
            readprop_full(att, prefix, "type", 0, invalid, "s", &sval);
            readprop_full(att, prefix, "name", 0, invalid, "s", &sval);

            if (readprop_full(att, prefix, "cid", 0, invalid, "s", &sval) > 0) {
                struct address *addr = NULL;
                parseaddr_list(sval, &addr);
                if (!addr || addr->next || addr->name) {
                    char *freeme = strconcat(prefix, ".", "cid", NULL);
                    json_array_append_new(invalid, json_string(freeme));
                    free(freeme);
                }
                parseaddr_free(addr);
            }

            readprop_full(att, prefix, "isInline", 0, invalid, "b", &bval);
            readprop_full(att, prefix, "width", 0, invalid, "i", &intval);
            readprop_full(att, prefix, "height", 0, invalid, "i", &intval);

            buf_reset(&buf);
        }

    }
    else if (JNOTNULL(prop)) {
        json_array_append_new(invalid, json_string("attachments"));
    }

    buf_free(&buf);
    if (mboxname) free(mboxname);
    if (mboxrole) free(mboxrole);
    if (date) free(date);
}

static int copyrecord(jmap_req_t *req, struct mailbox *src, struct mailbox *dst,
                      msgrecord_t *mrw)
{
    struct appendstate as;
    int r;
    int nolink = !config_getswitch(IMAPOPT_SINGLEINSTANCESTORE);
    ptrarray_t msgrecs = PTRARRAY_INITIALIZER;

    if (!strcmp(src->uniqueid, dst->uniqueid))
        return 0;

    r = append_setup_mbox(&as, dst, req->userid, httpd_authstate,
            ACL_INSERT, NULL, &jmap_namespace, 0, EVENT_MESSAGE_COPY);
    if (r) goto done;

    ptrarray_add(&msgrecs, mrw);

    r = append_copy(src, &as, &msgrecs, nolink,
            mboxname_same_userid(src->name, dst->name));
    if (r) {
        append_abort(&as);
        goto done;
    }

    r = append_commit(&as);
    if (r) goto done;

    sync_log_mailbox_double(src->name, dst->name);
done:
    ptrarray_fini(&msgrecs);
    return r;
}

static int updaterecord(msgrecord_t *mrw, int flagged, int unread, int answered)
{
    uint32_t flags = 0;
    int r;

    r = msgrecord_get_systemflags(mrw, &flags);
    if (r) return r;

    if (flagged > 0)
        flags |= FLAG_FLAGGED;
    else if (!flagged)
        flags &= ~FLAG_FLAGGED;

    if (unread > 0)
        flags &= ~FLAG_SEEN;
    else if (!unread)
        flags |= FLAG_SEEN;

    if (answered > 0)
        flags |= FLAG_ANSWERED;
    else if (!answered)
        flags &= ~FLAG_ANSWERED;

    return msgrecord_set_systemflags(mrw, flags);
}

struct updaterecord_data {
    jmap_req_t *req;
    json_t *mailboxes;
    int flagged;
    int unread;
    int answered;
};

static int updaterecord_cb(const conv_guidrec_t *rec, void *rock)
{
    struct updaterecord_data *d = (struct updaterecord_data *) rock;
    jmap_req_t *req = d->req;
    struct mailbox *mbox = NULL;
    msgrecord_t *mrw = NULL;
    int r = 0;

    if (rec->part) return 0;

    r = jmap_openmbox(req, rec->mboxname, &mbox, 1);
    if (r) goto done;

    if (!d->mailboxes || json_object_get(d->mailboxes, mbox->uniqueid)) {

        r = msgrecord_find(mbox, rec->uid, &mrw);
        if (r) goto done;

        r = updaterecord(mrw, d->flagged, d->unread, d->answered);
        if (r) goto done;

        r = msgrecord_rewrite(mrw);
        if (r) goto done;

        msgrecord_unref(&mrw);
    }

done:
    if (mrw) msgrecord_unref(&mrw);
    if (mbox) jmap_closembox(req, &mbox);
    return r;
}

static int delrecord(jmap_req_t *req, struct mailbox *mbox, uint32_t uid)
{
    int r;
    struct mboxevent *mboxevent = NULL;
    msgrecord_t *mrw = NULL;
    uint32_t flags;

    r = msgrecord_find(mbox, uid, &mrw);
    if (r) return r;

    r = msgrecord_get_systemflags(mrw, &flags);
    if (r) goto done;

    if (flags & FLAG_EXPUNGED) {
        r = 0;
        goto done;
    }

    /* Expunge index record */
    r = msgrecord_add_systemflags(mrw, FLAG_DELETED | FLAG_EXPUNGED);
    if (r) goto done;

    r = msgrecord_rewrite(mrw);
    if (r) goto done;

    /* Report mailbox event. */
    mboxevent = mboxevent_new(EVENT_MESSAGE_EXPUNGE);
    mboxevent_extract_msgrecord(mboxevent, mrw);
    mboxevent_extract_mailbox(mboxevent, mbox);
    mboxevent_set_numunseen(mboxevent, mbox, -1);
    mboxevent_set_access(mboxevent, NULL, NULL, req->userid, mbox->name, 0);
    mboxevent_notify(&mboxevent);
    mboxevent_free(&mboxevent);

done:
    if (mrw) msgrecord_unref(&mrw);
    return r;
}

struct delrecord_data {
    jmap_req_t *req;
    int deleted;
    json_t *mailboxes;
};

static int delrecord_cb(const conv_guidrec_t *rec, void *rock)
{
    struct delrecord_data *d = (struct delrecord_data *) rock;
    jmap_req_t *req = d->req;
    struct mailbox *mbox = NULL;
    int r = 0;

    r = jmap_openmbox(req, rec->mboxname, &mbox, 1);
    if (r) goto done;

    if (!d->mailboxes || json_object_get(d->mailboxes, mbox->uniqueid)) {
        r = delrecord(d->req, mbox, rec->uid);
        if (!r) d->deleted++;
    }

done:
    if (mbox) jmap_closembox(req, &mbox);
    return r;
}

static int jmapmsg_write(jmap_req_t *req, json_t *mailboxids, int system_flags, time_t internaldate,
                         int(*writecb)(jmap_req_t*, FILE*, void*), void *rock,
                         char **msgid)
{
    int fd;
    void *addr;
    FILE *f = NULL;
    char *mboxname = NULL;
    const char *id;
    struct stagemsg *stage = NULL;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    quota_t qdiffs[QUOTA_NUMRESOURCES] = QUOTA_DIFFS_DONTCARE_INITIALIZER;
    json_t *val, *mailboxes = NULL;
    size_t len, i, msgcount = 0;
    int r = HTTP_SERVER_ERROR;

    if (!internaldate) internaldate = time(NULL);

    /* Pick the mailbox to create the message in, prefer Drafts */
    mailboxes = json_pack("{}"); /* maps mailbox ids to mboxnames */
    json_array_foreach(mailboxids, i, val) {
        char *name = NULL;

        id = json_string_value(val);
        if (id && *id == '#') {
            id = hash_lookup(id + 1, &req->idmap->mailboxes);
        }
        if (!id) continue;

        name = jmapmbox_find_uniqueid(req, id);
        if (!name) continue;

        mbname_t *mbname = mbname_from_intname(name);
        char *role = jmapmbox_role(req, mbname);
        mbname_free(&mbname);
        if (role) {
            if (!strcmp(role, "drafts")) {
                if (mboxname) {
                    free(mboxname);
                }
                mboxname = xstrdup(name);
            }
            if (!strcmp(role, "outbox") && !mboxname) {
                if (mboxname) {
                    free(mboxname);
                }
                mboxname = xstrdup(name);
            }
        }

        if (!mboxname) {
            mboxname = xstrdup(name);
        }
        json_object_set_new(mailboxes, id, json_string(name));
        if (name) free(name);
        if (role) free(role);
    }

    /* Create the message in the destination mailbox */
    r = jmap_openmbox(req, mboxname, &mbox, 1);
    if (r) goto done;

    /* Write the message to the filesystem */
    if (!(f = append_newstage(mbox->name, internaldate, 0, &stage))) {
        syslog(LOG_ERR, "append_newstage(%s) failed", mbox->name);
        r = HTTP_SERVER_ERROR;
        goto done;
    }
    r = writecb(req, f, rock);
    if (r) goto done;
    if (fflush(f)) {
        r = IMAP_IOERROR;
        goto done;
    }
    fseek(f, 0L, SEEK_END);
    len = ftell(f);

    /* Generate a GUID from the raw file content */
    fd = fileno(f);
    if ((addr = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0))) {
        struct message_guid guid;
        message_guid_generate(&guid, addr, len);
        *msgid = jmap_msgid(&guid);
        munmap(addr, len);
    } else {
        r = IMAP_IOERROR;
        goto done;
    }
    fclose(f);
    f = NULL;

    /*  Check if a message with this GUID already exists */
    r = jmapmsg_count(req, *msgid, &msgcount);
    if (r && r != IMAP_NOTFOUND) {
        goto done;
    }

    if (msgcount == 0) {
        /* Great, that's a new message! */
        struct body *body = NULL;
        struct appendstate as;

        /* Append the message to the mailbox */
        qdiffs[QUOTA_MESSAGE] = 1;
        r = append_setup_mbox(&as, mbox, req->userid, httpd_authstate,
                0, qdiffs, 0, 0, EVENT_MESSAGE_NEW);
        if (r) goto done;
        r = append_fromstage(&as, &body, stage, internaldate, NULL, 0, NULL);
        if (r) {
            append_abort(&as);
            goto done;
        }
        message_free_body(body);
        free(body);

        r = append_commit(&as);
        if (r) goto done;

        /* Update system flags for new record */
        r = msgrecord_find(mbox, mbox->i.last_uid, &mr);
        if (r) goto done;

        r = msgrecord_add_systemflags(mr, system_flags);
        if (r) goto done;

        r = msgrecord_rewrite(mr);
        if (r) goto done;

        /* Flag mailbox */
        r = mailbox_user_flag(mbox, JMAP_HAS_ATTACHMENT_FLAG, NULL, 1);
        if (r) goto done;

        /* Complete message creation */
        if (stage) {
            append_removestage(stage);
            stage = NULL;
        }
        json_object_del(mailboxes, mbox->uniqueid);
    } else {
        /* A message with this GUID already exists! */
        uint32_t uid;
        json_t *oldmailboxes;

        append_removestage(stage);
        stage = NULL;

        jmap_closembox(req, &mbox);

        /* Don't overwrite the existing messages */
        oldmailboxes = jmapmsg_mailboxes(req, *msgid);
        json_object_foreach(oldmailboxes, id, val) {
            json_object_del(mailboxes, id);
        }
        json_decref(oldmailboxes);

        if (!json_object_size(mailboxes)) {
            r = IMAP_MAILBOX_EXISTS;
            goto done;
        }

        if (mboxname) free(mboxname);
        r = jmapmsg_find(req, *msgid, &mboxname, &uid);
        if (r) goto done;

        /* Open the mailbox where the message is stored */
        r = jmap_openmbox(req, mboxname, &mbox, 1);
        if (r) goto done;

        r = msgrecord_find(mbox, uid, &mr);
        if (r) goto done;

        /* Set flags in the new instances on this message,
         * but keep the existing entries as-is */
        r = msgrecord_add_systemflags(mr, system_flags);
        if (r) goto done;
    }

    /* Make sure there is enough quota for all mailboxes */
    qdiffs[QUOTA_STORAGE] = len;
    if (json_object_size(mailboxes)) {
        char foundroot[MAX_MAILBOX_BUFFER];
        json_t *deltas = json_pack("{}");
        const char *mbname;

        /* Count message delta for each quota root */
        json_object_foreach(mailboxes, id, val) {
            mbname = json_string_value(val);
            if (quota_findroot(foundroot, sizeof(foundroot), mbname)) {
                json_t *delta = json_object_get(deltas, mbname);
                delta = json_integer(json_integer_value(delta) + 1);
                json_object_set_new(deltas, mbname, delta);
            }
        }

        /* Check quota for each quota root. */
        json_object_foreach(deltas, mbname, val) {
            struct quota quota;
            quota_t delta = json_integer_value(val);

            quota_init(&quota, mbname);
            r = quota_check(&quota, QUOTA_STORAGE, delta * qdiffs[QUOTA_STORAGE]);
            if (!r) r = quota_check(&quota, QUOTA_MESSAGE, delta);
            quota_free(&quota);
            if (r) break;
        }
        json_decref(deltas);
        if (r) goto done;
    }

    /* Copy the message to all remaining mailboxes */
    json_object_foreach(mailboxes, id, val) {
        const char *dstname = json_string_value(val);
        struct mailbox *dst = NULL;

        if (!strcmp(mboxname, dstname))
            continue;

        r = jmap_openmbox(req, dstname, &dst, 1);
        if (r) goto done;

        r = copyrecord(req, mbox, dst, mr);

        jmap_closembox(req, &dst);
        if (r) goto done;
    }

done:
    if (f) fclose(f);
    if (stage) append_removestage(stage);
    if (mr) msgrecord_unref(&mr);
    if (mbox) jmap_closembox(req, &mbox);
    if (mboxname) free(mboxname);
    if (mailboxes) json_decref(mailboxes);
    return r;
}

static int jmapmsg_create(jmap_req_t *req, json_t *msg, char **msgid,
                          json_t *invalid)
{
    const char *id = NULL;
    json_t *val;
    time_t internaldate = 0;
    int have_mbox = 0;
    int system_flags = 0;
    size_t i;

    /* FIXME - this is broken for shared accounts, since we lookup
     * annotation per authenticated user. Needless to fix this,
     * since the JMAP spec has been updated to allow to create messages
     * in any mailboxes. */

    json_array_foreach(json_object_get(msg, "mailboxIds"), i, val) {
        id = json_string_value(val);
        if (id && *id == '#') {
            id = hash_lookup(id + 1, &req->idmap->mailboxes);
        }
        if (!id) {
            struct buf buf = BUF_INITIALIZER;
            buf_printf(&buf, "mailboxIds[%zu]", i);
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            buf_free(&buf);
            continue;
        }

        char *name = jmapmbox_find_uniqueid(req, id);
        if (!name) {
            struct buf buf = BUF_INITIALIZER;
            buf_printf(&buf, "mailboxIds[%zu]", i);
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            buf_free(&buf);
            continue;
        }
        mbname_t *mbname = mbname_from_intname(name);
        char *role = jmapmbox_role(req, mbname);
        mbname_free(&mbname);
        if (role) {
            if (!strcmp(role, "drafts") && !have_mbox) {
                have_mbox = 1;
                system_flags |= FLAG_DRAFT;
            }
            else if (!strcmp(role, "outbox")) {
                have_mbox = 1;
                system_flags &= ~FLAG_DRAFT;
            }
        }
        free(name);
        free(role);
    }
    if (!have_mbox && !json_array_size(invalid)) {
        json_array_append_new(invalid, json_string("mailboxIds"));
    }
    jmapmsg_validate(msg, invalid, system_flags & FLAG_DRAFT);

    if (json_array_size(invalid)) {
        return 0;
    }

    /* check for internaldate */
    const char *datestr = json_string_value(json_object_get(msg, "date"));
    if (datestr) {
        time_from_iso8601(datestr, &internaldate);
    }

    if (json_object_get(msg, "isFlagged") == json_true())
        system_flags |= FLAG_FLAGGED;

    return jmapmsg_write(req, json_object_get(msg, "mailboxIds"), system_flags, internaldate,
                         (int(*)(jmap_req_t*,FILE*,void*)) jmapmsg_to_mime,
                         msg, msgid);
}

struct msgupdate_checkacl_rock {
    jmap_req_t *req;
    json_t *newmailboxes;
    json_t *delmailboxes;
    json_t *oldmailboxes;
};

static int msgupdate_checkacl_cb(const mbentry_t *mbentry, void *xrock)
{
    struct msgupdate_checkacl_rock *rock = xrock;
    int need_rights = 0, got_rights;
    const char *id = mbentry->uniqueid;

    if (json_object_get(rock->newmailboxes, id))
        need_rights |= ACL_INSERT|ACL_ANNOTATEMSG;
    else if (json_object_get(rock->delmailboxes, id))
        need_rights |= ACL_DELETEMSG;
    else if (json_object_get(rock->oldmailboxes, id))
        need_rights |= ACL_ANNOTATEMSG;

    if (!need_rights)
        return 0;

    got_rights = jmap_myrights(rock->req, mbentry);
    if (!(need_rights & got_rights))
        return IMAP_PERMISSION_DENIED;

    return 0;
}

static int jmapmsg_update(jmap_req_t *req, const char *msgid, json_t *msg,
                          json_t *invalid)
{
    uint32_t uid;
    struct mailbox *mbox = NULL;
    char *mboxname = NULL;
    msgrecord_t *mrw = NULL;
    const char *id;
    int unread = -1, flagged = -1, answered = -1;
    int r;
    size_t i;
    json_t *val;
    json_t *dstmailboxes = NULL; /* destination mailboxes */
    json_t *srcmailboxes = NULL; /* current mailboxes */
    json_t *oldmailboxes = NULL; /* current mailboxes that are kept */
    json_t *newmailboxes = NULL; /* mailboxes to add the message to */
    json_t *delmailboxes = NULL; /* mailboxes to delete the message from */

    if (!strlen(msgid) || *msgid == '#') {
        return IMAP_NOTFOUND;
    }

    /* Pick record from any current mailbox. That's the master copy. */
    r = jmapmsg_find(req, msgid, &mboxname, &uid);
    if (r) return r;
    srcmailboxes = jmapmsg_mailboxes(req, msgid);

    /* Validate properties */
    if (json_object_get(msg, "isFlagged")) {
        readprop(msg, "isFlagged", 1, invalid, "b", &flagged);
    }
    if (json_object_get(msg, "isUnread")) {
        readprop(msg, "isUnread", 1, invalid, "b", &unread);
    }
    if (json_object_get(msg, "isAnswered")) {
        readprop(msg, "isAnswered", 1, invalid, "b", &answered);
    }
    if (JNOTNULL(json_object_get(msg, "mailboxIds"))) {
        dstmailboxes = json_pack("{}");
        json_array_foreach(json_object_get(msg, "mailboxIds"), i, val) {
            char *name = NULL;
            id = json_string_value(val);
            if (id && *id == '#') {
                id = hash_lookup(id + 1, &req->idmap->mailboxes);
            }
            if (id && (name = jmapmbox_find_uniqueid(req, id))) {
                json_object_set_new(dstmailboxes, id, json_string(name));
                free(name);
            } else {
                struct buf buf = BUF_INITIALIZER;
                buf_printf(&buf, "mailboxIds[%zu]", i);
                json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                buf_free(&buf);
            }
        }
        if (!json_object_size(dstmailboxes)) {
            json_array_append_new(invalid, json_string("mailboxIds"));
        }
    } else {
        dstmailboxes = json_deep_copy(srcmailboxes);
    }
    if (json_array_size(invalid)) {
        return 0;
    }

    /* Determine mailbox differences */
    newmailboxes = json_deep_copy(dstmailboxes);
    json_object_foreach(srcmailboxes, id, val) {
        json_object_del(newmailboxes, id);
    }
    delmailboxes = json_deep_copy(srcmailboxes);
    json_object_foreach(dstmailboxes, id, val) {
        json_object_del(delmailboxes, id);
    }
    oldmailboxes = json_deep_copy(srcmailboxes);
    json_object_foreach(newmailboxes, id, val) {
        json_object_del(oldmailboxes, id);
    }
    json_object_foreach(delmailboxes, id, val) {
        json_object_del(oldmailboxes, id);
    }

    /* Check mailbox ACL for shared accounts */
    if (strcmp(req->accountid, req->userid)) {
        struct msgupdate_checkacl_rock rock = {
            req, newmailboxes, delmailboxes, oldmailboxes
        };
        r = jmap_mboxlist(req, msgupdate_checkacl_cb, &rock, 0);
        if (r) goto done;
    }

    /* Update index record system flags. */
    r = jmap_openmbox(req, mboxname, &mbox, 1);
    if (r) goto done;

    r = msgrecord_find(mbox, uid, &mrw);
    if (r) goto done;

    r = updaterecord(mrw, flagged, unread, answered);
    if (r) goto done;

    r = msgrecord_rewrite(mrw);
    if (r) goto done;

    /* Update record in kept mailboxes, except its master copy. */
    json_object_del(oldmailboxes, mbox->uniqueid);
    if (json_object_size(oldmailboxes)) {
        struct updaterecord_data data = {
            req, oldmailboxes, flagged, unread, answered
        };

        r = conversations_guid_foreach(req->cstate, msgid+1, updaterecord_cb, &data);
        if (r) goto done;
    }

    /* Copy master copy to new mailboxes */
    json_object_foreach(newmailboxes, id, val) {
        const char *dstname = json_string_value(val);
        struct mailbox *dst = NULL;

        if (!strcmp(mboxname, dstname))
            continue;

        r = jmap_openmbox(req, dstname, &dst, 1);
        if (r) goto done;

        r = copyrecord(req, mbox, dst, mrw);

        jmap_closembox(req, &dst);
        if (r) goto done;
    }

    /* Remove message from mailboxes. We've checked the required ACLs already,
     * so any error here is fatal */
    if (json_object_size(delmailboxes)) {
        struct delrecord_data data = { req, 0, delmailboxes };

        r = conversations_guid_foreach(req->cstate, msgid+1, delrecord_cb, &data);
        if (r) goto done;
    }

done:
    if (mrw) msgrecord_unref(&mrw);
    if (mbox) jmap_closembox(req, &mbox);
    if (mboxname) free(mboxname);
    if (oldmailboxes) json_decref(oldmailboxes);
    if (srcmailboxes) json_decref(srcmailboxes);
    if (dstmailboxes) json_decref(dstmailboxes);
    if (newmailboxes) json_decref(newmailboxes);
    if (delmailboxes) json_decref(delmailboxes);

    if (r) syslog(LOG_ERR, "jmapmsg_update: %s", error_message(r));
    return r;
}

static int msgdelete_checkacl_cb(const conv_guidrec_t *rec, void *rock)
{
    jmap_req_t *req = rock;
    int r = 0;
    mbentry_t *mbentry = NULL;

    if (rec->part) return 0;

    r = mboxlist_lookup(rec->mboxname, &mbentry, NULL);
    if (r) return r;

    int rights = jmap_myrights(req, mbentry);
    if (!(rights & ACL_DELETEMSG)) {
        r = IMAP_PERMISSION_DENIED;
    }

    mboxlist_entry_free(&mbentry);
    return r;
}


static int jmapmsg_delete(jmap_req_t *req, const char *msgid)
{
    int r;
    struct delrecord_data data = { req, 0, NULL };

    if (msgid[0] != 'M')
        return IMAP_NOTFOUND;

    /* Check mailbox ACL for shared accounts */
    if (strcmp(req->accountid, req->userid)) {
        r = conversations_guid_foreach(req->cstate, msgid+1, msgdelete_checkacl_cb, req);
        if (r) return r;
    }

    /* Delete messages */
    r = conversations_guid_foreach(req->cstate, msgid+1, delrecord_cb, &data);
    if (r) return r;

    return data.deleted ? 0 : IMAP_NOTFOUND;
}

static int setMessages(jmap_req_t *req)
{
    int r = 0;
    json_t *set = NULL, *create, *update, *destroy, *state, *item;

    state = json_object_get(req->args, "ifInState");
    if (JNOTNULL(state)) {
        if (jmap_cmpstate(req, state, 0/*mbtype*/)) {
            json_array_append_new(req->response, json_pack("[s, {s:s}, s]",
                        "error", "type", "stateMismatch", req->tag));
            goto done;
        }
    }
    set = json_pack("{s:s}", "accountId", req->accountid);
    json_object_set_new(set, "oldState", state);

    create = json_object_get(req->args, "create");
    if (create) {
        json_t *created = json_pack("{}");
        json_t *notCreated = json_pack("{}");
        const char *key;
        json_t *msg;

        json_object_foreach(create, key, msg) {
            json_t *invalid = json_pack("[]");
            json_t *err = NULL;

            if (!strlen(key)) {
                err = json_pack("{s:s}", "type", "invalidArguments");
                json_object_set_new(notCreated, key, err);
                continue;
            }

            char *msgid = NULL;
            r = jmapmsg_create(req, msg, &msgid, invalid);
            if (json_array_size(invalid)) {
                err = json_pack("{s:s, s:o}",
                        "type", "invalidProperties", "properties", invalid);
                json_object_set_new(notCreated, key, err);
                free(msgid);
                continue;
            }
            json_decref(invalid);

            if (r == IMAP_QUOTA_EXCEEDED) {
                err = json_pack("{s:s}", "type", "maxQuotaReached");
                json_object_set_new(notCreated, key, err);
                free(msgid);
                continue;
            } else if (r) {
                free(msgid);
                goto done;
            }

            json_object_set_new(created, key, json_pack("{s:s}", "id", msgid));
            hash_insert(key, msgid, &req->idmap->messages); // hash takes ownership of msgid
        }

        if (json_object_size(created)) {
            json_object_set(set, "created", created);
        }
        json_decref(created);

        if (json_object_size(notCreated)) {
            json_object_set(set, "notCreated", notCreated);
        }
        json_decref(notCreated);
    }

    update = json_object_get(req->args, "update");
    if (update) {
        json_t *updated = json_pack("{}");
        json_t *notUpdated = json_pack("{}");
        const char *id;
        json_t *msg;

        json_object_foreach(update, id, msg) {
            json_t *invalid = json_pack("[]");
            /* Validate id */
            if (!id) {
                continue;
            }
            if (id && id[0] == '#') {
                const char *newid = hash_lookup(id + 1, &req->idmap->mailboxes);
                if (!newid) {
                    json_t *err = json_pack("{s:s}", "type", "notFound");
                    json_object_set_new(notUpdated, id, err);
                    continue;
                }
                id = newid;
            }
            /* Update message */
            if ((r = jmapmsg_update(req, id, msg, invalid))) {
                json_decref(invalid);
                if (r == IMAP_NOTFOUND) {
                    json_t *err= json_pack("{s:s}", "type", "notFound");
                    json_object_set_new(notUpdated, id, err);
                    r = 0;
                    continue;
                }
                else if (r == IMAP_PERMISSION_DENIED) {
                    json_t *err= json_pack("{s:s}", "type", "forbidden");
                    json_object_set_new(notUpdated, id, err);
                    r = 0;
                    continue;
                }
                else {
                    goto done;
                }
            }
            if (json_array_size(invalid)) {
                json_t *err = json_pack("{s:s, s:o}",
                        "type", "invalidProperties", "properties", invalid);
                json_object_set_new(notUpdated, id, err);
                continue;
            }
            json_object_set_new(updated, id, json_null());
            json_decref(invalid);
        }

        if (json_object_size(updated)) {
            json_object_set(set, "updated", updated);
        }
        json_decref(updated);

        if (json_object_size(notUpdated)) {
            json_object_set(set, "notUpdated", notUpdated);
        }
        json_decref(notUpdated);
    }

    destroy = json_object_get(req->args, "destroy");
    if (destroy) {
        json_t *destroyed = json_pack("[]");
        json_t *notDestroyed = json_pack("{}");
        json_t *id;
        size_t i;

        json_array_foreach(destroy, i, id) {
            const char *msgid = json_string_value(id);
            /* Validate msgid */
            if (!msgid) {
                continue;
            }
            if (msgid && msgid[0] == '#') {
                const char *newmsgid = hash_lookup(msgid + 1, &req->idmap->mailboxes);
                if (!newmsgid) {
                    json_t *err = json_pack("{s:s}", "type", "notFound");
                    json_object_set_new(notDestroyed, msgid, err);
                    continue;
                }
                msgid = newmsgid;
            }
            /* Delete message */
            if ((r = jmapmsg_delete(req, msgid))) {
                if (r == IMAP_NOTFOUND) {
                    json_t *err = json_pack("{s:s}", "type", "notFound");
                    json_object_set_new(notDestroyed, msgid, err);
                    r = 0;
                    continue;
                } else if (r == IMAP_PERMISSION_DENIED) {
                    json_t *err = json_pack("{s:s}", "type", "forbidden");
                    json_object_set_new(notDestroyed, msgid, err);
                    r = 0;
                    continue;
                } else if (r) {
                    goto done;
                }
            }
            json_array_append_new(destroyed, json_string(msgid));
        }

        if (json_array_size(destroyed)) {
            json_object_set(set, "destroyed", destroyed);
        }
        json_decref(destroyed);
        if (json_object_size(notDestroyed)) {
            json_object_set(set, "notDestroyed", notDestroyed);
        }
        json_decref(notDestroyed);
    }

    json_object_set_new(set, "newState", jmap_getstate(req, 0/*mbtype*/));
    json_object_set_new(set, "accountId", json_string(req->accountid));

    json_incref(set);
    item = json_pack("[]");
    json_array_append_new(item, json_string("messagesSet"));
    json_array_append_new(item, set);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

done:
    if (set) json_decref(set);
    return r;
}

struct jmapmsg_import_data {
    struct buf msg_buf;
    const struct body *part;
};

int jmapmsg_import_cb(jmap_req_t *req __attribute__((unused)),
                      FILE *out, void *rock)
{
    struct jmapmsg_import_data *data = (struct jmapmsg_import_data*) rock;

    // we never need to pre-decode rfc822 messages, they're always 7bit (right?)
    const char *base = data->msg_buf.s;
    size_t len = data->msg_buf.len;
    if (data->part) {
        base += data->part->content_offset;
        len = data->part->content_size;
    }

    struct protstream *stream = prot_readmap(base, len);

    int r = message_copy_strict(stream, out, len, 0);

    prot_free(stream);

    return r;
}

int jmapmsg_import(jmap_req_t *req, json_t *msg, json_t **createdmsg)
{
    struct jmapmsg_import_data data = { BUF_INITIALIZER, NULL };
    hash_table props = HASH_TABLE_INITIALIZER;
    struct body *body = NULL;
    struct mailbox *mbox = NULL;
    const char *blobid;
    json_t *mailboxids = json_object_get(msg, "mailboxIds");
    char *msgid = NULL;
    int system_flags = 0;
    char *mboxname = NULL;
    uint32_t uid;
    time_t internaldate = 0;
    msgrecord_t *mr = NULL;
    int r;

    /* Check mailbox ACL for shared accounts */
    if (strcmp(req->accountid, req->userid)) {
        struct msgupdate_checkacl_rock rock = {
            req, json_pack("{}") /*newmailboxes*/, NULL, NULL
        };
        size_t i;
        json_t *val;
        json_array_foreach(mailboxids, i, val) {
            json_object_set(rock.newmailboxes, json_string_value(val), json_true());
        }
        r = jmap_mboxlist(req, msgupdate_checkacl_cb, &rock, 0);
        json_decref(rock.newmailboxes);
        if (r) goto done;
    }

    blobid = json_string_value(json_object_get(msg, "blobId"));

    r = jmap_findblob(req, blobid, &mbox, &mr, &body, &data.part);
    if (r) goto done;

    r = msgrecord_get_body(mr, &data.msg_buf);
    if (r) goto done;

    jmap_closembox(req, &mbox);
    msgrecord_unref(&mr);

    /* Write the message to the file system */
    if (json_object_get(msg, "isDraft") == json_true())
        system_flags |= FLAG_DRAFT;
    if (json_object_get(msg, "isFlagged") == json_true())
        system_flags |= FLAG_FLAGGED;
    if (json_object_get(msg, "isAnswered") == json_true())
        system_flags |= FLAG_ANSWERED;
    if (json_object_get(msg, "isUnread") != json_true())
        system_flags |= FLAG_SEEN;

    /* check for internaldate */
    const char *datestr = json_string_value(json_object_get(msg, "date"));
    if (datestr) {
        time_from_iso8601(datestr, &internaldate);
    }

    r = jmapmsg_write(req, mailboxids, system_flags, internaldate,
                      jmapmsg_import_cb, &data, &msgid);
    if (r) goto done;

    /* Load its index record and convert to JMAP */
    r = jmapmsg_find(req, msgid, &mboxname, &uid);
    if (r) goto done;

    r = jmap_openmbox(req, mboxname, &mbox, 0);
    if (r) goto done;

    r = msgrecord_find(mbox, uid, &mr);
    if (r) goto done;

    construct_hash_table(&props, 4, 0);
    hash_insert("id", (void*)1, &props);
    hash_insert("blobId", (void*)1, &props);
    hash_insert("threadId", (void*)1, &props);
    hash_insert("size", (void*)1, &props);

    r = jmapmsg_from_record(req, &props, mr, createdmsg);
    if (r) goto done;

    jmap_closembox(req, &mbox);

done:
    free_hash_table(&props, NULL);
    buf_free(&data.msg_buf);
    if (msgid) free(msgid);
    if (mr) msgrecord_unref(&mr);
    if (mbox) jmap_closembox(req, &mbox);
    if (body) {
        message_free_body(body);
        free(body);
    }
    if (mboxname) free(mboxname);
    return r;
}

static int importMessages(jmap_req_t *req)
{
    int r = 0;
    json_t *res, *item, *msgs, *msg, *created, *notcreated;
    json_t *invalid, *invalidmbox;
    struct buf buf = BUF_INITIALIZER;
    const char *id;

    /* Parse and validate arguments. */
    invalid = json_pack("[]");
    invalidmbox = json_pack("[]");

    /* Import messages */
    created = json_pack("{}");
    notcreated = json_pack("{}");

    /* messages */
    msgs = json_object_get(req->args, "messages");
    json_object_foreach(msgs, id, msg) {
        const char *prefix;
        int b;
        size_t i;
        json_t *val;
        const char *s;

        buf_printf(&buf, "messages[%s]", id);
        prefix = buf_cstring(&buf);

        readprop_full(msg, prefix, "isDraft", 1, invalid, "b", &b);
        readprop_full(msg, prefix, "isFlagged", 1, invalid, "b", &b);
        readprop_full(msg, prefix, "isUnread", 1, invalid, "b", &b);
        readprop_full(msg, prefix, "isAnswered", 1, invalid, "b", &b);

        int pe = readprop_full(msg, prefix, "date", 0, invalid, "s", &s);
        if (pe > 0) {
            struct tm date;
            const char *p = strptime(s, "%Y-%m-%dT%H:%M:%SZ", &date);
            if (!p || *p) {
                char *val = strconcat(prefix, ".date", (char *)NULL);
                json_array_append_new(invalid, json_string(val));
                free(val);
            }
        }
        json_array_foreach(json_object_get(msg, "mailboxIds"), i, val) {
            char *name = NULL;
            const char *mboxid = json_string_value(val);
            if (mboxid && *mboxid == '#') {
                mboxid = hash_lookup(mboxid + 1, &req->idmap->mailboxes);
            }
            if (!mboxid || !(name = jmapmbox_find_uniqueid(req, mboxid))) {
                buf_printf(&buf, ".mailboxIds[%zu]", i);
                json_array_append_new(invalidmbox, json_string(buf_cstring(&buf)));
            }
            free(name);
        }
        if (json_array_size(json_object_get(msg, "mailboxIds")) == 0) {
            buf_printf(&buf, ".mailboxIds");
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
        }

        buf_reset(&buf);
    }
    if (!json_object_size(msgs)) {
        json_array_append_new(invalid, json_string("messages"));
    }

    /* Bail out for argument */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}",
                "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response,
                json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(invalid);

    /* Bail out for mailbox errors */
    if (json_array_size(invalidmbox)) {
        json_t *err = json_pack("{s:s, s:o}",
                "type", "invalidMailboxes", "mailboxes", invalidmbox);
        json_array_append_new(req->response,
                json_pack("[s,o,s]", "error", err, req->tag));
        r = 0;
        goto done;
    }
    json_decref(invalidmbox);

    json_object_foreach(msgs, id, msg) {
        json_t *mymsg;

        r = jmapmsg_import(req, msg, &mymsg);
        if (r == IMAP_NOTFOUND) {
            json_object_set_new(notcreated, id, json_pack("{s:s}",
                        "type", "attachmentNotFound"));
            r = 0;
            continue;
        }
        else if (r == IMAP_PERMISSION_DENIED) {
            json_object_set_new(notcreated, id, json_pack("{s:s}",
                        "type", "forbidden"));
            r = 0;
            continue;
        }
        else if (r == IMAP_MAILBOX_EXISTS) {
            json_object_set_new(notcreated, id, json_pack("{s:s}",
                        "type", "messageExists"));
            r = 0;
            continue;
        }
        else if (r == IMAP_QUOTA_EXCEEDED) {
            json_object_set_new(notcreated, id, json_pack("{s:s}",
                        "type", "maxQuotaReached"));
            r = 0;
            continue;
        }
        else if (r) {
            goto done;
        }

        json_object_set_new(created, id, mymsg);

        char *mymsgid = xstrdupnull(json_string_value(json_object_get(mymsg, "id")));
        hash_insert(id, mymsgid, &req->idmap->messages); /* idmap takes ownership */
    }

    /* Prepare the response */
    res = json_pack("{}");
    json_object_set_new(res, "accountId", json_string(req->accountid));
    json_object_set(res, "created", created);
    json_object_set(res, "notCreated", notcreated);

    item = json_pack("[]");
    json_array_append_new(item, json_string("messagesImported"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

done:
    json_decref(created);
    json_decref(notcreated);
    buf_free(&buf);
    return r;
}

static int getIdentities(jmap_req_t *req)
{
    int r = 0;
    json_t *res, *item, *val, *ids, *notfound, *identities, *me;
    struct buf buf = BUF_INITIALIZER;
    const char *s;
    size_t i;

    /* Parse and validate arguments. */
    json_t *invalid = json_pack("[]");

    /* ids */
    ids = json_object_get(req->args, "ids");
    json_array_foreach(ids, i, val) {
        if (!(s = json_string_value(val)) || !strlen(s)) {
            buf_printf(&buf, "ids[%zu]", i);
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            buf_reset(&buf);
        }
    }
    if (JNOTNULL(ids) && !json_is_array(ids)) {
        json_array_append_new(invalid, json_string("ids"));
    }

    /* Bail out for argument errors */
    if (json_array_size(invalid)) {
        json_t *err = json_pack("{s:s, s:o}", "type", "invalidArguments", "arguments", invalid);
        json_array_append_new(req->response, json_pack("[s,o,s]", "error", err, req->tag));
        goto done;
    }
    json_decref(invalid);

    notfound = json_pack("[]");
    identities = json_pack("[]");

    me = json_pack("{s:s s:s s:s s:s s:s s:s s:s s:b}",
            "id", req->userid,
            "name", "",
            "email", req->userid,
            "replyTo", "",
            "bcc", "",
            "textSignature", "",
            "htmlSignature", "",
            "mayDeleteIdentity", 0);
    if (!strchr(req->userid, '@')) {
        json_object_set_new(me, "email", json_string(""));
    }

    /* Build the identities */
    if (json_array_size(ids)) {
        json_array_foreach(ids, i, val) {
            if (strcmp(json_string_value(val), req->userid)) {
                json_array_append(notfound, val);
            }
            else {
                json_array_append(identities, me);
            }
        }
    } else if (!JNOTNULL(ids)) {
        json_array_append(identities, me);
    }
    json_decref(me);

    /* Prepare the response */
    res = json_pack("{}");
    json_object_set_new(res, "state", json_string("0"));
    json_object_set_new(res, "accountId", json_string(req->accountid));
    json_object_set_new(res, "list", identities);
    json_object_set_new(res, "notFound", notfound);

    item = json_pack("[]");
    json_array_append_new(item, json_string("identities"));
    json_array_append_new(item, res);
    json_array_append_new(item, json_string(req->tag));
    json_array_append_new(req->response, item);

done:
    buf_free(&buf);
    return r;
}
