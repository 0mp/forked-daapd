/*
 * Copyright (C) 2009 Julien BLACHE <jb@jblache.org>
 *
 * Pieces from mt-daapd:
 * Copyright (C) 2003 Ron Pedde (ron@pedde.com)
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file main.c
 *
 * Driver for mt-daapd, including the main() function.  This
 * is responsible for kicking off the initial mp3 scan, starting
 * up the signal handler, starting up the webserver, and waiting
 * around for external events to happen (like a request to rescan,
 * or a background rescan to take place.)
 *
 * It also contains the daap handling callback for the webserver.
 * This should almost certainly be somewhere else, and is in
 * desparate need of refactoring, but somehow continues to be in
 * this files.
 */

/** @mainpage mt-daapd
 * @section about_section About
 *
 * This is mt-daapd, an attempt to create an iTunes server for
 * linux and other POSIXish systems.  Maybe even Windows with cygwin,
 * eventually.
 *
 * You might check these locations for more info:
 * - <a href="http://www.mt-daapd.org">Home page</a>
 * - <a href="http://sf.net/projects/mt-daapd">Project page on SourceForge</a>
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <sys/signalfd.h>

#include "daapd.h"

#include "conf.h"
#include "configfile.h"
#include "err.h"
#include "mp3-scanner.h"
#include "webserver.h"
#include "db-generic.h"
#include "os.h"
#include "plugin.h"
#include "util.h"
#include "upnp.h"
#include "io.h"

#include "mdns_avahi.h"

#ifdef HAVE_GETOPT_H
# include "getopt.h"
#endif

#include <event.h>
#include <libavformat/avformat.h>


/** Seconds to sleep before checking for a shutdown or reload */
#define MAIN_SLEEP_INTERVAL  2

/** Let's hope if you have no atoll, you only have 32 bit inodes... */
#if !HAVE_ATOLL
#  define atoll(a) atol(a)
#endif

/*
 * Globals
 */
CONFIG config; /**< Main configuration structure, as read from configfile */

struct event_base *evbase_main;

/*
 * Forwards
 */
static void usage(char *program);
static void main_handler(WS_CONNINFO *pwsc);
static int main_auth(WS_CONNINFO *pwsc, char *username, char *password);
static void txt_add(char *txtrecord, char *fmt, ...);
static void main_io_errhandler(int level, char *msg);
static void main_ws_errhandler(int level, char *msg);

/**
 * build a dns text string
 *
 * @param txtrecord buffer to append text record string to
 * @param fmt sprintf-style format
 */
void txt_add(char *txtrecord, char *fmt, ...) {
    va_list ap;
    char buff[256];
    int len;
    char *end;

    va_start(ap, fmt);
    vsnprintf(buff, sizeof(buff), fmt, ap);
    va_end(ap);

    len = (int)strlen(buff);
    if(len + strlen(txtrecord) > 255) {
        DPRINTF(E_FATAL,L_MAIN,"dns-sd text string too long.  Try a shorter "
                "share name.\n");
    }

    end = txtrecord + strlen(txtrecord);
    *end = len;
    strcpy(end+1,buff);
}

void main_handler(WS_CONNINFO *pwsc) {
    DPRINTF(E_DBG,L_MAIN,"in main_handler\n");
    if(plugin_url_candispatch(pwsc)) {
        DPRINTF(E_DBG,L_MAIN,"Dispatching %s to plugin\n",ws_uri(pwsc));
        plugin_url_handle(pwsc);
        return;
    }

    DPRINTF(E_DBG,L_MAIN,"Dispatching %s to config handler\n",ws_uri(pwsc));
    config_handler(pwsc);
}

int main_auth(WS_CONNINFO *pwsc, char *username, char *password) {
    DPRINTF(E_DBG,L_MAIN,"in main_auth\n");
    if(plugin_url_candispatch(pwsc)) {
        DPRINTF(E_DBG,L_MAIN,"Dispatching auth for %s to plugin\n",ws_uri(pwsc));
        return plugin_auth_handle(pwsc,username,password);
    }

    DPRINTF(E_DBG,L_MAIN,"Dispatching auth for %s to config auth\n",ws_uri(pwsc));
    return config_auth(pwsc, username, password);
}


