
/*
 * $Id: disk.cc,v 1.119 1998/07/06 19:43:21 wessels Exp $
 *
 * DEBUG: section 6     Disk I/O Routines
 * AUTHOR: Harvest Derived
 *
 * SQUID Internet Object Cache  http://squid.nlanr.net/Squid/
 * --------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from the
 *  Internet community.  Development is led by Duane Wessels of the
 *  National Laboratory for Applied Network Research and funded by
 *  the National Science Foundation.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *  
 */

/*
 * Copyright (c) 1994, 1995.  All rights reserved.
 *  
 *   The Harvest software was developed by the Internet Research Task
 *   Force Research Group on Resource Discovery (IRTF-RD):
 *  
 *         Mic Bowman of Transarc Corporation.
 *         Peter Danzig of the University of Southern California.
 *         Darren R. Hardy of the University of Colorado at Boulder.
 *         Udi Manber of the University of Arizona.
 *         Michael F. Schwartz of the University of Colorado at Boulder.
 *         Duane Wessels of the University of Colorado at Boulder.
 *  
 *   This copyright notice applies to software in the Harvest
 *   ``src/'' directory only.  Users should consult the individual
 *   copyright notices in the ``components/'' subdirectories for
 *   copyright information about other software bundled with the
 *   Harvest source code distribution.
 *  
 * TERMS OF USE
 *   
 *   The Harvest software may be used and re-distributed without
 *   charge, provided that the software origin and research team are
 *   cited in any use of the system.  Most commonly this is
 *   accomplished by including a link to the Harvest Home Page
 *   (http://harvest.cs.colorado.edu/) from the query page of any
 *   Broker you deploy, as well as in the query result pages.  These
 *   links are generated automatically by the standard Broker
 *   software distribution.
 *   
 *   The Harvest software is provided ``as is'', without express or
 *   implied warranty, and with no support nor obligation to assist
 *   in its use, correction, modification or enhancement.  We assume
 *   no liability with respect to the infringement of copyrights,
 *   trade secrets, or any patents, and are not responsible for
 *   consequential damages.  Proper use of the Harvest software is
 *   entirely the responsibility of the user.
 *  
 * DERIVATIVE WORKS
 *  
 *   Users may make derivative works from the Harvest software, subject 
 *   to the following constraints:
 *  
 *     - You must include the above copyright notice and these 
 *       accompanying paragraphs in all forms of derivative works, 
 *       and any documentation and other materials related to such 
 *       distribution and use acknowledge that the software was 
 *       developed at the above institutions.
 *  
 *     - You must notify IRTF-RD regarding your distribution of 
 *       the derivative work.
 *  
 *     - You must clearly notify users that your are distributing 
 *       a modified version and not the original Harvest software.
 *  
 *     - Any derivative product is also subject to these copyright 
 *       and use restrictions.
 *  
 *   Note that the Harvest software is NOT in the public domain.  We
 *   retain copyright, as specified above.
 *  
 * HISTORY OF FREE SOFTWARE STATUS
 *  
 *   Originally we required sites to license the software in cases
 *   where they were going to build commercial products/services
 *   around Harvest.  In June 1995 we changed this policy.  We now
 *   allow people to use the core Harvest software (the code found in
 *   the Harvest ``src/'' directory) for free.  We made this change
 *   in the interest of encouraging the widest possible deployment of
 *   the technology.  The Harvest software is really a reference
 *   implementation of a set of protocols and formats, some of which
 *   we intend to standardize.  We encourage commercial
 *   re-implementations of code complying to this set of standards.  
 */

#include "squid.h"

#define DISK_LINE_LEN  1024

typedef struct disk_ctrl_t {
    int fd;
    void *data;
} disk_ctrl_t;


typedef struct open_ctrl_t {
    FOCB *callback;
    void *callback_data;
    char *path;
} open_ctrl_t;

static AIOCB diskHandleWriteComplete;
static AIOCB diskHandleReadComplete;
static PF diskHandleRead;
static PF diskHandleWrite;
static void file_open_complete(void *, int, int);

