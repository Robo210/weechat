/*
 * Copyright (c) 2003-2006 by FlashCode <flashcode@flashtux.org>
 * See README for License detail, AUTHORS for developers list.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* irc-dcc.c: Direct Client-to-Client (DCC) communication (files & chat) */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "../common/weechat.h"
#include "irc.h"
#include "../common/log.h"
#include "../common/hotlist.h"
#include "../common/util.h"
#include "../common/weeconfig.h"
#include "../gui/gui.h"


t_irc_dcc *dcc_list = NULL;     /* DCC files & chat list                    */
char *dcc_status_string[] =     /* strings for DCC status                   */
{ N_("Waiting"), N_("Connecting"), N_("Active"), N_("Done"), N_("Failed"),
  N_("Aborted") };


/*
 * dcc_redraw: redraw DCC buffer (and add to hotlist)
 */

void
dcc_redraw (int highlight)
{
    gui_window_redraw_buffer (gui_buffer_get_dcc (gui_current_window));
    if (highlight)
    {
        hotlist_add (highlight, NULL, gui_buffer_get_dcc (gui_current_window));
        gui_status_draw (gui_current_window->buffer, 0);
    }
}

/*
 * dcc_search: search a DCC
 */

t_irc_dcc *
dcc_search (t_irc_server *server, int type, int status, int port)
{
    t_irc_dcc *ptr_dcc;
    
    for (ptr_dcc = dcc_list; ptr_dcc; ptr_dcc = ptr_dcc->next_dcc)
    {
        if ((ptr_dcc->server == server)
            && (ptr_dcc->type == type)
            && (ptr_dcc->status = status)
            && (ptr_dcc->port == port))
            return ptr_dcc;
    }
    
    /* DCC not found */
    return NULL;
}

/*
 * dcc_port_in_use: return 1 if a port is in used (by an active or connecting DCC)
 */

int
dcc_port_in_use (int port)
{
    t_irc_dcc *ptr_dcc;
    
    /* skip any currently used ports */
    for (ptr_dcc = dcc_list; ptr_dcc; ptr_dcc = ptr_dcc->next_dcc)
    {
        if ((ptr_dcc->port == port) && (!DCC_ENDED(ptr_dcc->status)))
            return 1;
    }
    
    /* port not in use */
    return 0;
}

/*
 * dcc_file_is_resumable: check if a file can be used for resuming a download
 */

int
dcc_file_is_resumable (t_irc_dcc *ptr_dcc, char *filename)
{
    struct stat st;
    
    if (!cfg_dcc_auto_resume)
        return 0;
    
    if (access (filename, W_OK) == 0)
    {
        if (stat (filename, &st) != -1)
        {
            if ((unsigned long) st.st_size < ptr_dcc->size)
            {
                ptr_dcc->start_resume = (unsigned long) st.st_size;
                ptr_dcc->pos = st.st_size;
                ptr_dcc->last_check_pos = st.st_size;
                return 1;
            }
        }
    }
    
    /* not resumable */
    return 0;
}

/*
 * dcc_find_filename: find local filename for a DCC
 *                    if type if file/recv, add a suffix (like .1) if needed
 *                    if download is resumable, set "start_resume" to good value
 */

void
dcc_find_filename (t_irc_dcc *ptr_dcc)
{
    char *dir1, *dir2, *filename2;
    
    if (!DCC_IS_FILE(ptr_dcc->type))
        return;
    
    dir1 = weechat_strreplace (cfg_dcc_download_path, "~", getenv ("HOME"));
    if (!dir1)
        return;
    dir2 = weechat_strreplace (dir1, "%h", weechat_home);
    if (!dir2)
    {
        free (dir1);
        return;
    }
    
    ptr_dcc->local_filename = (char *) malloc (strlen (dir2) +
                                               strlen (ptr_dcc->nick) +
                                               strlen (ptr_dcc->filename) + 4);
    if (!ptr_dcc->local_filename)
        return;
    
    strcpy (ptr_dcc->local_filename, dir2);
    if (ptr_dcc->local_filename[strlen (ptr_dcc->local_filename) - 1] != DIR_SEPARATOR_CHAR)
        strcat (ptr_dcc->local_filename, DIR_SEPARATOR);
    strcat (ptr_dcc->local_filename, ptr_dcc->nick);
    strcat (ptr_dcc->local_filename, ".");
    strcat (ptr_dcc->local_filename, ptr_dcc->filename);
    
    if (dir1)
        free (dir1);
    if (dir2 )
        free (dir2);
    
    /* file already exists? */
    if (access (ptr_dcc->local_filename, F_OK) == 0)
    {
        if (dcc_file_is_resumable (ptr_dcc, ptr_dcc->local_filename))
            return;
        
        /* if auto rename is not set, then abort DCC */
        if (!cfg_dcc_auto_rename)
        {
            dcc_close (ptr_dcc, DCC_FAILED);
            dcc_redraw (HOTLIST_MSG);
            return;
        }
        
        filename2 = (char *) malloc (strlen (ptr_dcc->local_filename) + 16);
        if (!filename2)
        {
            dcc_close (ptr_dcc, DCC_FAILED);
            dcc_redraw (HOTLIST_MSG);
            return;
        }
        ptr_dcc->filename_suffix = 0;
        do
        {
            ptr_dcc->filename_suffix++;
            sprintf (filename2, "%s.%d",
                     ptr_dcc->local_filename,
                     ptr_dcc->filename_suffix);
            if (access (filename2, F_OK) == 0)
            {
                if (dcc_file_is_resumable (ptr_dcc, filename2))
                    break;
            }
            else
                break;
        }
        while (1);
        
        free (ptr_dcc->local_filename);
        ptr_dcc->local_filename = strdup (filename2);
        free (filename2);
    }
}

/*
 * dcc_calculate_speed: calculate DCC speed (for files only)
 */

void
dcc_calculate_speed (t_irc_dcc *ptr_dcc, int ended)
{
    time_t local_time, elapsed;
    unsigned long bytes_per_sec_total;
    
    local_time = time (NULL);
    if (ended || local_time > ptr_dcc->last_check_time)
    {
        if (ended)
        {
            /* calculate bytes per second (global) */
            elapsed = local_time - ptr_dcc->start_transfer;
            if (elapsed == 0)
                elapsed = 1;
            ptr_dcc->bytes_per_sec = (ptr_dcc->pos - ptr_dcc->start_resume) / elapsed;
            ptr_dcc->eta = 0;
        }
        else
        {
            /* calculate ETA */
            elapsed = local_time - ptr_dcc->start_transfer;
            if (elapsed == 0)
                elapsed = 1;
            bytes_per_sec_total = (ptr_dcc->pos - ptr_dcc->start_resume) / elapsed;
            if (bytes_per_sec_total == 0)
                bytes_per_sec_total = 1;
            ptr_dcc->eta = (ptr_dcc->size - ptr_dcc->pos) / bytes_per_sec_total;
            
            /* calculate bytes per second (since last check time) */
            elapsed = local_time - ptr_dcc->last_check_time;
            if (elapsed == 0)
                elapsed = 1;
            ptr_dcc->bytes_per_sec = (ptr_dcc->pos - ptr_dcc->last_check_pos) / elapsed;
        }
        ptr_dcc->last_check_time = local_time;
        ptr_dcc->last_check_pos = ptr_dcc->pos;
    }
}

/*
 * dcc_connect: connect to another host
 */

