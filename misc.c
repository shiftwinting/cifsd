/*
 *   fs/cifsd/misc.c
 *
 *   Copyright (C) 2015 Samsung Electronics Co., Ltd.
 *   Copyright (C) 2016 Namjae Jeon <namjae.jeon@protocolfreedom.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/kernel.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
#include <linux/xattr.h>
#endif

#include "glob.h"
#include "export.h"
#include "smb1pdu.h"
#include "smb2pdu.h"

/* Async ida to generate async id */
DEFINE_IDA(async_ida);

static struct {
	int index;
	char *name;
	char *prot;
	__u16 prot_id;
} protocols[] = {
	{CIFS_PROT, "\2NT LM 0.12", "NT1", 0},
#ifdef CONFIG_CIFS_SMB2_SERVER
	{SMB2_PROT, "\2SMB 2.002", "SMB2_02", SMB20_PROT_ID},
	{SMB21_PROT, "\2SMB 2.1", "SMB2_10", SMB21_PROT_ID},
	{SMB2X_PROT, "\2SMB 2.???", "SMB2_22", SMB2X_PROT_ID},
	{SMB30_PROT, "\2SMB 3.0", "SMB3_00", SMB30_PROT_ID},
	{SMB302_PROT, "\2SMB 3.02", "SMB3_02", SMB302_PROT_ID},
	{SMB311_PROT, "\2SMB 3.1.1", "SMB3_11", SMB311_PROT_ID},
#endif
};

inline int cifsd_min_protocol(void)
{
	return protocols[0].index;
}

inline int cifsd_max_protocol(void)
{
	return protocols[ARRAY_SIZE(protocols) - 1].index;
}

inline void remove_async_id(__u64 async_id)
{
	ida_simple_remove(&async_ida, (int)async_id);
}

int get_protocol_idx(char *str)
{
	int res = -1, i;
	int protocol_index = protocols[ARRAY_SIZE(protocols) - 1].index;
	int len = strlen(str);

	for (i = 0; i <= protocol_index; i++) {
		if (!strncmp(str, protocols[i].prot, len)) {
			cifsd_debug("selected %s dialect i = %d\n",
				protocols[i].prot, i);
			res = protocols[i].index;
			break;
		}
	}
	return res;
}

/**
 * check_smb_hdr() - check for valid smb request header
 * @smb:	smb header to be checked
 *
 * check for valid smb signature and packet direction(request/response)
 * TODO: properly check client authetication and tree authentication
 *
 * Return:      0 on success, otherwise 1
 */
static int check_smb_hdr(struct smb_hdr *smb)
{
	/* does it have the right SMB "signature" ? */
	if (*(__le32 *) smb->Protocol != SMB1_PROTO_NUMBER) {
		cifsd_debug("Bad protocol string signature header 0x%x\n",
			*(unsigned int *)smb->Protocol);
		return 1;
	} else
		cifsd_debug("got SMB\n");

	/* if it's not a response then accept */
	/* TODO : check for oplock break */
	if (!(smb->Flags & SMBFLG_RESPONSE))
		return 0;

	cifsd_debug("Server sent request, not response\n");
	return 1;
}

/**
 * check_smb2_hdr() - helper function to check for valid smb2 request header
 * @smb:	smb2 header to be checked
 *
 * Return:      0 on success, otherwise 1
 */
static inline int check_smb2_hdr(struct smb2_hdr *smb)
{
	if (!(smb->Flags & SMB2_FLAGS_SERVER_TO_REDIR))
		return 0;
	return 1;
}

/**
 * check_smb2_hdr() - check for valid smb2 request header
 * @buf:	smb2 header to be checked
 *
 * check for valid smb signature and packet direction(request/response)
 *
 * Return:      0 on success, otherwise 1
 */