void
disk_init(void)
{
#if USE_ASYNC_IO
    aioClose(dup(0));
#endif
}

/* Open a disk file. Return a file descriptor */
int
file_open(const char *path, int mode, FOCB * callback, void *callback_data, void *tag)
{
    int fd;
    open_ctrl_t *ctrlp;

    ctrlp = xmalloc(sizeof(open_ctrl_t));
    ctrlp->path = xstrdup(path);
    ctrlp->callback = callback;
    ctrlp->callback_data = callback_data;

    if (mode & O_WRONLY)
	mode |= O_APPEND;
    mode |= SQUID_NONBLOCK;

    /* Open file */
#if USE_ASYNC_IO
    if (callback != NULL) {
	aioOpen(path, mode, 0644, file_open_complete, ctrlp, tag);
	return DISK_OK;
    }
#endif
    fd = open(path, mode, 0644);
    file_open_complete(ctrlp, fd, errno);
    if (fd < 0)
	return DISK_ERROR;
    return fd;
}


static void
file_open_complete(void *data, int fd, int errcode)
{
    open_ctrl_t *ctrlp = (open_ctrl_t *) data;

    if (fd == -2 && errcode == -2) {	/* Cancelled - clean up */
	if (ctrlp->callback)
	    (ctrlp->callback) (ctrlp->callback_data, fd, errcode);
	xfree(ctrlp->path);
	xfree(ctrlp);
	return;
    }
    if (fd < 0) {
	errno = errcode;
	debug(50, 3) ("file_open: error opening file %s: %s\n", ctrlp->path,
	    xstrerror());
	if (ctrlp->callback)
	    (ctrlp->callback) (ctrlp->callback_data, DISK_ERROR, errcode);
	xfree(ctrlp->path);
	xfree(ctrlp);
	return;
    }
    debug(6, 5) ("file_open: FD %d\n", fd);
    commSetCloseOnExec(fd);
    fd_open(fd, FD_FILE, ctrlp->path);
    if (ctrlp->callback)
	(ctrlp->callback) (ctrlp->callback_data, fd, errcode);
    xfree(ctrlp->path);
    xfree(ctrlp);
}

/* close a disk file. */
void
file_close(int fd)
{
    fde *F = &fd_table[fd];
#if USE_ASYNC_IO
    if (fd < 0) {
	debug(6, 0) ("file_close: FD less than zero: %d\n", fd);
	return;
    }
#else
    assert(fd >= 0);
#endif
    assert(F->open);
    if (F->flags.write_daemon) {
	F->flags.close_request = 1;
	debug(6, 2) ("file_close: FD %d, delaying close\n", fd);
	return;
    }
#if USE_ASYNC_IO
    aioClose(fd);
#else
#if CALL_FSYNC_BEFORE_CLOSE
    fsync(fd);
#endif
    close(fd);
#endif
    debug(6, F->flags.close_request ? 2 : 5)
	("file_close: FD %d, really closing\n", fd);
    fd_close(fd);
}


