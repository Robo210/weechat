/*
 * Copyright (c) 2003-2008 by FlashCode <flashcode@flashtux.org>
 * See README for License detail, AUTHORS for developers list.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* gui-curses-main.c: main loop for Curses GUI */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include "../../core/weechat.h"
#include "../../core/wee-command.h"
#include "../../core/wee-config.h"
#include "../../core/wee-hook.h"
#include "../../core/wee-string.h"
#include "../../core/wee-utf8.h"
#include "../../core/wee-util.h"
#include "../../plugins/plugin.h"
#include "../gui-main.h"
#include "../gui-bar.h"
#include "../gui-bar-item.h"
#include "../gui-buffer.h"
#include "../gui-chat.h"
#include "../gui-color.h"
#include "../gui-infobar.h"
#include "../gui-input.h"
#include "../gui-history.h"
#include "../gui-nicklist.h"
#include "../gui-status.h"
#include "../gui-window.h"
#include "gui-curses.h"


/*
 * gui_main_pre_init: pre-initialize GUI (called before gui_init)
 */

void
gui_main_pre_init (int *argc, char **argv[])
{
    /* make C compiler happy */
    (void) argc;
    (void) argv;
    
    /* pre-init colors */
    gui_color_pre_init ();
    
    /* build empty prefixes (before reading config) */
    gui_chat_prefix_build_empty ();
}

/*
 * gui_main_init: init GUI
 */

void
gui_main_init ()
{
    struct t_gui_buffer *ptr_buffer;
    struct t_gui_bar *ptr_bar;
    struct t_gui_bar_window *ptr_bar_win;
    
    initscr ();
    
    curs_set (1);
    noecho ();
    nodelay (stdscr, TRUE);
    raw ();
    
    gui_color_init ();

    /* build prefixes according to config */
    gui_chat_prefix_build ();
    
    gui_infobar = NULL;
    
    gui_ok = ((COLS >= GUI_WINDOW_MIN_WIDTH)
              && (LINES >= GUI_WINDOW_MIN_HEIGHT));
    
    refresh ();
    
    /* init clipboard buffer */
    gui_input_clipboard = NULL;
    
    /* get time length */
    gui_chat_time_length = util_get_time_length (CONFIG_STRING(config_look_buffer_time_format));
    
    /* init bar items */
    gui_bar_item_init ();
    
    /* create new window/buffer */
    if (gui_window_new (NULL, 0, 0, COLS, LINES, 100, 100))
    {
        gui_current_window = gui_windows;
        ptr_buffer = gui_buffer_new (NULL, "weechat", "weechat",
                                     NULL, NULL,
                                     NULL, NULL);
        if (ptr_buffer)
        {
            gui_init_ok = 1;
            gui_buffer_set_title (ptr_buffer,
                                  "WeeChat " WEECHAT_COPYRIGHT_DATE
                                  " - " WEECHAT_WEBSITE);
            gui_window_redraw_buffer (ptr_buffer);
        }
        else
            gui_init_ok = 0;
        
        if (CONFIG_BOOLEAN(config_look_set_title))
            gui_window_title_set ();
    }
    
    if (gui_init_ok)
    {
        /* create bar windows for root bars (they were read from config,
           but no window was created (GUI was not initialized) */
        for (ptr_bar = gui_bars; ptr_bar; ptr_bar = ptr_bar->next_bar)
        {
            if ((CONFIG_INTEGER(ptr_bar->type) == GUI_BAR_TYPE_ROOT) && (!ptr_bar->bar_window))
                gui_bar_window_new (ptr_bar, NULL);
        }
        for (ptr_bar_win = GUI_CURSES(gui_windows)->bar_windows;
             ptr_bar_win; ptr_bar_win = ptr_bar_win->next_bar_window)
        {
            gui_bar_window_calculate_pos_size (ptr_bar_win, gui_windows);
            gui_bar_window_create_win (ptr_bar_win);
        }
    }
}

/*
 * gui_main_quit: quit WeeChat
 */

void
gui_main_quit ()
{
    quit_weechat = 1;
}

/*
 * gui_main_reload: reload WeeChat configuration
 */

void
gui_main_reload ()
{
    command_reload (NULL, NULL, 0, NULL, NULL);
}