int check_smb_message(char *buf)
{

	if (*(__le32 *)((struct smb2_hdr *)buf)->ProtocolId ==
			SMB2_PROTO_NUMBER) {

		cifsd_debug("got SMB2 command\n");
		return check_smb2_hdr((struct smb2_hdr *)buf);

	}

	return check_smb_hdr((struct smb_hdr *)buf);

}

/**
 * add_request_to_queue() - check a request for addition to pending smb work
 *				queue
 * @smb_work:	smb request work
 *
 * Return:      true if not add to queue, otherwise false
 */
void add_request_to_queue(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct list_head *requests_queue = NULL;

	if (*(__le32 *)((struct smb2_hdr *)smb_work->buf)->ProtocolId ==
			SMB2_PROTO_NUMBER) {
		unsigned int command = server->ops->get_cmd_val(smb_work);

		if (command != SMB2_CANCEL) {
			if (command == SMB2_CHANGE_NOTIFY ||
				command == SMB2_LOCK) {
				smb_work->async =
					kmalloc(sizeof(struct async_info),
					GFP_KERNEL);
				smb_work->type = ASYNC;

				smb_work->async->async_id =
					(__u64) ida_simple_get(&async_ida, 1, 0,
					GFP_KERNEL);

				requests_queue = &server->async_requests;
				smb_work->async->async_status = ASYNC_WAITING;
			} else {
				requests_queue = &server->requests;
				smb_work->type = SYNC;
			}
		}
	} else {
		if (server->ops->get_cmd_val(smb_work) != SMB_COM_NT_CANCEL)
			requests_queue = &server->requests;
	}

	if (requests_queue) {
		spin_lock(&server->request_lock);
		list_add_tail(&smb_work->request_entry, requests_queue);
		smb_work->added_in_request_list = 1;
		spin_unlock(&server->request_lock);
	}
}

/**
 * dump_smb_msg() - print smb packet for debugging
 * @buf:		smb packet
 * @smb_buf_length:	packet print length
 *
 */
void dump_smb_msg(void *buf, int smb_buf_length)
{
	int i, j;
	char debug_line[33];
	unsigned char *buffer = buf;

	if (likely(cifsd_debug_enable != 2))
		return;

	for (i = 0, j = 0; i < smb_buf_length; i++, j++) {
		if (i % 16 == 0) {
			/* have reached the beginning of line */
			pr_err("%04x ", i);
			pr_cont("| ");
			j = 0;
		}

		pr_cont("%02x ", buffer[i]);
		debug_line[2 * j] = ' ';
		if (isprint(buffer[i]))
			debug_line[1 + (2 * j)] = buffer[i];
		else
			debug_line[1 + (2 * j)] = '_';

		if (i % 16 == 15) {
			/* reached end of line, time to print ascii */
			debug_line[32] = 0;
			pr_cont(" | %s\n", debug_line);
		}
	}
	for (; j < 16; j++) {
		pr_cont("   ");
		debug_line[2 * j] = ' ';
		debug_line[1 + (2 * j)] = ' ';
	}
	pr_cont(" | %s\n", debug_line);
	return;
}

/**
 * switch_req_buf() - switch to big request buffer
 * @server:     TCP server instance of connection
 *
 * Return:      0 on success, otherwise -ENOMEM
 */