/* write handler */
static void
diskHandleWrite(int fd, void *notused)
{
    int len = 0;
    disk_ctrl_t *ctrlp;
    dwrite_q *q = NULL;
    dwrite_q *wq = NULL;
    fde *F = &fd_table[fd];
    struct _fde_disk *fdd = &F->disk;
    if (!fdd->write_q)
	return;
    debug(6, 3) ("diskHandleWrite: FD %d\n", fd);
    /* We need to combine subsequent write requests after the first */
    /* But only if we don't need to seek() in between them, ugh! */
    /* XXX This currently ignores any seeks (file_offset) */
    if (fdd->write_q->next != NULL && fdd->write_q->next->next != NULL) {
	len = 0;
	for (q = fdd->write_q->next; q != NULL; q = q->next)
	    len += q->len - q->buf_offset;
	wq = xcalloc(1, sizeof(dwrite_q));
	wq->buf = xmalloc(len);
	wq->len = 0;
	wq->buf_offset = 0;
	wq->next = NULL;
	wq->free_func = xfree;
	do {
	    q = fdd->write_q->next;
	    len = q->len - q->buf_offset;
	    xmemcpy(wq->buf + wq->len, q->buf + q->buf_offset, len);
	    wq->len += len;
	    fdd->write_q->next = q->next;
	    if (q->free_func)
		(q->free_func) (q->buf);
	    safe_free(q);
	} while (fdd->write_q->next != NULL);
	fdd->write_q_tail = wq;
	fdd->write_q->next = wq;
    }
    ctrlp = xcalloc(1, sizeof(disk_ctrl_t));
    ctrlp->fd = fd;
#if USE_ASYNC_IO
    ctrlp->data = fdd->write_q;
#endif
    assert(fdd->write_q != NULL);
    assert(fdd->write_q->len > fdd->write_q->buf_offset);
#if USE_ASYNC_IO
    aioWrite(fd,
	-1,			/* seek offset, -1 == append */
	fdd->write_q->buf + fdd->write_q->buf_offset,
	fdd->write_q->len - fdd->write_q->buf_offset,
	diskHandleWriteComplete,
	ctrlp);
#else
    debug(6, 3) ("diskHandleWrite: FD %d writing %d bytes\n",
	fd, (int) (fdd->write_q->len - fdd->write_q->buf_offset));
    len = write(fd,
	fdd->write_q->buf + fdd->write_q->buf_offset,
	fdd->write_q->len - fdd->write_q->buf_offset);
    diskHandleWriteComplete(ctrlp, len, errno);
#endif
}

