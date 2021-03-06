#pragma ident "$Id: cmdsan.c,v 1.10 2005/10/11 18:30:20 dechavez Exp $"
/*======================================================================
 *
 * Command interface for SAN-based systems
 *
 *- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * Copyright (c) 1997, 1998 Regents of the University of California.
 * All rights reserved.
 *- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * David Chavez (dec@essw.com)
 * Engineering Services & Software
 * San Diego, CA, USA
 *====================================================================*/
#include "isp_console.h"
#include "util.h"

extern char *Syscode;

/* Client side commands */

#define CMD_HELP       ISP_LAST_COMMAND+1  /* display commands          */
#define CMD_PRIVILEGE  ISP_LAST_COMMAND+2  /* privileged mode           */
#define CMD_REBOOT     ISP_LAST_COMMAND+3  /* reboot ISP                */
#define CMD_SHELL      ISP_LAST_COMMAND+4  /* escape to a shell         */
#define CMD_TAKECHARGE ISP_LAST_COMMAND+5  /* request operator status   */
#define CMD_ALARM      ISP_LAST_COMMAND+7  /* alarm report              */
#define CMD_NRTS       ISP_LAST_COMMAND+10 /* display NRTS status       */
#define CMD_VERSION    ISP_LAST_COMMAND+12 /* display version info      */
#define CMD_BEEP       ISP_LAST_COMMAND+16 /* beep control              */
#define CMD_SHUTUP     ISP_LAST_COMMAND+17 /* beep permanently disabled */
#define CMD_SANLOG     ISP_LAST_COMMAND+18 /* view DAS log file(s)      */
#define CMD_ISPLOG     ISP_LAST_COMMAND+19 /* view ISP log file(s)      */
#define CMD_CLEAR      ISP_LAST_COMMAND+20 /* clear something           */
#define CMD_TELNET     ISP_LAST_COMMAND+21 /* telnet to the SAN         */

struct command_map {
    int  perm;
    char *cmnd;
    char *help;
    int   code;
};

/* Glossary of permissions
 *
 * Priv Conn Host Who
 *   0    0    0    0 =  0 -> allpriv   anytime   anywhere   anybody
 *   0    0    0    1 =  1 -> allpriv   anytime   anywhere   operator
 *   0    0    1    0 =  2 -> allpriv   anytime   localhost  anybody
 *   0    0    1    1 =  3 -> allpriv   anytime   localhost  operator
 *   0    1    0    0 =  4 -> allpriv   connected anywhere   anybody
 *   0    1    0    1 =  5 -> allpriv   connected anywhere   operator
 *   0    1    1    0 =  6 -> allpriv   connected localhost  anybody
 *   0    1    1    1 =  7 -> allpriv   connected localhost  operator
 *   1    0    0    0 =  8 -> impossible state
 *   1    0    0    1 =  9 -> root      anytime   anywhere   operator
 *   1    0    1    0 = 10 -> impossible state
 *   1    0    1    1 = 11 -> root      anytime   localhost  operator
 *   1    1    0    0 = 12 -> impossible state
 *   1    1    0    1 = 13 -> root      connected anywhere   operator
 *   1    1    1    0 = 14 -> impossible state
 *   1    1    1    1 = 15 -> root      connected localhost  operator
 */ 