int switch_req_buf(struct tcp_server_info *server)
{
	char *buf = server->smallbuf;
	unsigned int pdu_length = get_rfc1002_length(buf);
	unsigned int hdr_len;

#ifdef CONFIG_CIFS_SMB2_SERVER
	hdr_len = MAX_SMB2_HDR_SIZE;
#else
	hdr_len = MAX_CIFS_HDR_SIZE;
#endif

	/* request can fit in large request buffer i.e. < 64K */
	if (pdu_length <= SMBMaxBufSize + hdr_len - 4) {
		cifsd_debug("switching to large buffer\n");
		server->large_buf = true;
		memcpy(server->bigbuf, buf, server->total_read);
	} else if (pdu_length <= CIFS_DEFAULT_IOSIZE + hdr_len - 4) {
		/* allocate big buffer for large write request i.e. > 64K */
		server->wbuf = vmalloc(CIFS_DEFAULT_IOSIZE + hdr_len);
		if (!server->wbuf) {
			cifsd_debug("failed to alloc mem\n");
			return -ENOMEM;
		}
		memcpy(server->wbuf, buf, server->total_read);

		/* as wbuf is used for request, free both small and big buf */
		mempool_free(server->smallbuf, cifsd_sm_req_poolp);
		mempool_free(server->bigbuf, cifsd_req_poolp);
		server->large_buf = false;
		server->smallbuf = NULL;
		server->bigbuf = NULL;
	} else {
		cifsd_debug("SMB request too long (%u bytes)\n", pdu_length);
		return -ECONNABORTED;
	}

	return 0;
}

/**
 * switch_rsp_buf() - switch to large response buffer
 * @smb_work:	smb request work
 *
 * Return:      0 on success, otherwise -ENOMEM
 */
int switch_rsp_buf(struct smb_work *smb_work)
{
	char *buf;
	if (smb_work->rsp_large_buf) {
		cifsd_debug("already using rsp_large_buf\n");
		return 0;
	}

	buf = mempool_alloc(cifsd_rsp_poolp, GFP_NOFS);
	if (!buf) {
		cifsd_debug("failed to alloc mem\n");
		return -ENOMEM;
	}

	/* free small buf and switch to large rsp buffer */
	cifsd_debug("switching to large rsp buf\n");
	memcpy(buf, smb_work->rsp_buf, MAX_CIFS_SMALL_BUFFER_SIZE);
	mempool_free(smb_work->rsp_buf, cifsd_sm_rsp_poolp);

	smb_work->rsp_buf = buf;
	smb_work->rsp_large_buf = true;
	return 0;
}

/**
 * is_smb_request() - check for valid smb request type
 * @server:     TCP server instance of connection
 * @type:	smb request type
 *
 * Return:      true on success, otherwise false
 */
bool is_smb_request(struct tcp_server_info *server, unsigned char type)
{
	switch (type) {
	case RFC1002_SESSION_MESSAGE:
		/* Regular SMB request */
		return true;
	case RFC1002_SESSION_KEEP_ALIVE:
		cifsd_debug("RFC 1002 session keep alive\n");
		break;
	default:
		cifsd_debug("RFC 1002 unknown request type 0x%x\n", type);
	}

	return false;
}

int find_matching_smb1_dialect(int start_index, char *cli_dialects,
		__le16 byte_count)
{
	int i, smb1_index, cli_count, bcount, dialect_id = BAD_PROT_ID;
	char *dialects = NULL;

	if (unlikely(start_index >= ARRAY_SIZE(protocols))) {
		cifsd_err("bad start_index %d\n", start_index);
		return dialect_id;
	}

	for (i = start_index; i >= CIFS_PROT; i--) {
		smb1_index = 0;
		bcount = le16_to_cpu(byte_count);
		dialects = cli_dialects;

		while (bcount) {
			cli_count = strlen(dialects);
			cifsd_debug("client requested dialect %s\n",
					dialects);
			if (!strncmp(dialects, protocols[i].name,
						cli_count)) {
				if (i >= server_min_pr && i <= server_max_pr) {
					cifsd_debug("selected %s dialect\n",
							protocols[i].name);
					if (i == CIFS_PROT)
						dialect_id = smb1_index;
					else
						dialect_id =
						protocols[i].prot_id;
				}
				goto out;
			}
			bcount -= (++cli_count);
			dialects += cli_count;
			smb1_index++;
		}
	}

out:
	return dialect_id;
}

#ifdef CONFIG_CIFS_SMB2_SERVER
/**
 * find_matching_smb2_dialect() - find the greatest dialect between dialects
 * client and server support.
 * @start_index:	start protocol id for lookup
 * @cli_dialects:	client dialects
 * @srv_dialects:	server dialects
 * @directs_count:	client dialect count
 *
 * Return:      0
 */
