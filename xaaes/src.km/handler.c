/*
 * $Id$
 *
 * XaAES - XaAES Ain't the AES (c) 1992 - 1998 C.Graham
 *                                 1999 - 2003 H.Robbers
 *                                        2004 F.Naumann
 *
 * A multitasking AES replacement for MiNT
 *
 * This file is part of XaAES.
 *
 * XaAES is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * XaAES is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with XaAES; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Main AES trap handler routine
 * -----------------------------
 * This module replaces the AES trap 2 vector to provide an interface to the
 * XaAES pipe based client/server AES, for normal GEM applications.
 *
 * It works by first creating a pair of XaAES reply pipes for the application
 * in response to the appl_init() call, then using these to communicate with the
 * AES server kernal. When an AES trap occurs, the handler drops the pointer to the
 * parameter block into the XaAES.cmd pipe.
 *
 * There are then 3 modes that the AES could have been called in.
 *
 * If standard GEM emulation mode the handler then drops back into user mode and
 * blocks whilst reading on the current process's reply pipe. This allows other
 * processes to execute whilst XaAES is performing AES functions (the XaAES server
 * runs wholely in user mode so it's more MiNT-friendly than MultiTOS). The server
 * writes back to the clients reply pipe with the reply when it has serviced
 * the command - this unblocks the client which then returns from the exception.
 *
 * If NOREPLY mode is used, the AES doesn't block the calling process to wait
 * for a reply - and indeed, won't generate one.
 *
 * If NOBLOCK mode is used, the AES doesn't block the calling process - but does
 * place the results in the client's reply pipe. The client is then expected to
 * handle it's own reply pipe. This allows multiple AES calls to be made without
 * blocking a process, so an app could make all it's GEM initialisation calls
 * at one go, then go on to do it's internal initialisation before coming back
 * to see if the AES has serviced it's requests (less blocking in the client,
 * and better multitasking).
 *
 * [13/2/96]
 * Included Martin Koehling's patches - this nicely does away with the 'far' data
 * kludges & register patches I had in before.....
 *
 * [18/2/96]
 * New timeout stuff to replace the SIGALRM stuff.
 *
 * [18/12/00]
 * HR: The 4 functions that must always be called direct, and hence run under
 *     the client pid are moved to this file. This way it is easier to detect any
 *     data area's that are shared between the server and the client processes.
 *     These functions are: appl_init, appl_exit, appl_yield and wind_update.
 *
 * [05/12/01]
 * Unclumsified the timeout value passing.
 *
 */

#include "handler.h"
#include "xa_global.h"

#include "c_window.h"
#include "k_main.h"
#include "k_mouse.h"
#include "op_names.h"
#include "xa_codes.h"

#include "mint/signal.h"


/* The main AES kernal command jump table */
static struct xa_ftab aes_tab[KtableSize];

/* timeout callback */
static void
wakeme_timeout(struct proc *p, struct xa_client *client)
{
	client->timeout = NULL;
	wake(IO_Q, (long)client);
}

/*
 * Trap exception handler
 * - This routine executes under the client application's pid
 * - I've semaphore locked any sensitive bits
 */