static struct command_map mapWtape[] = { 
{ 7, "ct",         "change tape (flush+eject)",         ISP_MEDIA_CHANGE },
{ 5, "fb",         "flush output buffer to tape",       ISP_MEDIA_FLUSH  },
{ 4, "va",         "view alarm details",                CMD_ALARM        },
{ 4, "vp",         "view run parameters",               ISP_PARAM        },
{ 4, "op",         "assume operator status",            CMD_TAKECHARGE   },
{ 4, "oper",       "",                                  CMD_TAKECHARGE   },
{ 5, "flushlog",   "flush buffered SAN log messages",   ISP_DAS_FLUSHLOG },
{ 1, "reboot",     "reboot [san | isp]",                CMD_REBOOT       },
{ 2, "nrts",       "",                                  CMD_NRTS         },
{ 2, "mon",        "",                                  CMD_NRTS         },
{ 2, "daslog",     "",                                  CMD_SANLOG       },
{ 2, "sanlog",     "view SAN log messages",             CMD_SANLOG       },
{ 2, "isplog",     "view ISP log messages",             CMD_ISPLOG       },
{ 5, "telnet",     "telnet to SAN",                     CMD_TELNET       },
{ 3, "shutdown",   "shutdown ISP",                      ISP_HOST_SHUTDOWN},
{ 4, "abort",      "abort ISP reboot/shutdown request", ISP_ABORT_BOOTOP },
{ 0, "quit",       "end program",                       ISP_DISCONNECT   },
{ 0, "q",          "",                                  ISP_DISCONNECT   },
/* Undocumented commands */
{ 7, "eject",      "",                                  ISP_MEDIA_EJECT  },
{ 5, "dbgbaro",    "",                                  ISP_DEBUG_BARO   },
{ 5, "dbgdpm",     "",                                  ISP_DEBUG_DPM    },
{ 5, "dbgclock",   "",                                  ISP_DEBUG_CLOCK  },
{ 5, "clear",      "",                                  CMD_CLEAR        },
{ 2, "shell",      "",                                  CMD_SHELL        },
{ 0, "help",       "",                                  CMD_HELP         },
{ 0, "?",          "",                                  CMD_HELP         },
{ 0, "ver",        "",                                  CMD_VERSION      },
{ 0, "beep",       "",                                  CMD_BEEP         },
{ 0, "shutup",     "",                                  CMD_SHUTUP       },
{ 4, "nop",        "",                                  ISP_NOP          },
{ 5, "append",     "",                                ISP_MEDIA_APPEND   },
{ 5, "ERASE",      "",                                ISP_MEDIA_OVERWRITE},
/* End of list marker */
{-1, "",           "",                                  0                }
};

static struct command_map mapWcd[] = { 
{ 7, "cd",         "copy ISO images to archive CD",     ISP_MEDIA_CHANGE },
{ 7, "bcd",        "",                                  ISP_MEDIA_CHANGE },
{ 5, "fb",         "flush staged files to ISO image",   ISP_MEDIA_FLUSH  },
{ 4, "va",         "view alarm details",                CMD_ALARM        },
{ 4, "vp",         "view run parameters",               ISP_PARAM        },
{ 4, "op",         "assume operator status",            CMD_TAKECHARGE   },
{ 4, "oper",       "",                                  CMD_TAKECHARGE   },
{ 5, "flushlog",   "flush buffered SAN log messages",   ISP_DAS_FLUSHLOG },
{ 1, "reboot",     "reboot [san | isp]",                CMD_REBOOT       },
{ 2, "nrts",       "",                                  CMD_NRTS         },
{ 2, "mon",        "",                                  CMD_NRTS         },
{ 2, "daslog",     "",                                  CMD_SANLOG       },
{ 2, "sanlog",     "view SAN log messages",             CMD_SANLOG       },
{ 2, "isplog",     "view ISP log messages",             CMD_ISPLOG       },
{ 5, "telnet",     "telnet to SAN",                     CMD_TELNET       },
{ 3, "shutdown",   "shutdown ISP",                      ISP_HOST_SHUTDOWN},
{ 4, "abort",      "abort ISP reboot/shutdown request", ISP_ABORT_BOOTOP },
{ 0, "quit",       "end program",                       ISP_DISCONNECT   },
{ 0, "q",          "",                                  ISP_DISCONNECT   },
/* Undocumented commands */
{ 4, "update",     "",                               ISP_UPDATE_CDR_STATS},
{ 7, "eject",      "",                                  ISP_MEDIA_EJECT  },
{ 5, "dbgbaro",    "",                                  ISP_DEBUG_BARO   },
{ 5, "dbgdpm",     "",                                  ISP_DEBUG_DPM    },
{ 5, "dbgclock",   "",                                  ISP_DEBUG_CLOCK  },
{ 5, "clear",      "",                                  CMD_CLEAR        },
{ 2, "shell",      "",                                  CMD_SHELL        },
{ 0, "help",       "",                                  CMD_HELP         },
{ 0, "?",          "",                                  CMD_HELP         },
{ 0, "ver",        "",                                  CMD_VERSION      },
{ 0, "beep",       "",                                  CMD_BEEP         },
{ 0, "shutup",     "",                                  CMD_SHUTUP       },
{ 4, "nop",        "",                                  ISP_NOP          },
{ 5, "append",     "",                                ISP_MEDIA_APPEND   },
{ 5, "ERASE",      "",                                ISP_MEDIA_OVERWRITE},
/* End of list marker */
{-1, "",           "",                                  0                }
};