int find_matching_smb2_dialect(int start_index, __le16 *cli_dialects,
	__le16 dialects_count)
{
	int i, dialect_id = BAD_PROT_ID;
	int count;

	for (i = start_index; i >= SMB2_PROT; i--) {
		count = le16_to_cpu(dialects_count);
		while (--count >= 0) {
			cifsd_debug("client requested dialect 0x%x\n",
				le16_to_cpu(cli_dialects[count]));
			if (le16_to_cpu(cli_dialects[count]) ==
					protocols[i].prot_id) {
				if (i >= server_min_pr && i <= server_max_pr) {
					cifsd_debug("selected %s dialect\n",
							protocols[i].name);
					dialect_id = protocols[i].prot_id;
				}
				goto out;
			}
		}
	}

out:
	return dialect_id;
}
#endif

/**
 * negotiate_dialect() - negotiate smb dialect with smb client
 * @buf:	smb header
 *
 * Return:     protocol index on success, otherwise bad protocol id error
 */
int negotiate_dialect(void *buf)
{
	int start_index, ret = BAD_PROT_ID;

#ifdef CONFIG_CIFS_SMB2_SERVER
	start_index = SMB311_PROT;
#else
	start_index = CIFS_PROT;
#endif

	if (*(__le32 *)((struct smb_hdr *)buf)->Protocol ==
			SMB1_PROTO_NUMBER) {
		/* SMB1 neg protocol */
		NEGOTIATE_REQ *req = (NEGOTIATE_REQ *)buf;
		ret = find_matching_smb1_dialect(start_index,
			req->DialectsArray, le16_to_cpu(req->ByteCount));
	} else if (*(__le32 *)((struct smb2_hdr *)buf)->ProtocolId ==
			SMB2_PROTO_NUMBER) {
#ifdef CONFIG_CIFS_SMB2_SERVER
		/* SMB2 neg protocol */
		struct smb2_negotiate_req *req;
		req = (struct smb2_negotiate_req *)buf;
		ret = find_matching_smb2_dialect(start_index, req->Dialects,
			le16_to_cpu(req->DialectCount));
#endif
	}

	return ret;
}

struct cifsd_sess *lookup_session_on_conn(struct tcp_server_info *server,
		uint64_t sess_id)
{
	struct cifsd_sess *sess;
	struct list_head *tmp, *t;

	list_for_each_safe(tmp, t, &server->cifsd_sess) {
		sess = list_entry(tmp, struct cifsd_sess, cifsd_ses_list);
		if (sess->sess_id == sess_id)
			return sess;
	}

	cifsd_err("User session(ID : %llu) not found\n", sess_id);
	return NULL;
}

/**
 * validate_sess_handle() - check for valid session handle
 * @sess:	handle to be validated
 *
 * Return:      matching session handle, otherwise NULL
 */
struct cifsd_sess *validate_sess_handle(struct cifsd_sess *session)
{
	struct cifsd_sess *sess;
	struct list_head *tmp, *t;

	list_for_each_safe(tmp, t, &cifsd_session_list) {
		sess = list_entry(tmp, struct cifsd_sess,
				cifsd_ses_global_list);
		if (sess == session)
			return sess;
	}

	cifsd_err("session(%p) not found\n", session);
	return NULL;
}

#ifndef CONFIG_CIFS_SMB2_SERVER
void init_smb2_0_server(struct tcp_server_info *server) { }
void init_smb2_1_server(struct tcp_server_info *server) { }
void init_smb3_0_server(struct tcp_server_info *server) { }
void init_smb3_02_server(struct tcp_server_info *server) { }
void init_smb3_11_server(struct tcp_server_info *server) { }
int is_smb2_neg_cmd(struct smb_work *smb_work)
{
	return 0;
}