/**
 * Print usage information to stdout
 *
 * \param program name of program (argv[0])
 */
void usage(char *program) {
    printf("Usage: %s [options]\n\n",program);
    printf("Options:\n");
    printf("  -a             Set cwd to app dir before starting\n");
    printf("  -d <number>    Debug level (0-9)\n");
    printf("  -D <mod,mod..> Debug modules\n");
    printf("  -c <file>      Use configfile specified\n");
    printf("  -P <file>      Write the PID to specified file\n");
    printf("  -f             Run in foreground\n");
    printf("  -y             Yes, go ahead and run as non-root user\n");
    printf("  -b <id>        ffid to be broadcast\n");
    printf("  -V             Display version information\n");
    printf("  -k             Kill a running daemon (based on pidfile)\n");
    printf("\n\n");
    printf("Valid debug modules:\n");
    printf(" config,webserver,database,scan,query,index,browse\n");
    printf(" playlist,art,daap,main,rend,misc\n");
    printf("\n\n");
}

/**
 * process a directory for plugins
 *
 * @returns TRUE if at least one plugin loaded successfully
 */
int load_plugin_dir(char *plugindir) {
    DIR *d_plugin;
    char de[sizeof(struct dirent) + MAXNAMLEN + 1]; /* ?? solaris  */
    struct dirent *pde;
    char *pext;
    char *perr=NULL;
    int loaded=FALSE;
    char plugin[PATH_MAX];

    if((d_plugin=opendir(plugindir)) == NULL) {
        DPRINTF(E_LOG,L_MAIN,"Error opening plugin dir %s.  Ignoring\n",
                plugindir);
        return FALSE;

    } else {
        while((readdir_r(d_plugin,(struct dirent *)de,&pde) != 1) && pde) {
                pext = strrchr(pde->d_name,'.');
                if((pext) && ((strcasecmp(pext,".so") == 0) ||
                   (strcasecmp(pext,".dylib") == 0) ||
                   (strcasecmp(pext,".dll") == 0))) {
                    /* must be a plugin */
                    snprintf(plugin,PATH_MAX,"%s%c%s",plugindir,
                             PATHSEP,pde->d_name);
                    if(plugin_load(&perr,plugin) != PLUGIN_E_SUCCESS) {
                        DPRINTF(E_LOG,L_MAIN,"Error loading plugin %s: %s\n",
                                plugin,perr);
                        free(perr);
                        perr = NULL;
                    } else {
                        loaded = TRUE;
                    }
                }
        }
        closedir(d_plugin);
    }

    return loaded;
}

/**
 * set up an errorhandler for io errors
 *
 * @param int level of the error (0=fatal, 9=debug)
 * @param msg the text error
 */
void main_io_errhandler(int level, char *msg) {
    DPRINTF(level,L_MAIN,"%s",msg);
}

/**
 * set up an errorhandler for webserver errors
 *
 * @param int level of the error (0=fatal, 9=debug)
 * @param msg the text error
 */
void main_ws_errhandler(int level, char *msg) {
    DPRINTF(level,L_WS,"%s",msg);
}