static struct command_map mapWnone[] = { 
{ 4, "va",         "view alarm details",                CMD_ALARM        },
{ 4, "vp",         "view run parameters",               ISP_PARAM        },
{ 4, "op",         "assume operator status",            CMD_TAKECHARGE   },
{ 4, "oper",       "",                                  CMD_TAKECHARGE   },
{ 5, "flushlog",   "flush buffered SAN log messages",   ISP_DAS_FLUSHLOG },
{ 1, "reboot",     "reboot [san | isp]",                CMD_REBOOT       },
{ 2, "nrts",       "",                                  CMD_NRTS         },
{ 2, "mon",        "",                                  CMD_NRTS         },
{ 2, "daslog",     "",                                  CMD_SANLOG       },
{ 2, "sanlog",     "view SAN log messages",             CMD_SANLOG       },
{ 2, "isplog",     "view ISP log messages",             CMD_ISPLOG       },
{ 5, "telnet",     "telnet to SAN",                     CMD_TELNET       },
{ 3, "shutdown",   "shutdown ISP",                      ISP_HOST_SHUTDOWN},
{ 4, "abort",      "abort ISP reboot/shutdown request", ISP_ABORT_BOOTOP },
{ 0, "quit",       "end program",                       ISP_DISCONNECT   },
{ 0, "q",          "",                                  ISP_DISCONNECT   },
/* Undocumented commands */
{ 7, "eject",      "",                                  ISP_MEDIA_EJECT  },
{ 5, "dbgbaro",    "",                                  ISP_DEBUG_BARO   },
{ 5, "dbgdpm",     "",                                  ISP_DEBUG_DPM    },
{ 5, "dbgclock",   "",                                  ISP_DEBUG_CLOCK  },
{ 5, "clear",      "",                                  CMD_CLEAR        },
{ 2, "shell",      "",                                  CMD_SHELL        },
{ 0, "help",       "",                                  CMD_HELP         },
{ 0, "?",          "",                                  CMD_HELP         },
{ 0, "ver",        "",                                  CMD_VERSION      },
{ 0, "beep",       "",                                  CMD_BEEP         },
{ 0, "shutup",     "",                                  CMD_SHUTUP       },
{ 4, "nop",        "",                                  ISP_NOP          },
{ 5, "append",     "",                                ISP_MEDIA_APPEND   },
{ 5, "ERASE",      "",                                ISP_MEDIA_OVERWRITE},
/* End of list marker */
{-1, "",           "",                                  0                }
};

#ifndef TELNET
#define TELNET "/usr/bin/telnet"
#endif

static struct command_map *GetMap()
{
    switch (OutputMediaType()) {
      case ISP_OUTPUT_TAPE: return mapWtape;
      case ISP_OUTPUT_CDROM: return mapWcd;
      case ISP_OUTPUT_NONE: return mapWnone;
      default: return mapWnone;
    }
}

static void TelnetToSan()
{
struct isp_params param;
static char command[MAXPATHLEN+32];

    if (GetParams(&param, 0) != 0) return;

    sprintf(command, "%s %s 1023", TELNET, param.san.addr);
    system(command);
}