bool is_chained_smb2_message(struct smb_work *smb_work)
{
	return 0;
}

void init_smb2_neg_rsp(struct smb_work *smb_work)
{
}
int is_smb2_rsp(struct smb_work *smb_work)
{
	return 0;
};
#endif

int smb_store_cont_xattr(struct path *path, char *prefix, void *value,
	ssize_t v_len)
{
	int err;

	err = smb_vfs_setxattr(NULL, path, prefix, value, v_len, 0);
	if (err)
		cifsd_debug("setxattr failed, err %d\n", err);

	return err;
}

ssize_t smb_find_cont_xattr(struct path *path, char *prefix, int p_len,
	char **value, int flags)
{
	char *name, *xattr_list = NULL, *tmp_a = NULL, *tmp_b = NULL;
	ssize_t value_len = -ENOENT, xattr_list_len;

	xattr_list_len = smb_vfs_listxattr(path->dentry, &xattr_list,
		XATTR_LIST_MAX);
	if (xattr_list_len < 0) {
		goto out;
	} else if (!xattr_list_len) {
		cifsd_debug("empty xattr in the file\n");
		goto out;
	}

	tmp_a = kmalloc(p_len, GFP_KERNEL);
	tmp_b = kmalloc(p_len, GFP_KERNEL);
	if (!tmp_a || !tmp_b) {
		xattr_list_len = -ENOMEM;
		goto out;
	}

	memcpy(tmp_a, prefix, p_len);

	for (name = xattr_list; name - xattr_list < xattr_list_len;
			name += strlen(name) + 1) {
		cifsd_debug("%s, len %zd\n", name, strlen(name));
		memcpy(tmp_b, name, p_len);

		if (strncasecmp(tmp_a, tmp_b, p_len))
			continue;

		value_len = smb_vfs_getxattr(path->dentry, name, value, flags);
		if (value_len < 0)
			cifsd_err("failed to get xattr in file\n");
		break;
	}

out:
	if (xattr_list)
		vfree(xattr_list);
	kfree(tmp_a);
	kfree(tmp_b);
	return value_len;
}

int get_pos_strnstr(const char *s1, const char *s2, size_t len)
{
	size_t l2;
	int index = 0;

	l2 = strlen(s2);
	if (!l2)
		return 0;

	while (len >= l2) {
		len--;
		if (!memcmp(s1, s2, l2))
			return index;
		s1++;
		index++;
	}
	return 0;
}

