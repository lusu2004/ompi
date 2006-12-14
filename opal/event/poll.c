/*	$OpenBSD: poll.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "opal_config.h"


#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <sys/tree.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "event.h"
#include "evsignal.h"

#ifdef USE_LOG
#include "log.h"
#else
#define LOG_DBG(x)
#define log_error(x)	perror(x)
#endif
#define event_debug(x) 

#include "event.h"
#if OPAL_EVENT_USE_SIGNALS
#include "evsignal.h"
#endif
#include "opal/threads/mutex.h"

extern struct opal_event_list opal_eventqueue;
extern opal_mutex_t opal_event_lock;

#if OPAL_EVENT_USE_SIGNALS
extern volatile sig_atomic_t opal_evsignal_caught;
#endif

struct pollop {
	int event_count;		/* Highest number alloc */
	int fd_count;                   /* Size of idxplus1_by_fd */
	struct pollfd *event_set;
	struct opal_event **event_r_back;
	struct opal_event **event_w_back;
	int *idxplus1_by_fd; /* Index into event_set by fd; we add 1 so
			      * that 0 (which is easy to memset) can mean
			      * "no entry." */
	sigset_t evsigmask;
};

void *poll_init	(void);
int poll_add		(void *, struct opal_event *);
int poll_del		(void *, struct opal_event *);
int poll_recalc		(void *, int);
int poll_dispatch	(void *, struct timeval *);

struct opal_eventop opal_pollops = {
	"poll",
	poll_init,
	poll_add,
	poll_del,
	poll_recalc,
	poll_dispatch
};

void *
poll_init(void)
{
	struct pollop *pollop;

	/* Disable kqueue when this environment variable is set */
	if (getenv("EVENT_NOPOLL"))
		return (NULL);

        if (!(pollop = calloc(1, sizeof(struct pollop))))
		return (NULL);

#if OPAL_EVENT_USE_SIGNALS
	opal_evsignal_init(&pollop->evsigmask);
#endif

	return (pollop);
}

/*
 * Called with the highest fd that we know about.  If it is 0, completely
 * recalculate everything.
 */

int
poll_recalc(void *arg, int max)
{
	struct pollop *pop = arg;

#if OPAL_EVENT_USE_SIGNALS
	return (opal_evsignal_recalc(&pop->evsigmask));
#else
	return (0)
#endif
}