int
dcc_connect (t_irc_dcc *ptr_dcc)
{
    struct sockaddr_in addr;
    struct hostent *hostent;
    char *ip4;
    
    if (ptr_dcc->type == DCC_CHAT_SEND)
        ptr_dcc->status = DCC_WAITING;
    else
        ptr_dcc->status = DCC_CONNECTING;
    
    if (ptr_dcc->sock < 0)
    {
        ptr_dcc->sock = socket (AF_INET, SOCK_STREAM, 0);
        if (ptr_dcc->sock < 0)
            return 0;
    }

    /* for sending (chat or file), listen to socket for a connection */
    if (DCC_IS_SEND(ptr_dcc->type))
      {
        if (fcntl (ptr_dcc->sock, F_SETFL, O_NONBLOCK) == -1)
	  return 0;	
        if (listen (ptr_dcc->sock, 1) == -1)
            return 0;
        if (fcntl (ptr_dcc->sock, F_SETFL, 0) == -1)
            return 0;
    }
    
    /* for receiving (chat or file), connect to listening host */
    if (DCC_IS_RECV(ptr_dcc->type))
    {
        if (fcntl (ptr_dcc->sock, F_SETFL, O_NONBLOCK) == -1)
            return 0;      
        if (cfg_proxy_use)
	{
            memset (&addr, 0, sizeof (addr));
            addr.sin_addr.s_addr = htonl (ptr_dcc->addr);
            ip4 = inet_ntoa(addr.sin_addr);

            memset (&addr, 0, sizeof (addr));
            addr.sin_port = htons (cfg_proxy_port);
            addr.sin_family = AF_INET;
            if ((hostent = gethostbyname (cfg_proxy_address)) == NULL)
                return 0;
            memcpy(&(addr.sin_addr),*(hostent->h_addr_list), sizeof(struct in_addr));
            connect (ptr_dcc->sock, (struct sockaddr *) &addr, sizeof (addr));
            if (pass_proxy(ptr_dcc->sock, ip4, ptr_dcc->port, ptr_dcc->server->username) == -1)
                return 0;
	}
        else
	{
            memset (&addr, 0, sizeof (addr));
            addr.sin_port = htons (ptr_dcc->port);
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl (ptr_dcc->addr);
            connect (ptr_dcc->sock, (struct sockaddr *) &addr, sizeof (addr));
	}
    }
    
    return 1;
}

/*
 * dcc_free: free DCC struct and remove it from list
 */

void
dcc_free (t_irc_dcc *ptr_dcc)
{
    t_irc_dcc *new_dcc_list;
    
    if (!ptr_dcc)
        return;
    
    /* DCC CHAT with channel => remove channel
       (to prevent channel from becoming standard pv) */
    if (ptr_dcc->channel)
    {
        /* check if channel is used for another active DCC CHAT */
        if (!ptr_dcc->channel->dcc_chat
            || (DCC_ENDED(((t_irc_dcc *)(ptr_dcc->channel->dcc_chat))->status)))
        {
            gui_buffer_free (ptr_dcc->channel->buffer, 1);
            if (ptr_dcc->channel)
                channel_free (ptr_dcc->server, ptr_dcc->channel);
        }
    }

    /* remove DCC from list */
    if (ptr_dcc->prev_dcc)
    {
        (ptr_dcc->prev_dcc)->next_dcc = ptr_dcc->next_dcc;
        new_dcc_list = dcc_list;
    }
    else
        new_dcc_list = ptr_dcc->next_dcc;
    if (ptr_dcc->next_dcc)
        (ptr_dcc->next_dcc)->prev_dcc = ptr_dcc->prev_dcc;

    /* free data */
    if (ptr_dcc->nick)
        free (ptr_dcc->nick);
    if (ptr_dcc->unterminated_message)
        free (ptr_dcc->unterminated_message);
    if (ptr_dcc->filename)
        free (ptr_dcc->filename);
    
    free (ptr_dcc);
    dcc_list = new_dcc_list;
}

/*
 * dcc_file_child_kill: kill child process and close pipe
 */

void
dcc_file_child_kill (t_irc_dcc *ptr_dcc)
{
    /* kill process */
    if (ptr_dcc->child_pid > 0)
    {
        kill (ptr_dcc->child_pid, SIGKILL);
        waitpid (ptr_dcc->child_pid, NULL, 0);
        ptr_dcc->child_pid = 0;
    }
    
    /* close pipe used with child */
    if (ptr_dcc->child_read != -1)
    {
        close (ptr_dcc->child_read);
        ptr_dcc->child_read = -1;
    }
    if (ptr_dcc->child_write != -1)
    {
        close (ptr_dcc->child_write);
        ptr_dcc->child_write = -1;
    }
}

/*
 * dcc_close: close a DCC connection
 */

void
dcc_close (t_irc_dcc *ptr_dcc, int status)
{
    t_gui_buffer *ptr_buffer;
    struct stat st;
    
    ptr_dcc->status = status;
    
    if ((status == DCC_DONE) || (status == DCC_ABORTED) || (status == DCC_FAILED))
    {
        if (DCC_IS_FILE(ptr_dcc->type))
        {
            irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                PREFIX_INFO);
            gui_printf (ptr_dcc->server->buffer,
                        _("DCC: file %s%s%s"),
                        GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                        ptr_dcc->filename,
                        GUI_COLOR(COLOR_WIN_CHAT));
            if (ptr_dcc->local_filename)
                gui_printf (ptr_dcc->server->buffer,
                            _(" (local filename: %s%s%s)"),
                            GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                            ptr_dcc->local_filename,
                            GUI_COLOR(COLOR_WIN_CHAT));
            if (ptr_dcc->type == DCC_FILE_SEND)
                gui_printf (ptr_dcc->server->buffer, _(" sent to "));
            else
                gui_printf (ptr_dcc->server->buffer, _(" received from "));
            gui_printf (ptr_dcc->server->buffer, "%s%s%s: %s\n",
                        GUI_COLOR(COLOR_WIN_CHAT_NICK),
                        ptr_dcc->nick,
                        GUI_COLOR(COLOR_WIN_CHAT),
                        (status == DCC_DONE) ? _("OK") : _("FAILED"));
            dcc_file_child_kill (ptr_dcc);
        }
    }
    if (status == DCC_ABORTED)
    {
        if (DCC_IS_CHAT(ptr_dcc->type))
        {
            if (ptr_dcc->channel)
                ptr_buffer = ptr_dcc->channel->buffer;
            else
                ptr_buffer = ptr_dcc->server->buffer;
            irc_display_prefix (ptr_dcc->server, ptr_buffer, PREFIX_INFO);
            gui_printf (ptr_buffer,
                        _("DCC chat closed with %s%s %s(%s%d.%d.%d.%d%s)\n"),
                        GUI_COLOR(COLOR_WIN_CHAT_NICK),
                        ptr_dcc->nick,
                        GUI_COLOR(COLOR_WIN_CHAT_DARK),
                        GUI_COLOR(COLOR_WIN_CHAT_HOST),
                        ptr_dcc->addr >> 24,
                        (ptr_dcc->addr >> 16) & 0xff,
                        (ptr_dcc->addr >> 8) & 0xff,
                        ptr_dcc->addr & 0xff,
                        GUI_COLOR(COLOR_WIN_CHAT_DARK));
        }
    }
    
    /* remove empty file if received file failed and nothing was transfered */
    if (((status == DCC_FAILED) || (status == DCC_ABORTED))
        && DCC_IS_FILE(ptr_dcc->type)
        && DCC_IS_RECV(ptr_dcc->type)
        && ptr_dcc->local_filename
        && ptr_dcc->pos == 0)
    {
        /* erase file only if really empty on disk */
        if (stat (ptr_dcc->local_filename, &st) != -1)
        {
            if ((unsigned long) st.st_size == 0)
                unlink (ptr_dcc->local_filename);
        }
    }
    
    if (DCC_IS_FILE(ptr_dcc->type))
        dcc_calculate_speed (ptr_dcc, 1);
    
    if (ptr_dcc->sock >= 0)
    {
        close (ptr_dcc->sock);
        ptr_dcc->sock = -1;
    }
    if (ptr_dcc->file >= 0)
    {
        close (ptr_dcc->file);
        ptr_dcc->file = -1;
    }
}

/*
 * dcc_channel_for_chat: create channel for DCC chat
 */