static void ListCommands(WINDOW *sohwin)
{
int i, y, x;
struct command_map *map;

    map = GetMap();

    wmove(sohwin, 1, 0);
    wclrtobot(sohwin);
    y = 2;
    for (i = 0; map[i].perm >= 0; i++) {
        if (strlen(map[i].help) > 0 && HavePerm(map[i].perm)) {
            wmove(sohwin, y, 0);
            wprintw(sohwin, "%10s - %s\n", map[i].cmnd, map[i].help);
            y++;
        }
    }
    wrefresh(sohwin);
    WaitToClear();
}

static int CheckInput(char *test)
{
int i;
struct command_map *map;

    map = GetMap();

    for (i = 0; map[i].perm >= 0; i++) {
        if (strcasecmp(test, map[i].cmnd) == 0) {
            return HavePerm(map[i].perm) ? map[i].code : ISP_NOPERM;
        }
    }
    return ISP_ERROR;
}

#define MAXTOKEN 2

void SanProcess(char *input, WINDOW *sohwin, WINDOW *cmdwin)
{
pid_t ispd;
int command, ntoken, certain;
struct isp_dascal dascal;
struct isp_dascnf dascnf;
struct isp_status status;
char *token[MAXTOKEN];
long logdate;
BOOL verified;
static char cmdbuf[128];
static char *fid = "SanProcess";

    ntoken = util_sparse(input, token, " ,\n;", MAXTOKEN);
    if (ntoken > 0) {
        command = CheckInput(token[0]);
        switch (command) {

          case ISP_NOPERM:
            beep();
            break;

          case ISP_HOST_SHUTDOWN:
            if (Confirm() && Reconfirm()) {
                if (Connected()) {
                    if (RemoteCommand(command) != ISP_OK) Reconnect(1);
                } else {
                    isp_bootmgr(0);
                }
            }
            break;

          case CMD_NRTS:
            if (ntoken == 1) {
                SetWantNrts(1);
                InitNrts();
            } else if (ntoken == 2 && Operator()) {
                if (strcasecmp(token[1], "start") == 0) {
                    if (RemoteCommand(ISP_NRTS_START) != ISP_OK) Reconnect(2);
                } else if (strcasecmp(token[1], "stop") == 0) {
                    if (RemoteCommand(ISP_NRTS_STOP) != ISP_OK) Reconnect(2);
                }
            }
            break;

          case CMD_SANLOG:
          case CMD_ISPLOG:
            optarg = (ntoken == 1) ? (char *) NULL : token[1];
            clear();
            refresh();
            EndWin();
            reset_shell_mode();
            napms(250);
            ViewLog(command == CMD_SANLOG ? DASLOG : ISPLOG, optarg);
            InitDisplay();
            break;

          case CMD_TELNET:
            clear();
            refresh();
            EndWin();
            reset_shell_mode();
            napms(250);
            TelnetToSan();
            InitDisplay();
            break;

          case CMD_PRIVILEGE:
            SetPrivilege(1); Prompt();
                while (TakeInput(cmdwin) == 0);
            SetPrivilege(0); Prompt();
            break;

          case CMD_SHELL:
            EndWin();
            system(SHELL);
            InitDisplay();
            break;

          case CMD_VERSION:
            ShowVersion();
            break;

          case CMD_HELP:
            ListCommands(sohwin);
            break;

          case CMD_REBOOT:
            certain  = (strcmp(token[0], "REBOOT") == 0);
            RebootDigitizer(token, ntoken, sohwin, certain);
            break;

          case CMD_ALARM:
            NeedAlarms(1);
            break;

          case CMD_CLEAR:
            ClearStats(token, ntoken);
            break;

          case CMD_BEEP:
            if (ntoken == 2) {
                if (strcasecmp(token[1], "on") == 0) {
                    SoundBeep(BEEP_ENABLE);
                } else if (strcasecmp(token[1], "off") == 0) {
                    SoundBeep(BEEP_DISABLE);
                }
            } else {
                SoundBeep(BEEP_ENABLE);
            }
            break;

          case CMD_SHUTUP:
            SoundBeep(BEEP_SHUTUP);
            break;

          case ISP_MEDIA_OVERWRITE:
            if (strcmp(token[0], "ERASE") != 0) break;
          case ISP_MEDIA_EJECT:
          case ISP_MEDIA_APPEND:
            if (RemoteCommand(command) != ISP_OK) Reconnect(2);
            break;

          case ISP_MEDIA_CHANGE:
            if (OutputMediaType() == ISP_OUTPUT_CDROM) {
                ClearBurnFlag();
                clear();
                refresh();
                EndWin();
                reset_shell_mode();
                napms(250);
                sprintf(cmdbuf, "%s %s", ISP_BURN_CD_COMMAND, util_lcase(Syscode));
                system(cmdbuf);
                InitDisplay();
                if (SuccessfulBurn() && RemoteCommand(command) != ISP_OK) Reconnect(2);
            } else {
                if (Confirm() && RemoteCommand(command) != ISP_OK) Reconnect(2);
            }
            break;

          case ISP_MEDIA_FLUSH:
            if (OutputMediaType() == ISP_OUTPUT_CDROM) {
                verified = Confirm();
            } else {
                verified = TRUE;
            }
            if (verified && RemoteCommand(command) != ISP_OK) Reconnect(2);
            break;

          case ISP_NOP:
          case ISP_DAS_FLUSHLOG:
          case ISP_ABORT_BOOTOP:
          case ISP_UPDATE_CDR_STATS:
          case ISP_DEBUG_BARO:
          case ISP_DEBUG_DPM:
          case ISP_DEBUG_CLOCK:
            if (RemoteCommand(command) != ISP_OK) Reconnect(3);
            break;

          case ISP_PARAM:
            if (RemoteCommand(command) != ISP_OK) Reconnect(6);
            NeedParams(1);
            break;

          case ISP_DISCONNECT:
            Quit(0);
            break;

          case CMD_TAKECHARGE:
            TakeCharge();
            break;

          case ISP_ERROR:
          default:
            beep();
            break;
        }
    } else {
        IncrementCrntNdx();
    }
}