static void
diskHandleWriteComplete(void *data, int len, int errcode)
{
    disk_ctrl_t *ctrlp = data;
    int fd = ctrlp->fd;
    fde *F = &fd_table[fd];
    struct _fde_disk *fdd = &F->disk;
    dwrite_q *q = fdd->write_q;
    int status = DISK_OK;
    int do_callback;
    int do_close;
    errno = errcode;
    debug(6, 3) ("diskHandleWriteComplete: FD %d len = %d\n", fd, len);
#if USE_ASYNC_IO
/*
 * From:    "Michael O'Reilly" <michael@metal.iinet.net.au>
 * Date:    24 Feb 1998 15:12:06 +0800
 *
 * A small patch to improve the AIO sanity. the patch below makes sure
 * the write request really does match the data passed back from the
 * async IO call.  note that I haven't actually rebooted with this
 * patch yet, so 'provisional' is an understatement.
 */
    if (q && q != ctrlp->data) {
	dwrite_q *p = ctrlp->data;
	debug(50, 0) ("KARMA: q != data (%p, %p)\n", q, p);
	debug(50, 0) ("KARMA: (%d, %d, %d FD %d)\n",
	    q->buf_offset, q->len, len, fd);
	debug(50, 0) ("KARMA: desc %s, type %d, open %d, flags 0x%x\n",
	    F->desc, F->type, F->open, F->flags);
	debug(50, 0) ("KARMA: (%d, %d)\n", p->buf_offset, p->len);
	len = -1;
	errcode = EFAULT;
    }
#endif
    safe_free(data);
    if (q == NULL)		/* Someone aborted then write completed */
	return;

    if (len == -2 && errcode == -2) {	/* Write cancelled - cleanup */
	do {
	    fdd->write_q = q->next;
	    if (q->free_func)
		(q->free_func) (q->buf);
	    safe_free(q);
	} while ((q = fdd->write_q));
	return;
    }
    fd_bytes(fd, len, FD_WRITE);
    if (len < 0) {
	if (!ignoreErrno(errno)) {
	    status = errno == ENOSPC ? DISK_NO_SPACE_LEFT : DISK_ERROR;
	    debug(50, 1) ("diskHandleWrite: FD %d: disk write error: %s\n",
		fd, xstrerror());
	    /*
	     * If there is no write callback, then this file is
	     * most likely something important like a log file, or
	     * an interprocess pipe.  Its not a swapfile.  We feel
	     * that a write failure on a log file is rather important,
	     * and Squid doesn't otherwise deal with this condition.
	     * So to get the administrators attention, we exit with
	     * a fatal message.
	     */
	    if (fdd->wrt_handle == NULL)
		fatal("Write failure -- check your disk space and cache.log");
	    /*
	     * If there is a write failure, then we notify the
	     * upper layer via the callback, at the end of this
	     * function.  Meanwhile, flush all pending buffers
	     * here.  Let the upper layer decide how to handle the
	     * failure.  This will prevent experiencing multiple,
	     * repeated write failures for the same FD because of
	     * the queued data.
	     */
	    do {
		fdd->write_q = q->next;
		if (q->free_func)
		    (q->free_func) (q->buf);
		safe_free(q);
	    } while ((q = fdd->write_q));
	}
	len = 0;
    }
    if (q != NULL) {
	/* q might become NULL from write failure above */
	q->buf_offset += len;
	if (q->buf_offset > q->len)
	    debug(50, 1) ("diskHandleWriteComplete: q->buf_offset > q->len (%p,%d, %d, %d FD %d)\n",
		q, (int) q->buf_offset, q->len, len, fd);
	assert(q->buf_offset <= q->len);
	if (q->buf_offset == q->len) {
	    /* complete write */
	    fdd->write_q = q->next;
	    if (q->free_func)
		(q->free_func) (q->buf);
	    safe_free(q);
	}
    }
    if (fdd->write_q == NULL) {
	/* no more data */
	fdd->write_q_tail = NULL;
	F->flags.write_daemon = 0;
    } else {
	/* another block is queued */
	cbdataLock(fdd->wrt_handle_data);
	commSetSelect(fd, COMM_SELECT_WRITE, diskHandleWrite, NULL, 0);
	F->flags.write_daemon = 1;
    }
    do_close = F->flags.close_request;
    if (fdd->wrt_handle) {
	if (fdd->wrt_handle_data == NULL)
	    do_callback = 1;
	else if (cbdataValid(fdd->wrt_handle_data))
	    do_callback = 1;
	else
	    do_callback = 0;
	if (fdd->wrt_handle_data != NULL)
	    cbdataUnlock(fdd->wrt_handle_data);
	if (do_callback) {
	    fdd->wrt_handle(fd, status, len, fdd->wrt_handle_data);
	    /*
	     * NOTE, this callback can close the FD, so we must
	     * not touch 'F', 'fdd', etc. after this.
	     */
	    return;
	}
    }
    if (do_close)
	file_close(fd);
}


/* write block to a file */
/* write back queue. Only one writer at a time. */
/* call a handle when writing is complete. */
void
file_write(int fd,
    off_t file_offset,
    void *ptr_to_buf,
    int len,
    DWCB handle,
    void *handle_data,
    FREE * free_func)
{
    dwrite_q *wq = NULL;
    fde *F = &fd_table[fd];
    assert(fd >= 0);
    assert(F->open);
    /* if we got here. Caller is eligible to write. */
    wq = xcalloc(1, sizeof(dwrite_q));
    wq->file_offset = file_offset;
    wq->buf = ptr_to_buf;
    wq->len = len;
    wq->buf_offset = 0;
    wq->next = NULL;
    wq->free_func = free_func;
    F->disk.wrt_handle = handle;
    F->disk.wrt_handle_data = handle_data;
    /* add to queue */
    if (F->disk.write_q == NULL) {
	/* empty queue */
	F->disk.write_q = F->disk.write_q_tail = wq;
    } else {
	F->disk.write_q_tail->next = wq;
	F->disk.write_q_tail = wq;
    }
    if (!F->flags.write_daemon) {
	cbdataLock(F->disk.wrt_handle_data);
#if USE_ASYNC_IO
	diskHandleWrite(fd, NULL);
#else
	commSetSelect(fd, COMM_SELECT_WRITE, diskHandleWrite, NULL, 0);
#endif
	F->flags.write_daemon = 1;
    }
}