void
dcc_channel_for_chat (t_irc_dcc *ptr_dcc)
{
    if (!channel_create_dcc (ptr_dcc))
    {
        irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                            PREFIX_ERROR);
        gui_printf (ptr_dcc->server->buffer,
                    _("%s can't associate DCC chat with private buffer "
                    "(maybe private buffer has already DCC CHAT?)\n"),
                    WEECHAT_ERROR);
        dcc_close (ptr_dcc, DCC_FAILED);
        dcc_redraw (HOTLIST_MSG);
        return;
    }
    
    irc_display_prefix (ptr_dcc->server, ptr_dcc->channel->buffer,
                        PREFIX_INFO);
    gui_printf_type (ptr_dcc->channel->buffer, MSG_TYPE_MSG,
                     _("Connected to %s%s %s(%s%d.%d.%d.%d%s)%s via DCC chat\n"),
                     GUI_COLOR(COLOR_WIN_CHAT_NICK),
                     ptr_dcc->nick,
                     GUI_COLOR(COLOR_WIN_CHAT_DARK),
                     GUI_COLOR(COLOR_WIN_CHAT_HOST),
                     ptr_dcc->addr >> 24,
                     (ptr_dcc->addr >> 16) & 0xff,
                     (ptr_dcc->addr >> 8) & 0xff,
                     ptr_dcc->addr & 0xff,
                     GUI_COLOR(COLOR_WIN_CHAT_DARK),
                     GUI_COLOR(COLOR_WIN_CHAT));
}

/*
 * dcc_chat_remove_channel: remove a buffer for DCC chat
 */

void
dcc_chat_remove_channel (t_irc_channel *channel)
{
    t_irc_dcc *ptr_dcc;
    
    if (!channel)
        return;

    for (ptr_dcc = dcc_list; ptr_dcc; ptr_dcc = ptr_dcc->next_dcc)
    {
        if (ptr_dcc->channel == channel)
            ptr_dcc->channel = NULL;
    }
}

/*
 * dcc_recv_connect_init: connect to sender and init file or chat
 */

void
dcc_recv_connect_init (t_irc_dcc *ptr_dcc)
{
    if (!dcc_connect (ptr_dcc))
    {
        dcc_close (ptr_dcc, DCC_FAILED);
        dcc_redraw (HOTLIST_MSG);
    }
    else
    {
        ptr_dcc->status = DCC_ACTIVE;
        
        /* DCC file => look for local filename and open it in writing mode */
        if (DCC_IS_FILE(ptr_dcc->type))
        {
            ptr_dcc->start_transfer = time (NULL);
            ptr_dcc->last_check_time = time (NULL);
            dcc_file_recv_fork (ptr_dcc);
        }
        else
        {
            /* DCC CHAT => associate DCC with channel */
            dcc_channel_for_chat (ptr_dcc);
        }
    }
    dcc_redraw (HOTLIST_MSG);
}

/*
 * dcc_accept: accepts a DCC file or chat request
 */

void
dcc_accept (t_irc_dcc *ptr_dcc)
{
    if (DCC_IS_FILE(ptr_dcc->type) && (ptr_dcc->start_resume > 0))
    {
        ptr_dcc->status = DCC_CONNECTING;
        server_sendf (ptr_dcc->server,
                      (strchr (ptr_dcc->filename, ' ')) ?
                          "PRIVMSG %s :\01DCC RESUME \"%s\" %d %u\01\r\n" :
                          "PRIVMSG %s :\01DCC RESUME %s %d %u\01\r\n",
                      ptr_dcc->nick, ptr_dcc->filename,
                      ptr_dcc->port, ptr_dcc->start_resume);
        dcc_redraw (HOTLIST_MSG);
    }
    else
        dcc_recv_connect_init (ptr_dcc);
}

/*
 * dcc_accept_resume: accepts a resume and inform the receiver
 */

void
dcc_accept_resume (t_irc_server *server, char *filename, int port,
                   unsigned long pos_start)
{
    t_irc_dcc *ptr_dcc;
    
    ptr_dcc = dcc_search (server, DCC_FILE_SEND, DCC_CONNECTING, port);
    if (ptr_dcc)
    {
        ptr_dcc->pos = pos_start;
        ptr_dcc->ack = pos_start;
        ptr_dcc->start_resume = pos_start;
        ptr_dcc->last_check_pos = pos_start;
        server_sendf (ptr_dcc->server,
                      (strchr (ptr_dcc->filename, ' ')) ?
                          "PRIVMSG %s :\01DCC ACCEPT \"%s\" %d %u\01\r\n" :
                          "PRIVMSG %s :\01DCC ACCEPT %s %d %u\01\r\n",
                      ptr_dcc->nick, ptr_dcc->filename,
                      ptr_dcc->port, ptr_dcc->start_resume);
        
        irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                            PREFIX_INFO);
        gui_printf (ptr_dcc->server->buffer,
                    _("DCC: file %s%s%s resumed at position %u\n"),
                    GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                    ptr_dcc->filename,
                    GUI_COLOR(COLOR_WIN_CHAT),
                    ptr_dcc->start_resume);
        dcc_redraw (HOTLIST_MSG);
    }
    else
        gui_printf (server->buffer,
                    _("%s can't resume file \"%s\" (port: %d, start position: %u): DCC not found or ended\n"),
                    WEECHAT_ERROR, filename, port, pos_start);
}

/*
 * dcc_start_resume: called when "DCC ACCEPT" is received (resume accepted by sender)
 */

void
dcc_start_resume (t_irc_server *server, char *filename, int port,
                  unsigned long pos_start)
{
    t_irc_dcc *ptr_dcc;
    
    ptr_dcc = dcc_search (server, DCC_FILE_RECV, DCC_CONNECTING, port);
    if (ptr_dcc)
    {
        ptr_dcc->pos = pos_start;
        ptr_dcc->ack = pos_start;
        ptr_dcc->start_resume = pos_start;
        ptr_dcc->last_check_pos = pos_start;
        dcc_recv_connect_init (ptr_dcc);
    }
    else
        gui_printf (server->buffer,
                    _("%s can't resume file \"%s\" (port: %d, start position: %u): DCC not found or ended\n"),
                    WEECHAT_ERROR, filename, port, pos_start);
}

/*
 * dcc_alloc: allocate a new DCC file
 */

t_irc_dcc *
dcc_alloc ()
{
    t_irc_dcc *new_dcc;
    
    /* create new DCC struct */
    if ((new_dcc = (t_irc_dcc *) malloc (sizeof (t_irc_dcc))) == NULL)
        return NULL;
    
    /* default values */
    new_dcc->server = NULL;
    new_dcc->channel = NULL;
    new_dcc->type = 0;
    new_dcc->status = 0;
    new_dcc->start_time = 0;
    new_dcc->start_transfer = 0;
    new_dcc->addr = 0;
    new_dcc->port = 0;
    new_dcc->nick = NULL;
    new_dcc->sock = -1;
    new_dcc->child_pid = 0;
    new_dcc->child_read = -1;
    new_dcc->child_write = -1;
    new_dcc->unterminated_message = NULL;
    new_dcc->fast_send = cfg_dcc_fast_send;
    new_dcc->file = -1;
    new_dcc->filename = NULL;
    new_dcc->local_filename = NULL;
    new_dcc->filename_suffix = -1;
    new_dcc->blocksize = cfg_dcc_blocksize;
    new_dcc->size = 0;
    new_dcc->pos = 0;
    new_dcc->ack = 0;
    new_dcc->start_resume = 0;
    new_dcc->last_check_time = 0;
    new_dcc->last_check_pos = 0;
    new_dcc->last_activity = 0;
    new_dcc->bytes_per_sec = 0;
    new_dcc->eta = 0;
    
    new_dcc->prev_dcc = NULL;
    new_dcc->next_dcc = dcc_list;
    if (dcc_list)
        dcc_list->prev_dcc = new_dcc;
    dcc_list = new_dcc;
    
    return new_dcc;
}

/*
 * dcc_add: add a DCC file to queue
 */