long _cdecl
XA_handler(void *_pb)
{
	register AESPB *pb = _pb;
	struct xa_client *client;
	short cmd;

	if (!pb)
	{
		DIAGS(("XaAES: No Parameter Block (pid %d)\n", p_getpid()));
		raise(SIGSYS);

		return 0;
	}

	cmd = pb->control[0];

	client = lookup_extension(NULL, XAAES_MAGIC);
	if (!client && cmd != XA_APPL_INIT)
	{
		pb->intout[0] = 0;

		DIAGS(("XaAES: AES trap (cmd %i) for non AES process (pid %ld, pb 0x%lx)\n", cmd, p_getpid(), pb));
		raise(SIGILL);

		return 0;
	}

	/* default paths are kept per process by MiNT ??
	 * so we need to get them here when we run under the process id.
	 */
	if (client)
	{
		client->usr_evnt = 0;

#if 0
		if ((!strcmp("  Thing Desktop", client->name)) || (!strcmp("  PORTHOS ", client->name)))
		{
			display("%s opcod %d - %d, %d, %d, %d, %d, %d, %d, %d, %d, %d", client->name, cmd,
			pb->intin[0], pb->intin[1], pb->intin[2], pb->intin[3], pb->intin[4],
			pb->intin[5], pb->intin[6], pb->intin[7], pb->intin[8], pb->intin[9] );

			if (cmd == XA_SHELL_WRITE)
				display("\n SW '%s','%s'", pb->addrin[0], pb->addrin[1]);
		}
#endif

		if (   cmd == XA_RSRC_LOAD
		    || cmd == XA_SHELL_FIND)
		{
			client->xdrive = d_getdrv();
			d_getpath(client->xpath, 0);
		}
	}

	/* better check this first */
	if ((cmd >= 0) && (cmd < KtableSize))
	{
		AES_function *cmd_routine;
		unsigned long cmd_rtn;

#if 0
		if ((cfg.fsel_cookie || cfg.no_xa_fsel)
		    && (   cmd == XA_FSEL_INPUT
		        || cmd == XA_FSEL_EXINPUT))
		{
			DIAG((D_fsel, client, "Redirected fsel call"));

			/* This causes call via the old vector
			 * see p_handlr.s
			 */
			return -1;
		}
#endif

		DIAG((D_trap, client, "AES trap: %s[%d] made by %s",
			op_code_names[cmd], cmd, client->name));

		cmd_routine = aes_tab[cmd].f;

		/* if opcode is implemented, call it */
		if (cmd_routine)
		{
			/* The root of all locking under client pid.
			 * 
			 * how about this? It means that these
			 * semaphores are not needed and are effectively skipped.
			 */
			enum locks lock = winlist|envstr|pending;

			// XXX ???
			// Ozk: Me thinks this is not necessary... but I'm not sure.
			//vq_mouse(C.vh, &button.b, &button.x, &button.y);
			//vq_key_s(C.vh, &button.ks);

			if (aes_tab[cmd].p & LOCKSCREEN)
				lock_screen(client, 0/*-1*/, NULL, 2);

			/* callout the AES function */
			cmd_rtn = (*cmd_routine)(lock, client, pb);

			if (aes_tab[cmd].p & LOCKSCREEN)
				unlock_screen(client, 2);

			/* execute delayed delete_window */
			if (S.deleted_windows.first)
				do_delayed_delete_window(lock);

			DIAG((D_trap, client, " %s[%d] retuned %ld for %s",
				op_code_names[cmd], cmd, cmd_rtn, client->name));

			switch (cmd_rtn)
			{
				case XAC_DONE:
					break;

				/* block indefinitly */
				case XAC_BLOCK:
				{
					if (!client)
						client = lookup_extension(NULL, XAAES_MAGIC);
					if (client)
					{
						DIAG((D_trap, client, "XA_Hand: Block client %s", client->name));
						Block(client, 1);
						DIAG((D_trap, client, "XA_Hand: Unblocked %s", client->name));
						break;
					}
				}

				/* block with timeout */
				case XAC_TIMER:
				{
					if (!client)
						client = lookup_extension(NULL, XAAES_MAGIC);

					if (client)
					{
						if (client->timer_val)
						{
							client->timeout = addtimeout(client->timer_val,
										     wakeme_timeout);

							if (client->timeout)
								client->timeout->arg = (long)client;
						}

						Block(client, 1);
						
						if (client->timeout)
						{
							canceltimeout(client->timeout);
							client->timeout = NULL;
						}
						else
						{
							/* timeout */

							if (client->waiting_for & XAWAIT_MULTI)
							{
								short *o;

								/* fill out mouse data */

								o = client->waiting_pb->intout;
								if (!(o[0] & MU_BUTTON))
								{
									check_mouse(client, o+3, o+1, o+2);
									vq_key_s(C.vh, o+4);
								}

								o[0] = MU_TIMER;
								o[5] = 0;
								o[6] = 0;
							}
							else
								/* evnt_timer() always returns 1 */
								client->waiting_pb->intout[0] = 1;

							cancel_evnt_multi(client,3);
							DIAG((D_kern, client, "[20]Unblocked timer for %s",
								c_owner(client)));
						}
					}
				}
			}
#if GENERATE_DIAGS
			if (client)
			{
				if (mouse_locked() || update_locked())
				{
					if (mouse_locked() == client)
						DIAG((D_kern, client, "Leaving AES with mouselock %s", client->name));
					if (update_locked() == client)
						DIAG((D_kern, client, "Leaving AES with screenlock %s", client->name));
				}
				else
					DIAG((D_kern, client, "Leaving AES %s", client->name));
			}
#endif
			return 0;
		}
		else
		{
			DIAGS(("Unimplemented AES code: %d", cmd));
		}
	}
	else
	{
		DIAGS(("Unimplemented AES code: %d", cmd));
	}

	/* error exit */
	pb->intout[0] = 0;
	return 0;
}