static void
mainloop_cb(int fd, short event, void *arg)
{
  int old_song_count, song_count;
  char **mp3_dir_array;
  struct event *main_timer;
  struct timeval tv;
  static int rescan_counter = 0;

  rescan_counter += MAIN_SLEEP_INTERVAL;

  main_timer = (struct event *)arg;
  evutil_timerclear(&tv);
  tv.tv_sec = MAIN_SLEEP_INTERVAL;
  evtimer_add(main_timer, &tv);

  if ((conf_get_int("general", "rescan_interval", 0)
       && (rescan_counter > conf_get_int("general", "rescan_interval", 0))))
    {
      if ((conf_get_int("general", "always_scan", 0))
	  || (config_get_session_count()))
	{
	  config.reload = 1;
	}
      else
	{
	  DPRINTF(E_DBG, L_MAIN | L_SCAN | L_DB, "Skipped background scan... no users\n");
	}
      rescan_counter = 0;
    }

  if (config.reload)
    {
      db_get_song_count(NULL, &old_song_count);

      DPRINTF(E_LOG, L_MAIN | L_DB | L_SCAN, "Rescanning database\n");

      if (conf_get_array("general", "mp3_dir", &mp3_dir_array))
	{
	  if (config.full_reload)
	    {
	      config.full_reload = 0;
	      db_force_rescan(NULL);
	    }

	  if (scan_init(mp3_dir_array))
	    {
	      DPRINTF(E_LOG, L_MAIN | L_DB | L_SCAN, "Error rescanning... bad path?\n");
	    }
	  conf_dispose_array(mp3_dir_array);
	}
      config.reload = 0;

      db_get_song_count(NULL, &song_count);
      DPRINTF(E_LOG, L_MAIN | L_DB | L_SCAN, "Scanned %d songs (was %d)\n",song_count, old_song_count);
    }
}

static void
signal_cb(int fd, short event, void *arg)
{
  struct sigaction sa_ign;
  struct sigaction sa_dfl;
  struct signalfd_siginfo info;
  int status;

  sa_ign.sa_handler=SIG_IGN;
  sa_ign.sa_flags=0;
  sigemptyset(&sa_ign.sa_mask);

  sa_dfl.sa_handler=SIG_DFL;
  sa_dfl.sa_flags=0;
  sigemptyset(&sa_dfl.sa_mask);

  while (read(fd, &info, sizeof(struct signalfd_siginfo)) > 0)
    {
      switch (info.ssi_signo)
	{
	  case SIGCHLD:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGCHLD, reaping children\n");

	    while (wait3(&status, WNOHANG, NULL) > 0)
	      /* Nothing. */ ;
	    break;

	  case SIGINT:
	  case SIGTERM:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGTERM or SIGINT\n");

	    config.stop = 1;
	    break;

	  case SIGHUP:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGHUP\n");

	    if (!config.stop)
	      {
		conf_reload();
		err_reopen();

		config.reload = 1;
	      }
	    break;
	}
    }

  if (config.stop)
    event_base_loopbreak(evbase_main);
}

/**
 * Kick off the daap server and wait for events.
 *
 * This starts the initial db scan, sets up the signal
 * handling, starts the webserver, then sits back and waits
 * for events, as notified by the signal handler and the
 * web interface.  These events are communicated via flags
 * in the config structure.
 *
 * \param argc count of command line arguments
 * \param argv command line argument pointers
 * \returns 0 on success, -1 otherwise
 *
 * \todo split out a ws_init and ws_start, so that the
 * web space handlers can be registered before the webserver
 * starts.
 *
 */