t_irc_dcc *
dcc_add (t_irc_server *server, int type, unsigned long addr, int port, char *nick,
         int sock, char *filename, char *local_filename, unsigned long size)
{
    t_irc_dcc *new_dcc;
    
    new_dcc = dcc_alloc ();
    if (!new_dcc)
    {
        irc_display_prefix (server, server->buffer, PREFIX_ERROR);
        gui_printf (server->buffer,
                    _("%s not enough memory for new DCC\n"),
                    WEECHAT_ERROR);
        return NULL;
    }
    
    /* initialize new DCC */
    new_dcc->server = server;
    new_dcc->channel = NULL;
    new_dcc->type = type;
    new_dcc->status = DCC_WAITING;
    new_dcc->start_time = time (NULL);
    new_dcc->start_transfer = time (NULL);
    new_dcc->addr = addr;
    new_dcc->port = port;
    new_dcc->nick = strdup (nick);
    new_dcc->sock = sock;
    new_dcc->unterminated_message = NULL;
    new_dcc->file = -1;
    if (DCC_IS_CHAT(type))
        new_dcc->filename = strdup (_("DCC chat"));
    else
        new_dcc->filename = (filename) ? strdup (filename) : NULL;
    new_dcc->local_filename = NULL;
    new_dcc->filename_suffix = -1;
    new_dcc->size = size;
    new_dcc->pos = 0;
    new_dcc->ack = 0;
    new_dcc->start_resume = 0;
    new_dcc->last_check_time = time (NULL);
    new_dcc->last_check_pos = 0;
    new_dcc->last_activity = time (NULL);
    new_dcc->bytes_per_sec = 0;
    new_dcc->eta = 0;
    if (local_filename)
        new_dcc->local_filename = strdup (local_filename);
    else
        dcc_find_filename (new_dcc);
    
    gui_current_window->dcc_first = NULL;
    gui_current_window->dcc_selected = NULL;
    
    /* write info message on server buffer */
    if (type == DCC_FILE_RECV)
    {
        irc_display_prefix (server, server->buffer, PREFIX_INFO);
        gui_printf (server->buffer,
                    _("Incoming DCC file from %s%s%s (%s%d.%d.%d.%d%s)%s: %s%s%s, %s%lu%s bytes\n"),
                    GUI_COLOR(COLOR_WIN_CHAT_NICK),
                    nick,
                    GUI_COLOR(COLOR_WIN_CHAT_DARK),
                    GUI_COLOR(COLOR_WIN_CHAT_HOST),
                    addr >> 24,
                    (addr >> 16) & 0xff,
                    (addr >> 8) & 0xff,
                    addr & 0xff,
                    GUI_COLOR(COLOR_WIN_CHAT_DARK),
                    GUI_COLOR(COLOR_WIN_CHAT),
                    GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                    filename,
                    GUI_COLOR(COLOR_WIN_CHAT),
                    GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                    size,
                    GUI_COLOR(COLOR_WIN_CHAT));
        dcc_redraw (HOTLIST_MSG);
    }
    if (type == DCC_FILE_SEND)
    {
        irc_display_prefix (server, server->buffer, PREFIX_INFO);
        gui_printf (server->buffer,
                    _("Sending DCC file to %s%s%s: %s%s%s "
                      "(local filename: %s%s%s), %s%lu%s bytes\n"),
                    GUI_COLOR(COLOR_WIN_CHAT_NICK),
                    nick,
                    GUI_COLOR(COLOR_WIN_CHAT),
                    GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                    filename,
                    GUI_COLOR(COLOR_WIN_CHAT),
                    GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                    local_filename,
                    GUI_COLOR(COLOR_WIN_CHAT),
                    GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                    size,
                    GUI_COLOR(COLOR_WIN_CHAT));
        dcc_redraw (HOTLIST_MSG);
    }
    if (type == DCC_CHAT_RECV)
    {
        irc_display_prefix (server, server->buffer, PREFIX_INFO);
        gui_printf (server->buffer,
                    _("Incoming DCC chat request from %s%s%s "
                      "(%s%d.%d.%d.%d%s)\n"),
                    GUI_COLOR(COLOR_WIN_CHAT_NICK),
                    nick,
                    GUI_COLOR(COLOR_WIN_CHAT_DARK),
                    GUI_COLOR(COLOR_WIN_CHAT_HOST),
                    addr >> 24,
                    (addr >> 16) & 0xff,
                    (addr >> 8) & 0xff,
                    addr & 0xff,
                    GUI_COLOR(COLOR_WIN_CHAT_DARK));
        dcc_redraw (HOTLIST_MSG);
    }
    if (type == DCC_CHAT_SEND)
    {
        irc_display_prefix (server, server->buffer, PREFIX_INFO);
        gui_printf (server->buffer,
                    _("Sending DCC chat request to %s%s\n"),
                    GUI_COLOR(COLOR_WIN_CHAT_NICK),
                    nick);
        dcc_redraw (HOTLIST_MSG);
    }
    
    if (DCC_IS_FILE(type) && (!new_dcc->local_filename))
    {
        dcc_close (new_dcc, DCC_FAILED);
        dcc_redraw (HOTLIST_MSG);
        return NULL;
    }
    
    if (DCC_IS_FILE(type) && (new_dcc->start_resume > 0))
    {
        irc_display_prefix (new_dcc->server, new_dcc->server->buffer,
                            PREFIX_INFO);
        gui_printf (new_dcc->server->buffer,
                    _("DCC: file %s%s%s (local filename: %s%s%s) "
                      "will be resumed at position %u\n"),
                    GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                    new_dcc->filename,
                    GUI_COLOR(COLOR_WIN_CHAT),
                    GUI_COLOR(COLOR_WIN_CHAT_CHANNEL),
                    new_dcc->local_filename,
                    GUI_COLOR(COLOR_WIN_CHAT),
                    new_dcc->start_resume);
        dcc_redraw (HOTLIST_MSG);
    }
    
    /* connect if needed and redraw DCC buffer */
    if (DCC_IS_SEND(type))
    {
        if (!dcc_connect (new_dcc))
        {
            dcc_close (new_dcc, DCC_FAILED);
            dcc_redraw (HOTLIST_MSG);
            return NULL;
        }
    }
    
    if ( ( (type == DCC_CHAT_RECV) && (cfg_dcc_auto_accept_chats) )
        || ( (type == DCC_FILE_RECV) && (cfg_dcc_auto_accept_files) ) )
        dcc_accept (new_dcc);
    else
        dcc_redraw (HOTLIST_PRIVATE);
    gui_status_draw (gui_current_window->buffer, 0);
    
    return new_dcc;
}

/*
 * dcc_send_request: send DCC request (file or chat)
 */