/* Revision History
 *
 * $Log: cmdsan.c,v $
 * Revision 1.10  2005/10/11 18:30:20  dechavez
 * support for improved burn script
 *
 * Revision 1.9  2005/08/26 20:19:13  dechavez
 * added "mon" alias for "nrts" command
 *
 * Revision 1.8  2003/10/02 20:24:51  dechavez
 * 2.3.1 mods (fixed console hang on exit, improved dumb terminal refresh
 * a bit, added idle timeout)
 *
 * Revision 1.7  2002/09/09 21:52:10  dec
 * added dbgbaro, dbgdpm, and dbgclock commands
 *
 * Revision 1.6  2002/02/22 23:48:51  dec
 * added update (ISP_UPDATE_CDR_STATS) command, supress reconfirm when
 * flushing buffer prematurely
 *
 * Revision 1.5  2001/10/26 23:41:02  dec
 * issue a ISP_MEDIA_CHANGE command to the server when a successful CD burn
 * has been noted, in order to reset the counters and update the last archive
 * time stamp
 *
 * Revision 1.4  2001/10/24 23:20:54  dec
 * added CDROM support (only partial support for DAS systems)
 *
 * Revision 1.3  2001/05/20 17:44:44  dec
 * Release 2.2 (switch to platform.h MUTEX, SEMAPHORE, THREAD macros)
 *
 * Revision 1.2  2000/11/02 20:30:43  dec
 * Production Release 2.0 (beta 4)
 *
 * Revision 1.1  2000/10/19 22:24:52  dec
 * checkpoint build (development release 2.0.(5), build 7)
 *
 */