int main(int argc, char *argv[]) {
    int option;
    char *configfile=CONFFILE;
    WSCONFIG ws_config;
    int reload=0;
    int start_time;
    int end_time;
    int song_count;
    int force_non_root=0;
    int skip_initial=1;
    int kill_server=0;
    int convert_conf=0;
    char *db_type,*db_parms,*web_root,*runas, *tmp;
    char **mp3_dir_array;
    char *servername;
    char *ffid = NULL;
    int appdir = 0;
    char *perr=NULL;
    char txtrecord[255];
    void *phandle;
    char *plugindir;
    struct event *main_timer;
    struct timeval tv;
    sigset_t sigs;
    int sigfd;
    struct event sig_event;
    int ret;

    int err;
    char *apppath;

    int debuglevel=0;
    int plugins_loaded = 0;

    err_setlevel(2);

    config.foreground=0;
    while((option=getopt(argc,argv,"D:d:c:P:mfrysiuvab:Vk")) != -1) {
        switch(option) {
        case 'a':
            appdir = 1;
            break;

        case 'b':
            ffid=optarg;
            break;

        case 'd':
            debuglevel = atoi(optarg);
            err_setlevel(debuglevel);
            break;

        case 'D':
            if(err_setdebugmask(optarg)) {
                usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            break;

        case 'f':
            config.foreground=1;
            err_setdest(err_getdest() | LOGDEST_STDERR);
            break;

        case 'c':
            configfile=optarg;
            break;

        case 'P':
            os_set_pidfile(optarg);
            break;

        case 'r':
            reload=1;
            break;

        case 's':
            skip_initial=0;
            break;

        case 'y':
            force_non_root=1;
            break;

        case 'v':
            convert_conf=1;
            break;

        case 'k':
            kill_server=1;
            break;

        case 'V':
            fprintf(stderr,"Firefly Media Server: Version %s\n",VERSION);
            exit(EXIT_SUCCESS);
            break;

        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
            break;
        }
    }

    if((getuid()) && (!force_non_root) && (!convert_conf)) {
        fprintf(stderr,"You are not root.  This is almost certainly wrong.  "
                "If you are\nsure you want to do this, use the -y "
                "command-line switch\n");
        exit(EXIT_FAILURE);
    }


    if(kill_server) {
        os_signal_server(S_STOP);
        exit(0);
    }

    io_init();
    io_set_errhandler(main_io_errhandler);
    ws_set_errhandler(main_ws_errhandler);

    /* read the configfile, if specified, otherwise
     * try defaults */
    config.stats.start_time=start_time=(int)time(NULL);
    config.stop=0;

    /* set appdir first, that way config resolves relative to appdir */
    if(appdir) {
        apppath = os_apppath(argv[0]);
        DPRINTF(E_INF,L_MAIN,"Changing cwd to %s\n",apppath);
        chdir(apppath);
        free(apppath);
        configfile="mt-daapd.conf";
    }

    if(CONF_E_SUCCESS != conf_read(configfile)) {
        fprintf(stderr,"Error reading config file (%s)\n",configfile);
        exit(EXIT_FAILURE);
    }

    if(debuglevel) /* was specified, should override the config file */
        err_setlevel(debuglevel);

    if(convert_conf) {
        fprintf(stderr,"Converting config file...\n");
        if(CONF_E_SUCCESS != conf_write()) {
            fprintf(stderr,"Error writing config file.\n");
            exit(EXIT_FAILURE);
        }
        exit(EXIT_SUCCESS);
    }

    DPRINTF(E_LOG,L_MAIN,"Firefly Version %s: Starting with debuglevel %d\n",
            VERSION,err_getlevel());

    /* initialize ffmpeg */
    av_register_all();

    /* load plugins before we drop privs?  Maybe... let the
     * plugins do stuff they might need to */
    plugin_init();
    if((plugindir=conf_alloc_string("plugins","plugin_dir",NULL)) != NULL) {
        /* instead of specifying plugins, let's walk through the directory
         * and load each of them */
        if(!load_plugin_dir(plugindir)) {
            DPRINTF(E_LOG,L_MAIN,"Warning: Could not load plugins\n");
        } else {
            plugins_loaded = TRUE;
        }
        free(plugindir);
    }

    if(!plugins_loaded) {
        if((!load_plugin_dir("/usr/lib/firefly/plugins")) &&
           (!load_plugin_dir("/usr/lib/mt-daapd/plugins")) &&
           (!load_plugin_dir("/lib/mt-daapd/plugins")) &&
           (!load_plugin_dir("/lib/mt-daapd/plugins")) &&
           (!load_plugin_dir("/usr/local/lib/mt-daapd/plugins")) &&
           (!load_plugin_dir("/usr/local/lib/mt-daapd/plugins")) &&
           (!load_plugin_dir("/opt/share/firefly/plugins")) &&
           (!load_plugin_dir("/opt/share/mt-daapd/plugins")) &&
           (!load_plugin_dir("/opt/lib/firefly/plugins")) &&
           (!load_plugin_dir("/opt/lib/mt-daapd/plugins")) &&
           (!load_plugin_dir("plugins/.libs"))) {
            DPRINTF(E_FATAL,L_MAIN,"plugins/plugin_dir not specified\n");
        }
    }

    phandle=NULL;
    while((phandle=plugin_enum(phandle))) {
        DPRINTF(E_LOG,L_MAIN,"Plugin loaded: %s\n",plugin_get_description(phandle));
    }

    /* Block signals for all threads except the main one */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    sigaddset(&sigs, SIGCHLD);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGPIPE);
    ret = pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    if (ret != 0)
      {
        DPRINTF(E_LOG, L_MAIN, "Error setting signal set\n");
	exit(EXIT_FAILURE);
      }

    runas = conf_alloc_string("general","runas","nobody");

    if(!os_init(config.foreground,runas)) {
      DPRINTF(E_LOG,L_MAIN,"Could not initialize server\n");

      exit(EXIT_FAILURE);
    }

    /* Initialize libevent (after forking) */
    evbase_main = event_init();
    main_timer = (struct event *)malloc(sizeof(struct event));
    if (!main_timer)
      {
	DPRINTF(E_FATAL, L_MAIN, "Out of memory\n");

	exit(EXIT_FAILURE);
      }
    free(runas);

    DPRINTF(E_LOG, L_MAIN, "mDNS init\n");
    ret = mdns_init();
    if (ret != 0)
      {
	DPRINTF(E_FATAL, L_MAIN | L_REND, "mDNS init failed\n");

	exit(EXIT_FAILURE);
      }

#ifdef UPNP
    upnp_init();
#endif

    /* this will require that the db be readable by the runas user */
    db_type = conf_alloc_string("general","db_type","sqlite");
    db_parms = conf_alloc_string("general","db_parms","/var/cache/mt-daapd");
    err=db_open(&perr,db_type,db_parms);

    if(err) {
        DPRINTF(E_LOG,L_MAIN|L_DB,"Error opening db: %s\n",perr);

	mdns_deinit();
        exit(EXIT_FAILURE);
    }

    free(db_type);
    free(db_parms);

    /* Initialize the database before starting */
    DPRINTF(E_LOG,L_MAIN|L_DB,"Initializing database\n");
    if(db_init(reload)) {
        DPRINTF(E_FATAL,L_MAIN|L_DB,"Error in db_init: %s\n",strerror(errno));
    }

    err=db_get_song_count(&perr,&song_count);
    if(err != DB_E_SUCCESS) {
        DPRINTF(E_FATAL,L_MISC,"Error getting song count: %s\n",perr);
    }
    /* do a full reload if the db is empty */
    if(!song_count)
        reload = 1;

    if(conf_get_array("general","mp3_dir",&mp3_dir_array)) {
        if((!skip_initial) || (reload)) {
            DPRINTF(E_LOG,L_MAIN|L_SCAN,"Starting mp3 scan\n");

            plugin_event_dispatch(PLUGIN_EVENT_FULLSCAN_START,0,NULL,0);
            start_time=(int) time(NULL);
            if(scan_init(mp3_dir_array)) {
                DPRINTF(E_LOG,L_MAIN|L_SCAN,"Error scanning MP3 files: %s\n",strerror(errno));
            }
            if(!config.stop) { /* don't send popup when shutting down */
                plugin_event_dispatch(PLUGIN_EVENT_FULLSCAN_END,0,NULL,0);
                err=db_get_song_count(&perr,&song_count);
                end_time=(int) time(NULL);
                DPRINTF(E_LOG,L_MAIN|L_SCAN,"Scanned %d songs in %d seconds\n",
                        song_count,end_time - start_time);
            }
        }
        conf_dispose_array(mp3_dir_array);
    }

    /* start up the web server */
    web_root = conf_alloc_string("general","web_root",NULL);
    ws_config.web_root=web_root;
    ws_config.port=conf_get_int("general","port",0);

    DPRINTF(E_LOG,L_MAIN|L_WS,"Starting web server from %s on port %d\n",
            ws_config.web_root, ws_config.port);

    config.server=ws_init(&ws_config);
    if(!config.server) {
        /* pthreads or malloc error */
        DPRINTF(E_FATAL,L_MAIN|L_WS,"Error initializing web server\n");
    }

    if(E_WS_SUCCESS != ws_start(config.server)) {
        /* listen or pthread error */
        DPRINTF(E_FATAL,L_MAIN|L_WS,"Error starting web server\n");
    }

    ws_registerhandler(config.server, "/",main_handler,main_auth,
                       0,1);

    /* Register mDNS services */
    servername = conf_get_servername();

    memset(txtrecord,0,sizeof(txtrecord));
    txt_add(txtrecord,"txtvers=1");
    txt_add(txtrecord,"Database ID=%0X",util_djb_hash_str(servername));
    txt_add(txtrecord,"Machine ID=%0X",util_djb_hash_str(servername));
    txt_add(txtrecord,"Machine Name=%s",servername);
    txt_add(txtrecord,"mtd-version=" VERSION);
    txt_add(txtrecord,"iTSh Version=131073"); /* iTunes 6.0.4 */
    txt_add(txtrecord,"Version=196610");      /* iTunes 6.0.4 */

    tmp = conf_alloc_string("general", "password", NULL);
    txt_add(txtrecord, "Password=%s", (tmp && (strlen(tmp) > 0)) ? "true" : "false");
    if (tmp)
      free(tmp);

    srand((unsigned int)time(NULL));

    if(ffid) {
      txt_add(txtrecord,"ffid=%s",ffid);
    } else {
      txt_add(txtrecord,"ffid=%08x",rand());
    }

    DPRINTF(E_LOG,L_MAIN|L_REND,"Registering rendezvous names\n");
    /* Register main service */
    mdns_register(servername, "_http._tcp", ws_config.port, txtrecord);
    /* Register plugin services */
    plugin_rend_register(servername, ws_config.port, txtrecord);

    free(servername);

    end_time=(int) time(NULL);

    err=db_get_song_count(&perr,&song_count);
    if(err != DB_E_SUCCESS) {
        DPRINTF(E_FATAL,L_MISC,"Error getting song count: %s\n",perr);
    }

    DPRINTF(E_LOG,L_MAIN,"Serving %d songs.  Startup complete in %d seconds\n",
            song_count,end_time-start_time);

    if(conf_get_int("general","rescan_interval",0) && (!reload) &&
       (!conf_get_int("scanning","skip_first",0)))
        config.reload = 1; /* force a reload on start */

    /* Set up signal fd */
    sigfd = signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0)
      {
	DPRINTF(E_FATAL, L_MAIN, "Could not setup signalfd: %s\n", strerror(errno));

	mdns_deinit();
	exit(EXIT_FAILURE);
      }

    event_set(&sig_event, sigfd, EV_READ, signal_cb, NULL);
    event_base_set(evbase_main, &sig_event);
    event_add(&sig_event, NULL);

    /* Set up main timer */
    evtimer_set(main_timer, mainloop_cb, main_timer);
    event_base_set(evbase_main, main_timer);
    evutil_timerclear(&tv);
    tv.tv_sec = MAIN_SLEEP_INTERVAL;
    evtimer_add(main_timer, &tv);

    /* Run the loop */
    event_base_dispatch(evbase_main);

    DPRINTF(E_LOG,L_MAIN,"Stopping gracefully\n");

    DPRINTF(E_LOG, L_MAIN | L_REND, "mDNS deinit\n");
    mdns_deinit();

#ifdef UPNP
    upnp_deinit();
#endif


    /* Got to find a cleaner way to stop the web server.
     * Closing the fd of the socking accepting doesn't necessarily
     * cause the accept to fail on some libcs.
     *
    DPRINTF(E_LOG,L_MAIN|L_WS,"Stopping web server\n");
    ws_stop(config.server);
    */
    free(web_root);
    conf_close();

    DPRINTF(E_LOG,L_MAIN|L_DB,"Closing database\n");
    db_deinit();

    DPRINTF(E_LOG,L_MAIN,"Done!\n");

    io_deinit();
    return EXIT_SUCCESS;
}