int smb_check_shared_mode(struct file *filp, struct cifsd_file *curr_fp)
{
	int rc = 0;
	struct cifsd_file *prev_fp;

	/*
	 * Lookup fp in global table, and check desired access and
	 * shared mode between previous open and current open.
	 */
	hash_for_each_possible(global_name_table, prev_fp, node,
			(unsigned long)file_inode(filp))
		if (file_inode(filp) == GET_FP_INODE(prev_fp)) {
			if (prev_fp->is_stream && curr_fp->is_stream) {
				if (strcmp(prev_fp->stream_name,
					curr_fp->stream_name)) {
					continue;
				}

				if (curr_fp->cdoption == FILE_SUPERSEDE_LE) {
					cifsd_err("not allow FILE_SUPERSEDE_LE if file is already opened with ADS\n");
					rc = -ESHARE;
					break;
				}
			}

			if (prev_fp->delete_pending) {
				rc = -EBUSY;
				break;
			}

			if (prev_fp->attrib_only != curr_fp->attrib_only)
				continue;

			if (!(prev_fp->saccess & (FILE_SHARE_DELETE_LE)) &&
					curr_fp->daccess & (FILE_DELETE_LE |
				FILE_GENERIC_ALL_LE | FILE_MAXIMAL_ACCESS_LE)) {
				cifsd_err("previous filename don't have share delete\n");
				cifsd_err("previous file's share access : 0x%x, current file's desired access : 0x%x\n",
					prev_fp->saccess, curr_fp->daccess);
				rc = -ESHARE;
				break;
			}

			if (prev_fp->is_stream && curr_fp->delete_on_close) {
				prev_fp->delete_pending = 1;
				prev_fp->delete_on_close = 1;
				curr_fp->delete_on_close = 0;
			}

			/*
			 * Only check FILE_SHARE_DELETE if stream opened and
			 * normal file opened.
			 */
			if (prev_fp->is_stream && !curr_fp->is_stream)
				continue;

			if (!(prev_fp->saccess & (FILE_SHARE_READ_LE)) &&
				curr_fp->daccess & (FILE_READ_DATA_LE |
					FILE_GENERIC_READ_LE |
					FILE_GENERIC_ALL_LE |
					FILE_MAXIMAL_ACCESS_LE)) {
				cifsd_err("previous filename don't have share read\n");
				cifsd_err("previous file's share access : 0x%x, current file's desired access : 0x%x\n",
					prev_fp->saccess, curr_fp->daccess);
				rc = -ESHARE;
				break;
			}

			if (!(prev_fp->saccess & (FILE_SHARE_WRITE_LE)) &&
				curr_fp->daccess & (FILE_WRITE_DATA_LE |
					FILE_GENERIC_WRITE_LE |
					FILE_GENERIC_ALL_LE |
					FILE_MAXIMAL_ACCESS_LE)) {
				cifsd_err("previous filename don't have share write\n");
				cifsd_err("previous file's share access : 0x%x, current file's desired access : 0x%x\n",
					prev_fp->saccess, curr_fp->daccess);
				rc = -ESHARE;
				break;
			}

			if (prev_fp->daccess & (FILE_READ_DATA_LE |
					FILE_GENERIC_READ_LE |
					FILE_GENERIC_ALL_LE |
					FILE_MAXIMAL_ACCESS_LE) &&
				!(curr_fp->saccess & FILE_SHARE_READ_LE)) {
				cifsd_err("previous filename don't have desired read access\n");
				cifsd_err("previous file's desired access : 0x%x, current file's share access : 0x%x\n",
					prev_fp->daccess, curr_fp->saccess);
				rc = -ESHARE;
				break;
			}

			if (prev_fp->daccess & (FILE_WRITE_DATA_LE |
					FILE_GENERIC_WRITE_LE |
					FILE_GENERIC_ALL_LE |
					FILE_MAXIMAL_ACCESS_LE) &&
				!(curr_fp->saccess & FILE_SHARE_WRITE_LE)) {
				cifsd_err("previous filename don't have desired write access\n");
				cifsd_err("previous file's desired access : 0x%x, current file's share access : 0x%x\n",
					prev_fp->daccess, curr_fp->saccess);
				rc = -ESHARE;
				break;
			}

			if (prev_fp->daccess & (FILE_DELETE_LE |
					FILE_GENERIC_ALL_LE |
					FILE_MAXIMAL_ACCESS_LE) &&
				!(curr_fp->saccess & FILE_SHARE_DELETE_LE)) {
				cifsd_err("previous filename don't have desired delete access\n");
				cifsd_err("previous file's desired access : 0x%x, current file's share access : 0x%x\n",
					prev_fp->daccess, curr_fp->saccess);
				rc = -ESHARE;
				break;
			}
		}

	return rc;
}

struct cifsd_file *find_fp_in_hlist_using_inode(struct inode *inode)
{
	struct cifsd_file *fp;

	hash_for_each_possible(global_name_table, fp, node,
			(unsigned long)inode)
		if (inode == GET_FP_INODE(fp))
			return fp;

	return NULL;
}

char *alloc_data_mem(size_t size)
{
	/*
	 * Use vzalloc area for allocation > 16KB,
	 * otherwise use kzalloc
	 */
	if (size <= (PAGE_SIZE << PAGE_ALLOC_KMEM_ORDER))
		return kzalloc(size, GFP_KERNEL);

	return vzalloc(size);
}
