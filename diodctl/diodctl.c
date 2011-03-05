/*****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security, LLC.
 *  Written by Jim Garlick <garlick@llnl.gov> LLNL-CODE-423279
 *  All Rights Reserved.
 *
 *  This file is part of the Distributed I/O Daemon (diod).
 *  For details, see <http://code.google.com/p/diod/>.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License (as published by the
 *  Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA or see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/

/* diodctl.c - super server for diod (runs as root on 9pfs port) */

/* What we do:
 * - Serve /diodctl synthetic file system
 * - File 'exports' contains list of I/O node exports
 * - File 'server' contains server port
 * - When an attached user reads 'server', if a diod server is already
 *   running for that user, return port.  If not, spawn it and return port.
 * - Reap children when they quit (on last trans shutdown)
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#define _BSD_SOURCE         /* daemon */
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#if HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <string.h>
#include <sys/resource.h>
#include <poll.h>
#include <assert.h>

#include "9p.h"
#include "npfs.h"
#include "list.h"

#include "diod_log.h"
#include "diod_conf.h"
#include "diod_trans.h"
#include "diod_upool.h"
#include "diod_sock.h"

#include "ops.h"
#include "serv.h"

static void          _daemonize (char *Lopt);
static void          _setrlimit (void); 

#ifndef NR_OPEN
#define NR_OPEN         1048576 /* works on RHEL 5 x86_64 arch */
#endif

#define OPTIONS "fd:l:w:c:e:anD:L:"
#if HAVE_GETOPT_LONG
#define GETOPT(ac,av,opt,lopt) getopt_long (ac,av,opt,lopt,NULL)
static const struct option longopts[] = {
    {"foreground",      no_argument,        0, 'f'},
    {"debug",           required_argument,  0, 'd'},
    {"listen",          required_argument,  0, 'l'},
    {"nwthreads",       required_argument,  0, 'w'},
    {"config-file",     required_argument,  0, 'c'},
    {"export",          required_argument,  0, 'e'},
    {"allowany",        no_argument,        0, 'a'},
    {"no-auth",         no_argument,        0, 'n'},
    {"diod-path",       required_argument,  0, 'D'},
    {"log-dest",        required_argument,  0, 'L'},
    {0, 0, 0, 0},
};
#else
#define GETOPT(ac,av,opt,lopt) getopt (ac,av,opt)
#endif

static void
usage()
{
    fprintf (stderr, 
"Usage: diodctl [OPTIONS]\n"
"   -f,--foreground        do not fork and disassociate with tty\n"
"   -d,--debug MASK        set debugging mask\n"
"   -l,--listen IP:PORT    set interface to listen on (just one allowed)\n"
"   -w,--nwthreads INT     set number of I/O worker threads to spawn\n"
"   -c,--config-file FILE  set config file path\n"
"   -e,--export PATH       export PATH (just one allowed)\n"
"   -a,--allowany          disable TCP wrappers checks\n"
"   -n,--no-auth           disable authentication check\n"
"   -D,--diod-path PATH    set path to diod executable\n"
"   -L,--log-dest DEST     log to DEST, can be syslog, stderr, or file\n"
"Note: command line overrides config file\n");
    exit (1);
}