void
dcc_send_request (t_irc_server *server, int type, char *nick, char *filename)
{
    char *dir1, *dir2, *filename2, *short_filename, *pos;
    int spaces, args, port_start, port_end;
    struct stat st;
    int sock, port;
    struct hostent *host;
    struct in_addr tmpaddr;
    struct sockaddr_in addr;
    socklen_t length;
    unsigned long local_addr;
    t_irc_dcc *ptr_dcc;
    
    filename2 = NULL;
    short_filename = NULL;
    spaces = 0;
    
    if (type == DCC_FILE_SEND)
    {
        /* add home if filename not beginning with '/' (not for Win32) */
#ifdef _WIN32
        filename2 = strdup (filename);
#else
        if (filename[0] == '/')
            filename2 = strdup (filename);
        else
        {
            dir1 = weechat_strreplace (cfg_dcc_upload_path, "~", getenv ("HOME"));
            if (!dir1)
                return;
            dir2 = weechat_strreplace (dir1, "%h", weechat_home);
            if (!dir2)
            {
                free (dir1);
                return;
            }
            filename2 = (char *) malloc (strlen (dir2) +
                                         strlen (filename) + 4);
            if (!filename2)
            {
                irc_display_prefix (server, server->buffer, PREFIX_ERROR);
                gui_printf (server->buffer,
                            _("%s not enough memory for DCC SEND\n"),
                            WEECHAT_ERROR);
                return;
            }
            strcpy (filename2, dir2);
            if (filename2[strlen (filename2) - 1] != DIR_SEPARATOR_CHAR)
                strcat (filename2, DIR_SEPARATOR);
            strcat (filename2, filename);
            if (dir1)
                free (dir1);
            if (dir2)
                free (dir2);
        }
#endif
        
        /* check if file exists */
        if (stat (filename2, &st) == -1)
        {
            irc_display_prefix (server, server->buffer, PREFIX_ERROR);
            gui_printf (server->buffer,
                        _("%s cannot access file \"%s\"\n"),
                        WEECHAT_ERROR, filename2);
            if (filename2)
                free (filename2);
            return;
        }
    }
    
    /* get local IP address */
    
    /* look up the IP address from dcc_own_ip, if set */
    local_addr = 0;
    if (cfg_dcc_own_ip && cfg_dcc_own_ip[0])
    {
        host = gethostbyname (cfg_dcc_own_ip);
        if (host)
        {
            memcpy (&tmpaddr, host->h_addr_list[0], sizeof(struct in_addr));
            local_addr = ntohl (tmpaddr.s_addr);
        }
        else
            gui_printf (server->buffer,
                        _("%s could not find address for '%s'. Falling back to local IP.\n"),
                        WEECHAT_WARNING, cfg_dcc_own_ip);
    }
    
    /* use the local interface, from the server socket */
    memset (&addr, 0, sizeof (struct sockaddr_in));
    length = sizeof (addr);
    getsockname (server->sock, (struct sockaddr *) &addr, &length);
    addr.sin_family = AF_INET;
    
    /* fallback to the local IP address on the interface, if required */
    if (local_addr == 0)
        local_addr = ntohl (addr.sin_addr.s_addr);
    
    /* open socket for DCC */
    sock = socket (AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        irc_display_prefix (server, server->buffer, PREFIX_ERROR);
        gui_printf (server->buffer,
                    _("%s cannot create socket for DCC\n"),
                    WEECHAT_ERROR);
        if (filename2)
            free (filename2);
        return;
    }
    
    /* look for port */
    
    port = 0;
    
    if (cfg_dcc_port_range && cfg_dcc_port_range[0])
    {
        /* find a free port in the specified range */
        args = sscanf (cfg_dcc_port_range, "%d-%d", &port_start, &port_end);
        if (args > 0)
        {
            port = port_start;
            if (args == 1)
                port_end = port_start;
            
            /* loop through the entire allowed port range */
            while (port <= port_end)
            {
                if (!dcc_port_in_use (port))
                {
                    /* attempt to bind to the free port */
                    addr.sin_port = htons (port);
                    if (bind (sock, (struct sockaddr *) &addr, sizeof (addr)) == 0)
                        break;
                }
                port++;
            }
            
            if (port > port_end)
                port = -1;
        }
    }
    
    if (port == 0)
    {
        /* find port automatically */
        addr.sin_port = 0;
        if (bind (sock, (struct sockaddr *) &addr, sizeof (addr)) == 0)
        {
            length = sizeof (addr);
            getsockname (sock, (struct sockaddr *) &addr, &length);
            port = ntohs (addr.sin_port);
        }
        else
            port = -1;
    }

    if (port == -1)
    {
        /* Could not find any port to bind */
        irc_display_prefix (server, server->buffer, PREFIX_ERROR);
        gui_printf (server->buffer,
                    _("%s cannot find available port for DCC\n"),
                    WEECHAT_ERROR);
        close (sock);
        if (filename2)
            free (filename2);
        return;
    }
    
    if (type == DCC_FILE_SEND)
    {
        /* extract short filename (without path) */
        pos = strrchr (filename2, DIR_SEPARATOR_CHAR);
        if (pos)
            short_filename = strdup (pos + 1);
        else
            short_filename = strdup (filename2);
        
        /* convert spaces to underscore if asked and needed */
        pos = short_filename;
        spaces = 0;
        while (pos[0])
        {
            if (pos[0] == ' ')
            {
                if (cfg_dcc_convert_spaces)
                    pos[0] = '_';
                else
                    spaces = 1;
            }
            pos++;
        }
    }
    
    /* add DCC entry and listen to socket */
    if (type == DCC_CHAT_SEND)
        ptr_dcc = dcc_add (server, DCC_CHAT_SEND, local_addr, port, nick, sock,
                           NULL, NULL, 0);
    else
        ptr_dcc = dcc_add (server, DCC_FILE_SEND, local_addr, port, nick, sock,
                           short_filename, filename2, st.st_size);
    if (!ptr_dcc)
    {
        irc_display_prefix (server, server->buffer, PREFIX_ERROR);
        gui_printf (server->buffer,
                    _("%s cannot send DCC\n"),
                    WEECHAT_ERROR);
        close (sock);
        if (short_filename)
            free (short_filename);
        if (filename2)
            free (filename2);
        return;
    }
    
    /* send DCC request to nick */
    if (type == DCC_CHAT_SEND)
        server_sendf (server, 
                      "PRIVMSG %s :\01DCC CHAT chat %lu %d\01\r\n",
                      nick, local_addr, port);
    else
        server_sendf (server, 
                      (spaces) ?
                          "PRIVMSG %s :\01DCC SEND \"%s\" %lu %d %u\01\r\n" :
                          "PRIVMSG %s :\01DCC SEND %s %lu %d %u\01\r\n",
                      nick, short_filename, local_addr, port,
                      (unsigned long) st.st_size);
    
    if (short_filename)
        free (short_filename);
    if (filename2)
        free (filename2);
}

/*
 * dcc_chat_send: send data to remote host via DCC CHAT
 */

int
dcc_chat_send (t_irc_dcc *ptr_dcc, char *buffer, int size_buf)
{
    if (!ptr_dcc)
        return -1;
    
    return send (ptr_dcc->sock, buffer, size_buf, 0);
}

/*
 * dcc_chat_sendf: send formatted data to remote host via DCC CHAT
 */

void
dcc_chat_sendf (t_irc_dcc *ptr_dcc, char *fmt, ...)
{
    va_list args;
    static char buffer[4096];
    char *buf2;
    int size_buf;

    if (!ptr_dcc || (ptr_dcc->sock < 0))
        return;
    
    va_start (args, fmt);
    size_buf = vsnprintf (buffer, sizeof (buffer) - 1, fmt, args);
    va_end (args);
    
    if ((size_buf == 0) || (strcmp (buffer, "\r\n") == 0))
        return;
    
    buffer[sizeof (buffer) - 1] = '\0';
    if ((size_buf < 0) || (size_buf > (int) (sizeof (buffer) - 1)))
        size_buf = strlen (buffer);
#ifdef DEBUG
    buffer[size_buf - 2] = '\0';
    gui_printf (ptr_dcc->server->buffer, "[DEBUG] Sending to remote host (DCC CHAT) >>> %s\n", buffer);
    buffer[size_buf - 2] = '\r';
#endif
    buf2 = channel_iconv_encode (ptr_dcc->server,
                                 ptr_dcc->channel,
                                 buffer);
    if (dcc_chat_send (ptr_dcc, buf2, strlen (buf2)) <= 0)
    {
        irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                            PREFIX_ERROR);
        gui_printf (ptr_dcc->server->buffer,
                    _("%s error sending data to \"%s\" via DCC CHAT\n"),
                    WEECHAT_ERROR, ptr_dcc->nick);
        dcc_close (ptr_dcc, DCC_FAILED);
    }
    free (buf2);
}

/*
 * dcc_chat_recv: receive data from DCC CHAT host
 */

