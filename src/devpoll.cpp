/*
    Copyright (c) 2007-2009 FastMQ Inc.

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "platform.hpp"

#if defined ZS_HAVE_SOLARIS || defined ZS_HAVE_HPUX

#include <sys/devpoll.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "devpoll.hpp"
#include "err.hpp"
#include "config.hpp"

zs::devpoll_t::devpoll_t ()
{
    //  Get limit on open files
    struct rlimit rl;
    int rc = getrlimit (RLIMIT_NOFILE, &rl);
    errno_assert (rc != -1);
    fd_table.resize (rl.rlim_cur);

    for (rlim_t i = 0; i < rl.rlim_cur; i ++)
        fd_table [i].valid = false;

    devpoll_fd = open ("/dev/poll", O_RDWR);
    errno_assert (devpoll_fd != -1);
}

zs::devpoll_t::~devpoll_t ()
{
    close (devpoll_fd);
}

void zs::devpoll_t::devpoll_ctl (fd_t fd_, short events_)
{
    struct pollfd pfd = {fd_, events_, 0};
    ssize_t rc = write (devpoll_fd, &pfd, sizeof pfd);
    zs_assert (rc == sizeof pfd);
}

zs::handle_t zs::devpoll_t::add_fd (fd_t fd_, i_poll_events *reactor_)
{
    assert (!fd_table [fd_].valid);

    fd_table [fd_].events = 0;
    fd_table [fd_].reactor = reactor_;
    fd_table [fd_].valid = true;
    fd_table [fd_].accepted = false;

    devpoll_ctl (fd_, 0);
    pending_list.push_back (fd_);

    //  Increase the load metric of the thread.
    load.add (1);

    handle_t handle;
    handle.fd = fd_;
    return handle;
}

void zs::devpoll_t::rm_fd (handle_t handle_)
{
    assert (fd_table [handle_.fd].valid);

    devpoll_ctl (handle_.fd, POLLREMOVE);
    fd_table [handle_.fd].valid = false;

    //  Decrease the load metric of the thread.
    load.sub (1);
}

void zs::devpoll_t::set_pollin (handle_t handle_)
{
    fd_t fd = handle_.fd;
    devpoll_ctl (fd, POLLREMOVE);
    fd_table [fd].events |= POLLIN;
    devpoll_ctl (fd, fd_table [fd].events);
}

void zs::devpoll_t::reset_pollin (handle_t handle_)
{
    fd_t fd = handle_.fd;
    devpoll_ctl (fd, POLLREMOVE);
    fd_table [fd].events &= ~((short) POLLIN);
    devpoll_ctl (fd, fd_table [fd].events);
}

void zs::devpoll_t::set_pollout (handle_t handle_)
{
    fd_t fd = handle_.fd;
    devpoll_ctl (fd, POLLREMOVE);
    fd_table [fd].events |= POLLOUT;
    devpoll_ctl (fd, fd_table [fd].events);
}

void zs::devpoll_t::reset_pollout (handle_t handle_)
{
    fd_t fd = handle_.fd;
    devpoll_ctl (fd, POLLREMOVE);
    fd_table [fd].events &= ~((short) POLLOUT);
    devpoll_ctl (fd, fd_table [fd].events);
}

void zs::devpoll_t::add_timer (i_poll_events *events_)
{
     timers.push_back (events_);
}

void zs::devpoll_t::cancel_timer (i_poll_events *events_)
{
    timers_t::iterator it = std::find (timers.begin (), timers.end (), events_);
    if (it != timers.end ())
        timers.erase (it);
}

int zs::devpoll_t::get_load ()
{
    return load.get ();
}

void zs::devpoll_t::start ()
{
    worker.start (worker_routine, this);
}

void zs::devpoll_t::stop ()
{
    stopping = true;
}

void zs::devpoll_t::join ()
{
    worker.stop ();
}

bool zs::devpoll_t::loop ()
{
    //  According to the poll(7d) man page, we can retrieve
    //  no more then (OPEN_MAX - 1) events.
    int nfds = std::min (max_io_events, OPEN_MAX - 1);

    while (!stopping) {

        struct pollfd ev_buf [max_io_events];
        struct dvpoll poll_req;

        for (pending_list_t::size_type i = 0; i < pending_list.size (); i ++)
            fd_table [pending_list [i]].accepted = true;
        pending_list.clear ();

        poll_req.dp_fds = &ev_buf [0];
        poll_req.dp_nfds = nfds;
        poll_req.dp_timeout = timers.empty () ? -1 : max_timer_period;

        //  Wait for events.
        int n = ioctl (devpoll_fd, DP_POLL, &poll_req);
        if (n == -1 && errno == EINTR)
            continue;
        errno_assert (n != -1);

        //  Handle timer.
        if (!n) {

            //  Use local list of timers as timer handlers may fill new timers
            //  into the original array.
            timers_t t;
            std::swap (timers, t);

            //  Trigger all the timers.
            for (timers_t::iterator it = t.begin (); it != t.end (); it ++)
                (*it)->timer_event ();

            continue;
        }

        for (int i = 0; i < n; i ++) {

            fd_entry_t *fd_ptr = &fd_table [ev_buf [i].fd];
            if (!fd_ptr->valid || !fd_ptr->accepted)
                continue;
            if (ev_buf [i].revents & (POLLERR | POLLHUP))
                fd_ptr->reactor->in_event ();
            if (!fd_ptr->valid || !fd_ptr->accepted)
                continue;
            if (ev_buf [i].revents & POLLOUT)
                fd_ptr->reactor->out_event ();
            if (!fd_ptr->valid || !fd_ptr->accepted)
                continue;
            if (ev_buf [i].revents & POLLIN)
                fd_ptr->reactor->in_event ();
        }
    }
}

void zs::devpoll_t::worker_routine (void *arg_)
{
    ((devpoll_t*) arg_)->loop ();
}

#endif