/*
 * gui_main_loop: main loop for WeeChat with ncurses GUI
 */

void
gui_main_loop ()
{
    struct t_hook *hook_fd_keyboard;
    struct t_gui_window *ptr_win;
    struct t_gui_buffer *ptr_buffer;
    struct timeval tv_timeout;
    fd_set read_fds, write_fds, except_fds;
    int max_fd;
    int ready;
    
    quit_weechat = 0;
    
    /* catch SIGTERM signal: quit program */
    util_catch_signal (SIGTERM, &gui_main_quit);
    
    /* catch SIGHUP signal: reload configuration */
    util_catch_signal (SIGHUP, &gui_main_reload);
    
    /* catch SIGWINCH signal: redraw screen */
    util_catch_signal (SIGWINCH, &gui_window_refresh_screen_sigwinch);
    
    /* hook stdin (read keyboard) */
    hook_fd_keyboard = hook_fd (NULL, STDIN_FILENO, 1, 0, 0,
                                &gui_keyboard_read_cb, NULL);
    
    while (!quit_weechat)
    {
        /* execute hook timers */
        hook_timer_exec ();
        
        /* refresh window if needed */
        if (gui_window_refresh_needed)
            gui_window_refresh_screen ();
        
        /* refresh status bar if needed */
        if (gui_status_refresh_needed)
            gui_status_draw (1);
        
        for (ptr_win = gui_windows; ptr_win; ptr_win = ptr_win->next_window)
        {
            if (ptr_win->refresh_needed)
            {
                gui_window_switch_to_buffer (ptr_win, ptr_win->buffer);
                gui_window_redraw_buffer (ptr_win->buffer);
                ptr_win->refresh_needed = 0;
            }
        }
        
        for (ptr_buffer = gui_buffers; ptr_buffer;
             ptr_buffer = ptr_buffer->next_buffer)
        {
            /* refresh title if needed */
            if (ptr_buffer->title_refresh_needed)
                gui_chat_draw_title (ptr_buffer, 1);
            
            /* refresh chat if needed */
            if (ptr_buffer->chat_refresh_needed)
            {
                gui_chat_draw (ptr_buffer,
                               (ptr_buffer->chat_refresh_needed) > 1 ? 1 : 0);
            }
            
            /* refresh nicklist if needed */
            if (ptr_buffer->nicklist_refresh_needed)
                gui_nicklist_draw (ptr_buffer, 1);
            
            /* refresh input if needed */
            if (ptr_buffer->input_refresh_needed)
                gui_input_draw (ptr_buffer, 1);
        }
        
        /* wait for keyboard or network activity */
        FD_ZERO (&read_fds);
        FD_ZERO (&write_fds);
        FD_ZERO (&except_fds);
        max_fd = hook_fd_set (&read_fds, &write_fds, &except_fds);
        if (hook_timer_time_to_next (&tv_timeout))
            ready = select (max_fd + 1, &read_fds, &write_fds, &except_fds,
                            &tv_timeout);
        else
            ready = select (max_fd + 1, &read_fds, &write_fds, &except_fds,
                            NULL);
        if (ready > 0)
        {
            hook_fd_exec (&read_fds, &write_fds, &except_fds);
        }
    }
    
    /* remove keyboard hook */
    unhook (hook_fd_keyboard);
}

/*
 * gui_main_end: GUI end
 */

void
gui_main_end ()
{
    /* remove bar items and bars */
    gui_bar_item_end ();
    gui_bar_free_all ();
    
    /* free clipboard buffer */
    if (gui_input_clipboard)
        free (gui_input_clipboard);
    
    /* delete all windows */
    while (gui_windows)
        gui_window_free (gui_windows);
    gui_window_tree_free (&gui_windows_tree);

    /* delete all buffers */
    while (gui_buffers)
        gui_buffer_close (gui_buffers, 0);
    
    /* delete global history */
    gui_history_global_free ();
    
    /* delete infobar messages */
    while (gui_infobar)
        gui_infobar_remove ();

    /* reset title */
    if (CONFIG_BOOLEAN(config_look_set_title))
	gui_window_title_reset ();
    
    /* end color */
    gui_color_end ();
    
    /* end of Curses output */
    refresh ();
    endwin ();
}