int
poll_dispatch(void *arg, struct timeval *tv)
{
	int res, i, count, fd_count, sec, nfds;
	struct opal_event *ev;
	struct pollop *pop = arg;
	int *idxplus1_by_fd;

	count = pop->event_count;
	fd_count = pop->fd_count;
	idxplus1_by_fd = pop->idxplus1_by_fd;
	memset(idxplus1_by_fd, 0, sizeof(int)*fd_count);
	nfds = 0;

	TAILQ_FOREACH(ev, &opal_eventqueue, ev_next) {
		struct pollfd *pfd = NULL;
		if (nfds + 1 >= count) {
			if (count < 32)
				count = 32;
			else
				count *= 2;

			/* We need more file descriptors */
			pop->event_set = realloc(pop->event_set,
			    count * sizeof(struct pollfd));
			if (pop->event_set == NULL) {
                                log_error("realloc");
				return (-1);
			}
			pop->event_r_back = realloc(pop->event_r_back,
			    count * sizeof(struct event *));
			pop->event_w_back = realloc(pop->event_w_back,
			    count * sizeof(struct event *));
			if (pop->event_r_back == NULL ||
			    pop->event_w_back == NULL) {
				log_error("realloc");
				return (-1);
			}
			pop->event_count = count;
		}
		if (!(ev->ev_events & (OPAL_EV_READ|OPAL_EV_WRITE)))
			continue;
		if (ev->ev_fd >= fd_count) {
			int new_count;
			if (fd_count < 32)
				new_count = 32;
			else
				new_count = fd_count * 2;
			while (new_count <= ev->ev_fd)
				new_count *= 2;
			idxplus1_by_fd = pop->idxplus1_by_fd =
			  realloc(pop->idxplus1_by_fd, new_count*sizeof(int));
			if (idxplus1_by_fd == NULL) {
				log_error("realloc");
				return (-1);
			}
			memset(pop->idxplus1_by_fd + fd_count,
			       0, sizeof(int)*(new_count-fd_count));
			fd_count = pop->fd_count = new_count;
		}
		i = idxplus1_by_fd[ev->ev_fd] - 1;
		if (i >= 0) {
			pfd = &pop->event_set[i];
		} else {
			i = nfds++;
			pfd = &pop->event_set[i];
			pop->event_w_back[i] = pop->event_r_back[i] = NULL;
			pfd->events = 0;
			idxplus1_by_fd[ev->ev_fd] = i + 1;
		}

		if (ev->ev_events & OPAL_EV_WRITE) {
			pfd->fd = ev->ev_fd;
			pfd->events |= POLLOUT;
			pfd->revents = 0;

			pop->event_w_back[i] = ev;
		}
		if (ev->ev_events & OPAL_EV_READ) {
			pfd->fd = ev->ev_fd;
			pfd->events |= POLLIN;
			pfd->revents = 0;

			pop->event_r_back[i] = ev;
		}
	}

#if OPAL_EVENT_USE_SIGNALS
	if (opal_evsignal_deliver(&pop->evsigmask) == -1)
		return (-1);
#endif

	sec = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
        /* release lock while waiting in kernel */
        opal_mutex_unlock(&opal_event_lock);
	res = poll(pop->event_set, nfds, sec);
        opal_mutex_lock(&opal_event_lock);

#if OPAL_EVENT_USE_SIGNALS
	if (opal_evsignal_recalc(&pop->evsigmask) == -1)
		return (-1);
#endif

	if (res == -1) {
		if (errno != EINTR) {
                        log_error("poll");
			return (-1);
		}

#if OPAL_EVENT_USE_SIGNALS
		opal_evsignal_process();
#endif
		return (0);
#if OPAL_EVENT_USE_SIGNALS
	} else if (opal_evsignal_caught) {
		opal_evsignal_process();
#endif
	}

	event_debug(("%s: poll reports %d", __func__, res));

	if (res == 0)
		return (0);

	for (i = 0; i < nfds; i++) {
                int what = pop->event_set[i].revents;
		struct opal_event *r_ev = NULL, *w_ev = NULL;
		
		res = 0;

		/* If the file gets closed notify */
		if (what & POLLHUP)
			what |= POLLIN|POLLOUT;
                if (what & POLLERR) 
                        what |= POLLIN|POLLOUT;
		if (what & POLLIN) {
			res |= OPAL_EV_READ;
			r_ev = pop->event_r_back[i];
		}
		if (what & POLLOUT) {
			res |= OPAL_EV_WRITE;
			w_ev = pop->event_w_back[i];
		}
		if (res == 0)
			continue;

		if (r_ev && (res & r_ev->ev_events)) {
			if (!(r_ev->ev_events & OPAL_EV_PERSIST))
				opal_event_del(r_ev);
			opal_event_active(r_ev, res & r_ev->ev_events, 1);
		}
		if (w_ev && w_ev != r_ev && (res & w_ev->ev_events)) {
			if (!(w_ev->ev_events & OPAL_EV_PERSIST))
				opal_event_del(w_ev);
			opal_event_active(w_ev, res & w_ev->ev_events, 1);
		}
	}

	return (0);
}

int
poll_add(void *arg, struct opal_event *ev)
{
	struct pollop *pop = arg;

#if OPAL_EVENT_USE_SIGNALS
	if (ev->ev_events & OPAL_EV_SIGNAL)
		return (opal_evsignal_add(&pop->evsigmask, ev));
#endif

	return (0);
}

/*
 * Nothing to be done here.
 */

int
poll_del(void *arg, struct opal_event *ev)
{
#if OPAL_EVENT_USE_SIGNALS
	struct pollop *pop = arg;

	if (!(ev->ev_events & OPAL_EV_SIGNAL))
		return (0);

	return (opal_evsignal_del(&pop->evsigmask, ev));
#else
	return 0;
#endif
}