void
dcc_chat_recv (t_irc_dcc *ptr_dcc)
{
    static char buffer[4096 + 2];
    char *buf2, *pos, *ptr_buf, *ptr_buf2, *next_ptr_buf;
    char *ptr_buf_color;
    int num_read;

    num_read = recv (ptr_dcc->sock, buffer, sizeof (buffer) - 2, 0);
    if (num_read > 0)
    {
        buffer[num_read] = '\0';
        
        buf2 = NULL;
        ptr_buf = buffer;
        if (ptr_dcc->unterminated_message)
        {
            buf2 = (char *) malloc (strlen (ptr_dcc->unterminated_message) +
                strlen (buffer) + 1);
            if (buf2)
            {
                strcpy (buf2, ptr_dcc->unterminated_message);
                strcat (buf2, buffer);
            }
            ptr_buf = buf2;
            free (ptr_dcc->unterminated_message);
            ptr_dcc->unterminated_message = NULL;
        }
        
        while (ptr_buf && ptr_buf[0])
        {
            next_ptr_buf = NULL;
            pos = strstr (ptr_buf, "\r\n");
            if (pos)
            {
                pos[0] = '\0';
                next_ptr_buf = pos + 2;
            }
            else
            {
                pos = strstr (ptr_buf, "\n");
                if (pos)
                {
                    pos[0] = '\0';
                    next_ptr_buf = pos + 1;
                }
                else
                {
                    ptr_dcc->unterminated_message = strdup (ptr_buf);
                    ptr_buf = NULL;
                    next_ptr_buf = NULL;
                }
            }
            
            if (ptr_buf)
            {
                ptr_buf2 = channel_iconv_decode (ptr_dcc->server,
                                                 ptr_dcc->channel,
                                                 ptr_buf);
                ptr_buf_color = (char *)gui_color_decode ((ptr_buf2) ?
                                                          (unsigned char *)ptr_buf2 : (unsigned char *)ptr_buf,
                                                          cfg_irc_colors_receive);
                
                if (irc_is_highlight (ptr_buf, ptr_dcc->server->nick))
                {
                    irc_display_nick (ptr_dcc->channel->buffer, NULL, ptr_dcc->nick,
                                      MSG_TYPE_NICK | MSG_TYPE_HIGHLIGHT, 1,
                                      COLOR_WIN_CHAT_HIGHLIGHT, 0);
                    if ((cfg_look_infobar_delay_highlight > 0)
                        && (ptr_dcc->channel->buffer != gui_current_window->buffer))
                    {
                        gui_infobar_printf (cfg_look_infobar_delay_highlight,
                                            COLOR_WIN_INFOBAR_HIGHLIGHT,
                                            _("Private %s> %s"),
                                            ptr_dcc->nick,
                                            (ptr_buf_color) ? ptr_buf_color : ((ptr_buf2) ? ptr_buf2 : ptr_buf));
                    }
                }
                else
                    irc_display_nick (ptr_dcc->channel->buffer, NULL, ptr_dcc->nick,
                                      MSG_TYPE_NICK, 1, COLOR_WIN_NICK_PRIVATE, 0);
                gui_printf_type (ptr_dcc->channel->buffer, MSG_TYPE_MSG,
                                 "%s%s\n",
                                 GUI_COLOR(COLOR_WIN_CHAT),
                                 (ptr_buf_color) ? ptr_buf_color : ptr_buf);
                if (ptr_buf_color)
                    free (ptr_buf_color);
                if (ptr_buf2)
                    free (ptr_buf2);
            }
            
            ptr_buf = next_ptr_buf;
        }
        
        if (buf2)
            free (buf2);
    }
    else
    {
        dcc_close (ptr_dcc, DCC_ABORTED);
        dcc_redraw (HOTLIST_MSG);
    }
}

/*
 * dcc_file_create_pipe: create pipe for communication with child process
 *                       return 1 if ok, 0 if error
 */

int
dcc_file_create_pipe (t_irc_dcc *ptr_dcc)
{
    int child_pipe[2];
    
    if (pipe (child_pipe) < 0)
    {
        irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                            PREFIX_ERROR);
        gui_printf (ptr_dcc->server->buffer,
                    _("%s DCC: unable to create pipe\n"),
                    WEECHAT_ERROR);
        dcc_close (ptr_dcc, DCC_FAILED);
        dcc_redraw (HOTLIST_MSG);
        return 0;
    }
    
    ptr_dcc->child_read = child_pipe[0];
    ptr_dcc->child_write = child_pipe[1];
    return 1;
}

/*
 * dcc_file_write_pipe: write data into pipe
 */

void
dcc_file_write_pipe (t_irc_dcc *ptr_dcc, int status, int error)
{
    char buffer[1 + 1 + 12 + 1];   /* status + error + pos + \0 */

    snprintf (buffer, sizeof (buffer), "%c%c%012lu",
              status + '0', error + '0', ptr_dcc->pos);
    write (ptr_dcc->child_write, buffer, sizeof (buffer));
}

/*
 * dcc_file_send_child: child process for sending file
 */

void
dcc_file_send_child (t_irc_dcc *ptr_dcc)
{
    int num_read, num_sent;
    static char buffer[DCC_MAX_BLOCKSIZE];
    uint32_t ack;
    time_t last_sent, new_time;
    
    last_sent = time (NULL);
    while (1)
    {
        /* read DCC ACK (sent by receiver) */
        if (ptr_dcc->pos > ptr_dcc->ack)
        {
            /* we should receive ACK for packets sent previously */
            while (1)
            {
                num_read = recv (ptr_dcc->sock, (char *) &ack, 4, MSG_PEEK);
                if ((num_read < 1) &&
                    ((num_read != -1) || (errno != EAGAIN)))
                {
                    dcc_file_write_pipe (ptr_dcc, DCC_FAILED, DCC_ERROR_READ_LOCAL);
                    return;
                }
                if (num_read == 4)
                {
                    recv (ptr_dcc->sock, (char *) &ack, 4, 0);
                    ptr_dcc->ack = ntohl (ack);
                    
                    /* DCC send ok? */
                    if ((ptr_dcc->pos >= ptr_dcc->size)
                        && (ptr_dcc->ack >= ptr_dcc->size))
                    {
                        dcc_file_write_pipe (ptr_dcc, DCC_DONE, DCC_NO_ERROR);
                        return;
                    }
                }
                else
                    break;
            }
        }
        
        /* send a block to receiver */
        if ((ptr_dcc->pos < ptr_dcc->size) &&
             (ptr_dcc->fast_send || (ptr_dcc->pos <= ptr_dcc->ack)))
        {
            lseek (ptr_dcc->file, ptr_dcc->pos, SEEK_SET);
            num_read = read (ptr_dcc->file, buffer, ptr_dcc->blocksize);
            if (num_read < 1)
            {
                dcc_file_write_pipe (ptr_dcc, DCC_FAILED, DCC_ERROR_READ_LOCAL);
                return;
            }
            num_sent = send (ptr_dcc->sock, buffer, num_read, 0);
            if (num_sent < 0)
            {
                /* socket is temporarily not available (receiver can't receive
                   amount of data we sent ?!) */
                if (errno == EAGAIN)
                    usleep (1000);
                else
                {
                    dcc_file_write_pipe (ptr_dcc, DCC_FAILED, DCC_ERROR_READ_LOCAL);
                    return;
                }
            }
            if (num_sent > 0)
            {
                ptr_dcc->pos += (unsigned long) num_sent;
                new_time = time (NULL);
                if (last_sent != new_time)
                {
                    last_sent = new_time;
                    dcc_file_write_pipe (ptr_dcc, DCC_ACTIVE, DCC_NO_ERROR);
                }
            }
        }
        else
            usleep (1000);
    }
}

/*
 * dcc_file_recv_child: child process for receiving file
 */