/*
 * Setup the AES kernal jump table
 * 
 * HR 210202: LOCKSCREEN flag for AES functions that are writing the screen
 *            and are supposed to be locking
 * HR 110802: NO_SEMA flag for Call direct for small AES functions that do
 *            not need the call_direct semaphores.
 */

#include "xa_appl.h"
#include "xa_form.h"
#include "xa_fsel.h"
#include "xa_evnt.h"
#include "xa_graf.h"
#include "xa_wind.h"
#include "xa_objc.h"
#include "xa_rsrc.h"
#include "xa_menu.h"
#include "xa_shel.h"
#include "xa_scrp.h"
#include "xa_wdlg.h"
#include "xa_lbox.h"

void
setup_k_function_table(void)
{
	bzero(aes_tab, sizeof(aes_tab));

	/*
	 * appl_? class functions
	 */

	aes_tab[XA_APPL_INIT   ].f = XA_appl_init;
	aes_tab[XA_APPL_INIT   ].d = true;		/* Must always call appl_init/exit/yield directly */

	aes_tab[XA_APPL_EXIT   ].f = XA_appl_exit;
	aes_tab[XA_APPL_EXIT   ].d = true;		/* because they must run under the client pid */

	aes_tab[XA_APPL_GETINFO].f = XA_appl_getinfo;
	aes_tab[XA_APPL_GETINFO].p = NO_SEMA;

	aes_tab[XA_APPL_FIND   ].f = XA_appl_find;
	aes_tab[XA_APPL_FIND   ].p = NO_SEMA;

	aes_tab[XA_APPL_WRITE  ].f = XA_appl_write;
/*	aes_tab[XA_APPL_WRITE  ].p = NO_SEMA; */

	aes_tab[XA_APPL_YIELD  ].f = XA_appl_yield;
	aes_tab[XA_APPL_YIELD  ].d = true;

	aes_tab[XA_APPL_SEARCH ].f = XA_appl_search;
	aes_tab[XA_APPL_CONTROL].f = XA_appl_control;


	/*
	 * form_? class functions
	 */

	aes_tab[XA_FORM_ALERT ].f = XA_form_alert;
	aes_tab[XA_FORM_ALERT ].p = LOCKSCREEN;
	aes_tab[XA_FORM_ERROR ].f = XA_form_error;
	aes_tab[XA_FORM_ERROR ].p = LOCKSCREEN;

	aes_tab[XA_FORM_CENTER].f = XA_form_center;
	aes_tab[XA_FORM_CENTER].p = NO_SEMA;

	aes_tab[XA_FORM_DIAL  ].f = XA_form_dial;
	aes_tab[XA_FORM_DIAL  ].p = LOCKSCREEN;
	aes_tab[XA_FORM_BUTTON].f = XA_form_button;
	aes_tab[XA_FORM_DO    ].f = XA_form_do;
	aes_tab[XA_FORM_DO    ].p = LOCKSCREEN;
	aes_tab[XA_FORM_KEYBD ].f = XA_form_keybd;


	/*
	 * fsel_? class functions
	 */

#if FILESELECTOR
	if (!cfg.no_xa_fsel)
	{
		aes_tab[XA_FSEL_INPUT  ].f = XA_fsel_input;
		aes_tab[XA_FSEL_INPUT  ].p = LOCKSCREEN;
		aes_tab[XA_FSEL_EXINPUT].f = XA_fsel_exinput;
		aes_tab[XA_FSEL_EXINPUT].p = LOCKSCREEN;
	}
#endif

	/*
	 * evnt_? class functions
	 */

	aes_tab[XA_EVNT_BUTTON].f = XA_evnt_button;
	aes_tab[XA_EVNT_KEYBD ].f = XA_evnt_keybd;
	aes_tab[XA_EVNT_MESAG ].f = XA_evnt_mesag;
	aes_tab[XA_EVNT_MULTI ].f = XA_evnt_multi;
	aes_tab[XA_EVNT_TIMER ].f = XA_evnt_timer;
	aes_tab[XA_EVNT_MOUSE ].f = XA_evnt_mouse;
/*	aes_tab[XA_EVNT_DCLICK].f = XA_evnt_dclick; */


	/*
	 * graf_? class functions
	 */

	aes_tab[XA_GRAF_RUBBERBOX].f = XA_graf_rubberbox;
	aes_tab[XA_GRAF_DRAGBOX  ].f = XA_graf_dragbox;

	aes_tab[XA_GRAF_HANDLE   ].f = XA_graf_handle;
	aes_tab[XA_GRAF_HANDLE   ].p = NO_SEMA;

	aes_tab[XA_GRAF_MOUSE    ].f = XA_graf_mouse;
	aes_tab[XA_GRAF_MOUSE    ].p = NO_SEMA;

	aes_tab[XA_GRAF_MKSTATE  ].f = XA_graf_mkstate;
/*	aes_tab[XA_GRAF_MKSTATE  ].p = NO_SEMA; */

/*	aes_tab[XA_GRAF_GROWBOX  ].f = XA_graf_growbox; */
/*	aes_tab[XA_GRAF_SHRINKBOX].f = XA_graf_growbox; */
/*	aes_tab[XA_GRAF_MOVEBOX  ].f = XA_graf_movebox; */
	aes_tab[XA_GRAF_SLIDEBOX ].f = XA_graf_slidebox;
	aes_tab[XA_GRAF_WATCHBOX ].f = XA_graf_watchbox;


	/*
	 * wind_? class functions
	 */

	aes_tab[XA_WIND_CREATE].f = XA_wind_create;

	aes_tab[XA_WIND_OPEN  ].f = XA_wind_open;
	aes_tab[XA_WIND_OPEN  ].p = LOCKSCREEN;

	aes_tab[XA_WIND_CLOSE ].f = XA_wind_close;
	aes_tab[XA_WIND_CLOSE ].p = LOCKSCREEN;

	aes_tab[XA_WIND_SET   ].f = XA_wind_set;
	aes_tab[XA_WIND_SET   ].p = LOCKSCREEN;

	aes_tab[XA_WIND_GET   ].f = XA_wind_get;
	aes_tab[XA_WIND_FIND  ].f = XA_wind_find;

	aes_tab[XA_WIND_UPDATE].f = XA_wind_update;
	/* wind_update must ALWAYS be call direct as it uses semaphore locking */
	aes_tab[XA_WIND_UPDATE].d = true;

	aes_tab[XA_WIND_DELETE].f = XA_wind_delete;

	aes_tab[XA_WIND_NEW   ].f = XA_wind_new;
	aes_tab[XA_WIND_NEW   ].p = LOCKSCREEN;

	aes_tab[XA_WIND_CALC  ].f = XA_wind_calc;


	/*
	 * objc_? class functions
	 */

	aes_tab[XA_OBJC_ADD   ].f = XA_objc_add;
	aes_tab[XA_OBJC_DELETE].f = XA_objc_delete;

	aes_tab[XA_OBJC_DRAW  ].f = XA_objc_draw;
/*	aes_tab[XA_OBJC_DRAW  ].p = LOCKSCREEN; */

	aes_tab[XA_OBJC_FIND  ].f = XA_objc_find;
	aes_tab[XA_OBJC_FIND  ].p = NO_SEMA;

	aes_tab[XA_OBJC_OFFSET].f = XA_objc_offset;
	aes_tab[XA_OBJC_OFFSET].p = NO_SEMA;

	aes_tab[XA_OBJC_ORDER ].f = XA_objc_order;
	aes_tab[XA_OBJC_CHANGE].f = XA_objc_change;
	aes_tab[XA_OBJC_EDIT  ].f = XA_objc_edit;

	aes_tab[XA_OBJC_SYSVAR].f = XA_objc_sysvar;
	aes_tab[XA_OBJC_SYSVAR].p = NO_SEMA;


	/*
	 * rsrc_? class functions
	 */

	aes_tab[XA_RSRC_LOAD ].f = XA_rsrc_load;
	aes_tab[XA_RSRC_FREE ].f = XA_rsrc_free;

	aes_tab[XA_RSRC_GADDR].f = XA_rsrc_gaddr;
	aes_tab[XA_RSRC_GADDR].p = NO_SEMA;

	aes_tab[XA_RSRC_OBFIX].f = XA_rsrc_obfix;
	aes_tab[XA_RSRC_OBFIX].p = NO_SEMA;

	aes_tab[XA_RSRC_RCFIX].f = XA_rsrc_rcfix;
#if MEM_PROT
	/* rsrc_load must ALWAYS be call direct as it uses semaphore locking */
	aes_tab[XA_RSRC_LOAD].d = true;
	/* rsrc_free must ALWAYS be call direct as it uses semaphore locking */
	aes_tab[XA_RSRC_FREE].d = true;
#endif


	/*
	 * menu_? class functions
	 */

	aes_tab[XA_MENU_BAR     ].f = XA_menu_bar;
	aes_tab[XA_MENU_BAR     ].p = LOCKSCREEN;
	aes_tab[XA_MENU_TNORMAL ].f = XA_menu_tnormal;
	aes_tab[XA_MENU_TNORMAL ].p = LOCKSCREEN;
	aes_tab[XA_MENU_ICHECK  ].f = XA_menu_icheck;
	aes_tab[XA_MENU_ICHECK  ].p = LOCKSCREEN;
	aes_tab[XA_MENU_IENABLE ].f = XA_menu_ienable;
	aes_tab[XA_MENU_IENABLE ].p = LOCKSCREEN;
	aes_tab[XA_MENU_TEXT    ].f = XA_menu_text;
	aes_tab[XA_MENU_REGISTER].f = XA_menu_register;
	aes_tab[XA_MENU_POPUP   ].f = XA_menu_popup;
	aes_tab[XA_MENU_POPUP   ].p = LOCKSCREEN;		/* Ozk: Since we cannot lock with client PID
							 * and unlock under kernel PID
							 * we gotta let the function itself (under kernel PID)
							 * do the lock_screen() call
							 * Ozk: But we change it so all screenlocks happen under
							 * the client PID instead of under kernel PID..
							 */
	aes_tab[XA_MENU_ATTACH  ].f = XA_menu_attach;
	aes_tab[XA_MENU_ISTART  ].f = XA_menu_istart;
	aes_tab[XA_MENU_SETTINGS].f = XA_menu_settings;


	/*
	 * shell_? class functions
	 */

	aes_tab[XA_SHELL_WRITE].f = XA_shell_write;
	aes_tab[XA_SHELL_READ ].f = XA_shell_read;
	aes_tab[XA_SHELL_FIND ].f = XA_shell_find;
	aes_tab[XA_SHELL_ENVRN].f = XA_shell_envrn;
#if MEM_PROT
	aes_tab[XA_SHELL_ENVRN].d = true;
#endif


	/*
	 * scrap_? class functions
	 */

	aes_tab[XA_SCRAP_READ ].f = XA_scrp_read;
	aes_tab[XA_SCRAP_WRITE].f = XA_scrp_write;

	aes_tab[XA_FORM_POPUP ].f = XA_form_popup;
	aes_tab[XA_FORM_POPUP ].p = LOCKSCREEN;		/* Ozk: Same situation as for XA_menu_popup */


#if WDIAL
	/*
	 * wdlg_? class functions
	 */

	aes_tab[XA_WDIAL_CREATE].f = XA_wdlg_create;
	aes_tab[XA_WDIAL_OPEN  ].f = XA_wdlg_open;
	aes_tab[XA_WDIAL_CLOSE ].f = XA_wdlg_close;
	aes_tab[XA_WDIAL_DELETE].f = XA_wdlg_delete;
	aes_tab[XA_WDIAL_GET   ].f = XA_wdlg_get;
	aes_tab[XA_WDIAL_SET   ].f = XA_wdlg_set;
	aes_tab[XA_WDIAL_EVENT ].f = XA_wdlg_event;
	aes_tab[XA_WDIAL_REDRAW].f = XA_wdlg_redraw;
#endif


#if LBOX
	/*
	 * lbox_? class functions
	 */
	aes_tab[XA_LBOX_CREATE ].f = XA_lbox_create;
	aes_tab[XA_LBOX_UPDATE ].f = XA_lbox_update;
	aes_tab[XA_LBOX_DO     ].f = XA_lbox_do;
	aes_tab[XA_LBOX_DELETE ].f = XA_lbox_delete;
	aes_tab[XA_LBOX_GET    ].f = XA_lbox_get;
	aes_tab[XA_LBOX_SET    ].f = XA_lbox_set;
#endif
}