/* a wrapper around file_write to allow for MemBuf to be file_written in a snap */
void
file_write_mbuf(int fd, off_t off, MemBuf mb, DWCB * handler, void *handler_data)
{
    file_write(fd, off, mb.buf, mb.size, handler, handler_data, memBufFreeFunc(&mb));
}

/* Read from FD */
static void
diskHandleRead(int fd, void *data)
{
    dread_ctrl *ctrl_dat = data;
#if !USE_ASYNC_IO
    fde *F = &fd_table[fd];
    int len;
#endif
    disk_ctrl_t *ctrlp = xcalloc(1, sizeof(disk_ctrl_t));
    ctrlp->fd = fd;
    ctrlp->data = ctrl_dat;
#if USE_ASYNC_IO
    aioRead(fd,
	ctrl_dat->offset,
	ctrl_dat->buf,
	ctrl_dat->req_len,
	diskHandleReadComplete,
	ctrlp);
#else
    if (F->disk.offset != ctrl_dat->offset) {
	debug(6, 3) ("diskHandleRead: FD %d seeking to offset %d\n",
	    fd, (int) ctrl_dat->offset);
	lseek(fd, ctrl_dat->offset, SEEK_SET);	/* XXX ignore return? */
	F->disk.offset = ctrl_dat->offset;
    }
    len = read(fd, ctrl_dat->buf, ctrl_dat->req_len);
    F->disk.offset += len;
    diskHandleReadComplete(ctrlp, len, errno);
#endif
}

static void
diskHandleReadComplete(void *data, int len, int errcode)
{
    disk_ctrl_t *ctrlp = data;
    dread_ctrl *ctrl_dat = ctrlp->data;
    int fd = ctrlp->fd;
    int rc = DISK_OK;
    errno = errcode;

    xfree(data);

    if (len == -2 && errcode == -2) {	/* Read cancelled - cleanup */
	cbdataUnlock(ctrl_dat->client_data);
	safe_free(ctrl_dat);
	return;
    }
    fd_bytes(fd, len, FD_READ);
    if (len < 0) {
	if (ignoreErrno(errno)) {
	    commSetSelect(fd, COMM_SELECT_READ, diskHandleRead, ctrl_dat, 0);
	    return;
	}
	debug(50, 1) ("diskHandleRead: FD %d: %s\n", fd, xstrerror());
	len = 0;
	rc = DISK_ERROR;
    } else if (len == 0) {
	rc = DISK_EOF;
    }
    if (cbdataValid(ctrl_dat->client_data))
	ctrl_dat->handler(fd, ctrl_dat->buf, len, rc, ctrl_dat->client_data);
    cbdataUnlock(ctrl_dat->client_data);
    safe_free(ctrl_dat);
}


/* start read operation */
/* buffer must be allocated from the caller. 
 * It must have at least req_len space in there. 
 * call handler when a reading is complete. */
int
file_read(int fd, char *buf, int req_len, off_t offset, DRCB * handler, void *client_data)
{
    dread_ctrl *ctrl_dat;
    assert(fd >= 0);
    ctrl_dat = xcalloc(1, sizeof(dread_ctrl));
    ctrl_dat->fd = fd;
    ctrl_dat->offset = offset;
    ctrl_dat->req_len = req_len;
    ctrl_dat->buf = buf;
    ctrl_dat->end_of_file = 0;
    ctrl_dat->handler = handler;
    ctrl_dat->client_data = client_data;
    cbdataLock(client_data);
#if USE_ASYNC_IO
    diskHandleRead(fd, ctrl_dat);
#else
    commSetSelect(fd,
	COMM_SELECT_READ,
	diskHandleRead,
	ctrl_dat,
	0);
#endif
    return DISK_OK;
}

int
diskWriteIsComplete(int fd)
{
    return fd_table[fd].disk.write_q ? 0 : 1;
}