void
dcc_file_recv_child (t_irc_dcc *ptr_dcc)
{
    int num_read;
    static char buffer[DCC_MAX_BLOCKSIZE];
    uint32_t pos;
    time_t last_sent, new_time;

    last_sent = time (NULL);
    while (1)
    {    
        num_read = recv (ptr_dcc->sock, buffer, sizeof (buffer), 0);
        if (num_read == -1)
        {
            /* socket is temporarily not available (sender is not fast ?!) */
            if (errno == EAGAIN)
                usleep (1000);
            else
            {
                dcc_file_write_pipe (ptr_dcc, DCC_FAILED, DCC_ERROR_RECV_BLOCK);
                return;
            }
        }
        else
        {
            if (num_read == 0)
            {
                dcc_file_write_pipe (ptr_dcc, DCC_FAILED, DCC_ERROR_RECV_BLOCK);
                return;
            }
            
            if (write (ptr_dcc->file, buffer, num_read) == -1)
            {
                dcc_file_write_pipe (ptr_dcc, DCC_FAILED, DCC_ERROR_WRITE_LOCAL);
                return;
            }
            
            ptr_dcc->pos += (unsigned long) num_read;
            pos = htonl (ptr_dcc->pos);
            
            /* we don't check return code, not a problem if an ACK send failed */
            send (ptr_dcc->sock, (char *) &pos, 4, 0);

            /* file received ok? */
            if (ptr_dcc->pos >= ptr_dcc->size)
            {
                dcc_file_write_pipe (ptr_dcc, DCC_DONE, DCC_NO_ERROR);
                return;
            }
            
            new_time = time (NULL);
            if (last_sent != new_time)
            {
                last_sent = new_time;
                dcc_file_write_pipe (ptr_dcc, DCC_ACTIVE, DCC_NO_ERROR);
            }
        }
    }
}

/*
 * dcc_file_child_read: read data from child via pipe
 */

void
dcc_file_child_read (t_irc_dcc *ptr_dcc)
{
    char bufpipe[1 + 1 + 12 + 1];
    int num_read;
    char *error;
    
    num_read = read (ptr_dcc->child_read, bufpipe, sizeof (bufpipe));
    if (num_read > 0)
    {
        error = NULL;
        ptr_dcc->pos = strtol (bufpipe + 2, &error, 10);
        ptr_dcc->last_activity = time (NULL);
        dcc_calculate_speed (ptr_dcc, 0);
        
        /* read error code */
        switch (bufpipe[1] - '0')
        {
            case DCC_ERROR_READ_LOCAL:
                irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                    PREFIX_ERROR);
                gui_printf (ptr_dcc->server->buffer,
                            _("%s DCC: unable to read local file\n"),
                            WEECHAT_ERROR);
                break;
            case DCC_ERROR_SEND_BLOCK:
                irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                    PREFIX_ERROR);
                gui_printf (ptr_dcc->server->buffer,
                            _("%s DCC: unable to send block to receiver\n"),
                            WEECHAT_ERROR);
                break;
            case DCC_ERROR_READ_ACK:
                irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                    PREFIX_ERROR);
                gui_printf (ptr_dcc->server->buffer,
                            _("%s DCC: unable to read ACK from receiver\n"),
                            WEECHAT_ERROR);
                break;
            case DCC_ERROR_RECV_BLOCK:
                irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                    PREFIX_ERROR);
                gui_printf (ptr_dcc->server->buffer,
                            _("%s DCC: unable to receive block from sender\n"),
                            WEECHAT_ERROR);
                break;
            case DCC_ERROR_WRITE_LOCAL:
                irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                    PREFIX_ERROR);
                gui_printf (ptr_dcc->server->buffer,
                            _("%s DCC: unable to write local file\n"),
                            WEECHAT_ERROR);
                break;
        }
        
        /* read new DCC status */
        switch (bufpipe[0] - '0')
        {
            case DCC_ACTIVE:
                dcc_redraw (HOTLIST_LOW);
                break;
            case DCC_DONE:
                dcc_close (ptr_dcc, DCC_DONE);
                dcc_redraw (HOTLIST_MSG);
                break;
            case DCC_FAILED:
                dcc_close (ptr_dcc, DCC_FAILED);
                dcc_redraw (HOTLIST_MSG);
                break;
        }
    }
}

/*
 * dcc_file_send_fork: fork process for sending file
 */

void
dcc_file_send_fork (t_irc_dcc *ptr_dcc)
{
    pid_t pid;
    
    if (!dcc_file_create_pipe (ptr_dcc))
        return;

    ptr_dcc->file = open (ptr_dcc->local_filename, O_RDONLY | O_NONBLOCK, 0644);
    
    switch (pid = fork ())
    {
        /* fork failed */
        case -1:
            irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                PREFIX_ERROR);
            gui_printf (ptr_dcc->server->buffer,
                        _("%s DCC: unable to fork\n"),
                        WEECHAT_ERROR);
            dcc_close (ptr_dcc, DCC_FAILED);
            dcc_redraw (HOTLIST_MSG);
            return;
            /* child process */
        case 0:
            setuid (getuid ());
            dcc_file_send_child (ptr_dcc);
            _exit (EXIT_SUCCESS);
    }
    
    /* parent process */
    ptr_dcc->child_pid = pid;
}

/*
 * dcc_file_recv_fork: fork process for receiving file
 */

void
dcc_file_recv_fork (t_irc_dcc *ptr_dcc)
{
    pid_t pid;
    
    if (!dcc_file_create_pipe (ptr_dcc))
        return;
    
    if (ptr_dcc->start_resume > 0)
        ptr_dcc->file = open (ptr_dcc->local_filename,
                              O_APPEND | O_WRONLY | O_NONBLOCK);
    else
        ptr_dcc->file = open (ptr_dcc->local_filename,
                              O_CREAT | O_TRUNC | O_WRONLY | O_NONBLOCK,
                              0644);
    
    switch (pid = fork ())
    {
        /* fork failed */
        case -1:
            irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                PREFIX_ERROR);
            gui_printf (ptr_dcc->server->buffer,
                        _("%s DCC: unable to fork\n"),
                        WEECHAT_ERROR);
            dcc_close (ptr_dcc, DCC_FAILED);
            dcc_redraw (HOTLIST_MSG);
            return;
            /* child process */
        case 0:
            setuid (getuid ());
            dcc_file_recv_child (ptr_dcc);
            _exit (EXIT_SUCCESS);
    }
    
    /* parent process */
    ptr_dcc->child_pid = pid;
}

/*
 * dcc_handle: receive/send data for all active DCC
 */