int
main(int argc, char **argv)
{
    Npsrv *srv;
    int c;
    int eopt = 0;
    int lopt = 0;
    char *copt = NULL;
    char *Lopt = NULL;
    struct pollfd *fds = NULL;
    int nfds = 0;
    List hplist;
   
    diod_log_init (argv[0]); 
    diod_conf_init ();

    /* config file overrides defaults */
    opterr = 0;
    while ((c = GETOPT (argc, argv, OPTIONS, longopts)) != -1) {
        switch (c) {
            case 'c':   /* --config-file PATH */
                copt = optarg;
                break;
            default:
                break;
        }
    }
#if HAVE_LUA_H
    diod_conf_init_config_file (copt);
#else
    if (copt)
        msg_exit ("No lua support but --config-file was specified");
#endif
    /* command line overrides config file */
    optind = 0;
    opterr = 0;
    while ((c = GETOPT (argc, argv, OPTIONS, longopts)) != -1) {
        switch (c) {
            case 'f':   /* --foreground */
                diod_conf_set_foreground (1);
                break;
            case 'd':   /* --debug MASK */
                diod_conf_set_debuglevel (strtoul (optarg, NULL, 0));
                break;
            case 'l':   /* --listen HOST:PORT */
                if (!lopt) {
                    diod_conf_clr_diodctllisten ();
                    lopt = 1;
                }
                if (!strchr (optarg, ':'))
                    usage ();
                diod_conf_add_diodctllisten (optarg);
                break;
            case 'w':   /* --nwthreads INT */
                diod_conf_set_nwthreads (strtoul (optarg, NULL, 10));
                break;
            case 'c':   /* --config-file PATH */
                break;
            case 'e':   /* --export PATH */
                if (!eopt) {
                    diod_conf_clr_export ();
                    eopt = 1;
                }
                diod_conf_add_export (optarg);
                break;
            case 'a':   /* --allowany */
                diod_conf_set_tcpwrappers (0);
                break;
            case 'n':   /* --no-auth */
                diod_conf_set_auth_required (0);
                break;
            case 'D':   /* --diod-path PATH */
                diod_conf_set_diodpath (optarg);
                break;
            case 'L':   /* --log-to DEST */
                diod_log_set_dest (optarg);
                Lopt = optarg;
                break;
            default:
                usage();
        }
    }
    if (optind < argc)
        usage();

    /* sane config? */
    diod_conf_validate_exports ();
#if ! HAVE_TCP_WRAPPERS
    if (diod_conf_get_tcpwrappers ())
        msg_exit ("no TCP wrapper support but config enables it");
#endif

    if (geteuid () != 0)
        msg_exit ("must run as root");
    _setrlimit ();

    if (!diod_conf_get_foreground ())
        _daemonize (Lopt);

    srv = np_srv_create (diod_conf_get_nwthreads ());
    if (!srv)
        msg_exit ("out of memory");

    hplist = diod_conf_get_diodctllisten ();
    if (!diod_sock_listen_hostport_list (hplist, &fds, &nfds, NULL, 0))
        msg_exit ("failed to set up listen ports");

    diodctl_serv_init ();
    diodctl_register_ops (srv);
    diod_sock_accept_loop (srv, fds, nfds, diod_conf_get_tcpwrappers ());
    /*NOTREACHED*/

    exit (0);
}

/* Create run directory if it doesn't exist and chdir there.
 * Disassociate from parents controlling tty.  Switch logging to syslog.
 * Exit on error.
 */
static void
_daemonize (char *Lopt)
{
    char rdir[PATH_MAX];
    struct stat sb;

    snprintf (rdir, sizeof(rdir), "%s/run/diod", X_LOCALSTATEDIR);
    if (stat (rdir, &sb) < 0) {
        if (mkdir (rdir, 0755) < 0)
            err_exit ("mkdir %s", rdir);
    } else if (!S_ISDIR (sb.st_mode))
        msg_exit ("%s is not a directory", rdir);
    
    if (chdir (rdir) < 0)
        err_exit ("chdir %s", rdir);
    if (daemon (1, 0) < 0)
        err_exit ("daemon");
    if (Lopt == NULL)
        diod_log_set_dest ("syslog");
}

/* Remove any resource limits that might hamper our (non-root) children.
 * Exit on error.
 */
static void 
_setrlimit (void)
{
    struct rlimit r, r2;

    r.rlim_cur = r.rlim_max = RLIM_INFINITY;    
    if (setrlimit (RLIMIT_FSIZE, &r) < 0)
        err_exit ("setrlimit RLIMIT_FSIZE");

    r.rlim_cur = r.rlim_max = NR_OPEN;
    r2.rlim_cur = r2.rlim_max = sysconf(_SC_OPEN_MAX);
    if (setrlimit (RLIMIT_NOFILE, &r) < 0)
        if (errno != EPERM || setrlimit (RLIMIT_NOFILE, &r2) < 0)
            err_exit ("setrlimit RLIMIT_NOFILE");

    r.rlim_cur = r.rlim_max = RLIM_INFINITY;    
    if (setrlimit (RLIMIT_LOCKS, &r) < 0)
        err_exit ("setrlimit RLIMIT_LOCKS");

    r.rlim_cur = r.rlim_max = RLIM_INFINITY;    
    if (setrlimit (RLIMIT_CORE, &r) < 0)
        err_exit ("setrlimit RLIMIT_CORE");

    r.rlim_cur = r.rlim_max = RLIM_INFINITY;    
    if (setrlimit (RLIMIT_AS, &r) < 0)
        err_exit ("setrlimit RLIMIT_AS");
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