void
dcc_handle ()
{
    t_irc_dcc *ptr_dcc;
    fd_set read_fd;
    static struct timeval timeout;
    int sock;
    struct sockaddr_in addr;
    socklen_t length;
    
    for (ptr_dcc = dcc_list; ptr_dcc; ptr_dcc = ptr_dcc->next_dcc)
    {
        /* check DCC timeout */
        if (DCC_IS_FILE(ptr_dcc->type) && !DCC_ENDED(ptr_dcc->status))
        {
            if ((cfg_dcc_timeout != 0) && (time (NULL) > ptr_dcc->last_activity + cfg_dcc_timeout))
            {
                irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                    PREFIX_ERROR);
                gui_printf (ptr_dcc->server->buffer,
                            _("%s DCC: timeout\n"),
                            WEECHAT_ERROR);
                dcc_close (ptr_dcc, DCC_FAILED);
                dcc_redraw (HOTLIST_MSG);
                continue;
            }
        }
        
        if (ptr_dcc->status == DCC_CONNECTING)
        {
            if (ptr_dcc->type == DCC_FILE_SEND)
            {
                FD_ZERO (&read_fd);
                FD_SET (ptr_dcc->sock, &read_fd);
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
                
                /* something to read on socket? */
                if (select (FD_SETSIZE, &read_fd, NULL, NULL, &timeout) > 0)
                {
                    if (FD_ISSET (ptr_dcc->sock, &read_fd))
                    {
                        ptr_dcc->last_activity = time (NULL);
                        length = sizeof (addr);
                        sock = accept (ptr_dcc->sock, (struct sockaddr *) &addr, &length);
                        close (ptr_dcc->sock);
                        ptr_dcc->sock = -1;
                        if (sock < 0)
                        {
                            irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                                PREFIX_ERROR);
                            gui_printf (ptr_dcc->server->buffer,
                                        _("%s DCC: unable to create socket for sending file\n"),
                                        WEECHAT_ERROR);
                            dcc_close (ptr_dcc, DCC_FAILED);
                            dcc_redraw (HOTLIST_MSG);
                            continue;
                        }
                        ptr_dcc->sock = sock;
                        if (fcntl (ptr_dcc->sock, F_SETFL, O_NONBLOCK) == -1)
                        {
                            irc_display_prefix (ptr_dcc->server, ptr_dcc->server->buffer,
                                                PREFIX_ERROR);
                            gui_printf (ptr_dcc->server->buffer,
                                        _("%s DCC: unable to set 'nonblock' option for socket\n"),
                                        WEECHAT_ERROR);
                            dcc_close (ptr_dcc, DCC_FAILED);
                            dcc_redraw (HOTLIST_MSG);
                            continue;
                        }
                        ptr_dcc->addr = ntohl (addr.sin_addr.s_addr);
                        ptr_dcc->status = DCC_ACTIVE;
                        ptr_dcc->start_transfer = time (NULL);
                        dcc_redraw (HOTLIST_MSG);
                        dcc_file_send_fork (ptr_dcc);
                    }
                }
            }
        }
        
        if (ptr_dcc->status == DCC_WAITING)
        {
            if (ptr_dcc->type == DCC_CHAT_SEND)
            {
                FD_ZERO (&read_fd);
                FD_SET (ptr_dcc->sock, &read_fd);
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
                
                /* something to read on socket? */
                if (select (FD_SETSIZE, &read_fd, NULL, NULL, &timeout) > 0)
                {
                    if (FD_ISSET (ptr_dcc->sock, &read_fd))
                    {
                        length = sizeof (addr);
                        sock = accept (ptr_dcc->sock, (struct sockaddr *) &addr, &length);
                        close (ptr_dcc->sock);
                        ptr_dcc->sock = -1;
                        if (sock < 0)
                        {
                            dcc_close (ptr_dcc, DCC_FAILED);
                            dcc_redraw (HOTLIST_MSG);
                            continue;
                        }
                        ptr_dcc->sock = sock;
                        if (fcntl (ptr_dcc->sock, F_SETFL, O_NONBLOCK) == -1)
                        {
                            dcc_close (ptr_dcc, DCC_FAILED);
                            dcc_redraw (HOTLIST_MSG);
                            continue;
                        }
                        ptr_dcc->addr = ntohl (addr.sin_addr.s_addr);
                        ptr_dcc->status = DCC_ACTIVE;
                        dcc_redraw (HOTLIST_MSG);
                        dcc_channel_for_chat (ptr_dcc);
                    }
                }
            }
        }
        
        if (ptr_dcc->status == DCC_ACTIVE)
        {
            if (DCC_IS_CHAT(ptr_dcc->type))
            {
                FD_ZERO (&read_fd);
                FD_SET (ptr_dcc->sock, &read_fd);
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
                
                /* something to read on socket? */
                if (select (FD_SETSIZE, &read_fd, NULL, NULL, &timeout) > 0)
                {
                    if (FD_ISSET (ptr_dcc->sock, &read_fd))
                        dcc_chat_recv (ptr_dcc);
                }
            }
            else
            {
                FD_ZERO (&read_fd);
                FD_SET (ptr_dcc->child_read, &read_fd);
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
                
                /* something to read on child pipe? */
                if (select (FD_SETSIZE, &read_fd, NULL, NULL, &timeout) > 0)
                {
                    if (FD_ISSET (ptr_dcc->child_read, &read_fd))
                        dcc_file_child_read (ptr_dcc);
                }
            }
        }
    }
}

/*
 * dcc_end: close all opened sockets (called when WeeChat is exiting)
 */

void
dcc_end ()
{
    t_irc_dcc *ptr_dcc;
    
    for (ptr_dcc = dcc_list; ptr_dcc; ptr_dcc = ptr_dcc->next_dcc)
    {
        if (ptr_dcc->sock >= 0)
        {
            if (ptr_dcc->status == DCC_ACTIVE)
                weechat_log_printf (_("Aborting active DCC: \"%s\" from %s\n"),
                                    ptr_dcc->filename, ptr_dcc->nick);
            dcc_close (ptr_dcc, DCC_FAILED);
        }
    }
}

/*
 * dcc_print_log: print DCC infos in log (usually for crash dump)
 */

void
dcc_print_log ()
{
    t_irc_dcc *ptr_dcc;
    
    for (ptr_dcc = dcc_list; ptr_dcc; ptr_dcc = ptr_dcc->next_dcc)
    {
        weechat_log_printf ("\n");
        weechat_log_printf ("[DCC (addr:0x%X)]\n", ptr_dcc);
        weechat_log_printf ("  server. . . . . . . : 0x%X\n", ptr_dcc->server);
        weechat_log_printf ("  channel . . . . . . : 0x%X\n", ptr_dcc->channel);
        weechat_log_printf ("  type. . . . . . . . : %d\n",   ptr_dcc->type);
        weechat_log_printf ("  status. . . . . . . : %d\n",   ptr_dcc->status);
        weechat_log_printf ("  start_time. . . . . : %ld\n",  ptr_dcc->start_time);
        weechat_log_printf ("  start_transfer. . . : %ld\n",  ptr_dcc->start_transfer);
        weechat_log_printf ("  addr. . . . . . . . : %lu\n",  ptr_dcc->addr);
        weechat_log_printf ("  port. . . . . . . . : %d\n",   ptr_dcc->port);
        weechat_log_printf ("  nick. . . . . . . . : '%s'\n", ptr_dcc->nick);
        weechat_log_printf ("  sock. . . . . . . . : %d\n",   ptr_dcc->sock);
        weechat_log_printf ("  child_pid . . . . . : %d\n",   ptr_dcc->child_pid);
        weechat_log_printf ("  child_read. . . . . : %d\n",   ptr_dcc->child_read);
        weechat_log_printf ("  child_write . . . . : %d\n",   ptr_dcc->child_write);
        weechat_log_printf ("  unterminated_message: '%s'\n", ptr_dcc->unterminated_message);
        weechat_log_printf ("  fast_send . . . . . : %d\n",   ptr_dcc->fast_send);
        weechat_log_printf ("  file. . . . . . . . : %d\n",   ptr_dcc->file);
        weechat_log_printf ("  filename. . . . . . : '%s'\n", ptr_dcc->filename);
        weechat_log_printf ("  local_filename. . . : '%s'\n", ptr_dcc->local_filename);
        weechat_log_printf ("  filename_suffix . . : %d\n",   ptr_dcc->filename_suffix);
        weechat_log_printf ("  blocksize . . . . . : %d\n",   ptr_dcc->blocksize);
        weechat_log_printf ("  size. . . . . . . . : %lu\n",  ptr_dcc->size);
        weechat_log_printf ("  pos . . . . . . . . : %lu\n",  ptr_dcc->pos);
        weechat_log_printf ("  ack . . . . . . . . : %lu\n",  ptr_dcc->ack);
        weechat_log_printf ("  start_resume. . . . : %lu\n",  ptr_dcc->start_resume);
        weechat_log_printf ("  last_check_time . . : %ld\n",  ptr_dcc->last_check_time);
        weechat_log_printf ("  last_check_pos. . . : %lu\n",  ptr_dcc->last_check_pos);
        weechat_log_printf ("  last_activity . . . : %ld\n",  ptr_dcc->last_activity);
        weechat_log_printf ("  bytes_per_sec . . . : %lu\n",  ptr_dcc->bytes_per_sec);
        weechat_log_printf ("  eta . . . . . . . . : %lu\n",  ptr_dcc->eta);
        weechat_log_printf ("  prev_dcc. . . . . . : 0x%X\n", ptr_dcc->prev_dcc);
        weechat_log_printf ("  next_dcc. . . . . . : 0x%X\n", ptr_dcc->next_dcc);
    }
}
