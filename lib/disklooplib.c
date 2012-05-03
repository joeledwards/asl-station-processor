/*
File:       diskloop.c
Copyright:  (C) 2007 by Albuquerque Seismological Laboratory
Author:     Frank Shelly
Purpose:    Routines for accessing the diskloop software
Routines:
  Configuration file routines:
    ParseDiskLoopConfig
            Reads the configuration file.  Stores the values in static local
            variables so that callback routines can make calls to retrieve
            them.
    LoopRecordSize
            Returns how many bytes in each disk loop record
    NumChanRecords
            Returns the number of records for the given channel
    NumLogRecords
            Returns the number of records for the LOG file
    NetBufferSize
            Returns the number of bytes to buffer for netserver
    LoopDirectory
            Returns the directory string where archive files are stored
    LogServerPort
            Returns the port where driver sends updates to
    LogSNCL
            Returns Station, Network, Channel Location to use for log messages
            NOTE that some programs will want to use a different channel
    SetLogSNCL
            Can be used to change the defaults returned by LogSNCL
    WriteChan
            Writes a record of SEED data to the diskloop
    GetRecordRange
            Given a start and stop time, returns first and last index of
            records that fall within that range
    DumpSpans
            Debug type print of all the spans in the circular buffer
    NoArchive
            Don't archive records
    NoIDA
            Don't add records to the IDA diskloop
    RemapStationName
            Used to implement diskloop.config MapStation: keyword
            Changes seed header station name if a MapStation match is found

============================================================
=== NEW low-level logic wrappers, which will replace the ===
=== internal logic of the above functions.               ===

    adl_open
    adl_write
    adl_flush
    adl_read
    adl_close

    adl_oldest_index
    adl_newest_index

    adl_index_before
    adl_index_after
    adl_range_indices


Update History:
yyyy-mm-dd WHO - Changes
==============================================================================
2007-01-25 FCS - Creation
2007-05-02 FCS - Eliminate seed record time overlap caused by restarts
2007-06-01 FCS - Add sequence number and samples return values to ParseSeedHeader
2007-06-01 FCS - Use sequence number and samples for overwrite last record
2009-10-22 FCS - Support for new Falcon configuration keywords
2010-04-01 FCS - Add NoIDA keyword supporting channels for archive only
2010-09-14 FCS - Add RemapStationName routine
2011-08-11 JDE - Add NoArchive keyword supporting channels for IDA diskloop only
2012-05-02 JDE - Switched to new high-performance system for handling diskloops
******************************************************************************/

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <arpa/inet.h>    // Needed for ntohl,ntohs 
#include <sys/time.h>
#include "include/diskloop.h"
#include "include/dcc_time_proto2.h"
#include "include/map.h"
#include "include/netreq.h"

/*
First are the tools for getting data from the configuration file
These routines return NULL if everything is okay, or a pointer to
an error message string.

Since these routines can be called by callback functions, everything
is localy stored in static variables.
*/

// faster version of isalnum()
#define ALNUM(c) (((c > 47) && (c < 58)) || ((c > 64) && (c < 91)) || ((c > 96) && (c < 123)))
// timeval to 64-bit time in microseconds
#define TIME_MICRO(tv) ((tv.tv_sec * 1000000LL) + tv.tv_usec)


// diskloop file flush frequency (1 min.)
struct timeval flush_interval = {.tv_sec =  60, .tv_usec = 0};
// index file write frequency (5 min.)
struct timeval index_interval = {.tv_sec = 300, .tv_usec = 0};

// Need to know when/if the file has been successfully parsed
// 0 = not parsed
// 1 = parsed with no errors
// -1 = parsed with errors
static int parse_state=0;

// Map which stores a context for each open ASL diskloop
static Map *pDiskLoops=NULL;

// Will get filled in by Buffer: entries in configuration file
static struct s_bufconfig *pChanSizeList=NULL;
static Map *pChanSizeShortcut=NULL;

// Will get filled in by NoArchive: entries in configuration file
static struct s_bufconfig *pNoArchiveList=NULL;
static Map *pNoArchiveShortcut=NULL;

// Will get filled in by NoIDA: entries in configuration file
static struct s_bufconfig *pNoIDAList=NULL;
static Map *pNoIDAShortcut=NULL;

// Need a list of remapped station names
static struct s_mapstation *pMapStationList=NULL;
static Map *pMapStationShortcut=NULL;

// Populated durion operation. Override settings in NoIDA and 
// NoArchive if matching entries exist. These can be modified
// while the program is running.
static Map *pChannelToArchive=NULL;
static Map *pChannelToIDA=NULL;
static int CHANNEL_NF  = -1;
static int CHANNEL_OFF = 0;
static int CHANNEL_ON  = 1;

// Should either be 512 or 4096 to match SEED record size
static int iLoopRecordSize=512;

// What port to send channel set commands to
static int iChannelSetPort=9999;

// What port to send update messages to
static int iLogServerPort=8888;

// How many bytes the netserver buffer should have
static int iNetBufferSize=401408;

static int iLogBufferDepth=1000;

// Where the archive data files are stored
static char loopDir[MAXCONFIGLINELEN+2];

static char looperrstr[MAXCONFIGLINELEN+2];

// Default Station, Network, Channel, Location to use for log messages
static char log_station[8] = "ZZZZ";
static char log_network[4] = "XX";
static char log_channel[4] = "LOG";
static char log_location[4] = "90";

// Username and Password option for Falcon
// If character pointers are null then use station name
static char *falcon_username=NULL;
static char *falcon_password=NULL;

// Falcon IP and port number
static char falcon_IP[MAXCONFIGLINELEN+2] = "0.0.0.0";
static int  falcon_port    = 5080;


//////////////////////////////////////////////////////////////////////////////
// Low-level logic functions

// Open diskloop
char *adl_open(
        diskloop_context_t **return_context,
        const char *station,
        const char *location,
        const char *channel
    );

// Write a record to the diskloop (optional index used if zero or greater)
char *adl_write(
        diskloop_context_t *context,
        int index,
        char *record
    );

// Flush diskloop
char *adl_flush(
        diskloop_context_t *context
    );

// Update the index file
char *adl_write_index(
        diskloop_context_t *context
    );

// Read a record from the diskloop (optional index used if zero or greater)
char *adl_read(
        diskloop_context_t *context,
        int index,
        char *record
    );

// Close diskloop
char *adl_close(
        diskloop_context_t **context
    );

// Close all open diskloops
char *adl_close_all();

// Get the index of the oldest record
char *adl_oldest_index(
        diskloop_context_t *context,
        int *index
    );

// Get the index of the newest record
char *adl_newest_index(
        diskloop_context_t *context,
        int *index
    );

// Get the index of the record starting on or before this time
char *adl_index_before(
        diskloop_context_t *context,
        STDTIME2  *time,
        int inclusive,
        int *index
    );

// Get the index of the record starting on or after this time
char *adl_index_after(
        diskloop_context_t *context,
        STDTIME2  *time,
        int inclusive,
        int *index
    );

// Get the index of records containing the sandwiched times
char *adl_range_indices(
        diskloop_context_t *context,
        STDTIME2  *start_time,
        STDTIME2  *end_time,
        int inclusive,
        int *start_index,
        int *end_index
    );

// Get the index of the record based on its relationship to the supplied time
char *adl_locate_index(
        diskloop_context_t *context,
        STDTIME2 *time,
        int whence,
        int *index
    );

// Get the index of the oldest record
char *adl_oldest_index(
        diskloop_context_t *context,
        int *index
    );

// Get the index of the newest record
char *adl_newest_index(
        diskloop_context_t *context,
        int *index
    );


//////////////////////////////////////////////////////////////////////////////
// Converts a string to upper case
char *strToUpper(char *s)
{
    size_t i;
    size_t len = strlen(s);
    for (i=0; i < len; i++) {
        s[i] = toupper(s[i]);
    }
    return s;
}

//////////////////////////////////////////////////////////////////////////////
// Makes a canonical channel key
// Ensures that all keys are created in the same way
// format: SSSS-LL-CCC
void MakeKey(
        const char *station,  // station name
        const char *location, // location code
        const char *channel,  // channel name
        char *key // 16-bytes allocated minimum for resulting key
        )
{
    char *p = key;
    const char *t;

    // For each component, add characters until we hit a non-alpha-numeric
    // character, or null termination

    // strip a leading network identifier (if any exists)
    t = station;
    while (ALNUM(*t)) { t++; }
    if (*t == 0) {
        // the first non alpha-numeric character was a null termination,
        // so there was no leading network
        t = station;
    } else {
        // otherwise skip the non-alpha-numeric character
        t++;
    }

    // Station
    while (ALNUM(*t)) {
        *p = toupper(*t); p++;
        t++;
    }

    *p = '-'; p++;

    t = location;
    // Location
    while (ALNUM(*t)) {
        *p = toupper(*t); p++;
        t++;
    }

    *p = '-'; p++;

    t = channel;
    // Channel
    while (ALNUM(*t)) {
        *p = toupper(*t); p++;
        t++;
    }

    *p = 0; // Null terminate
}

//////////////////////////////////////////////////////////////////////////////
// Reads the configuration file.  Stores the values in static local
// variables so that callback routines can make calls to retrieve
// them.
char *ParseDiskLoopConfig(
        const char *filename    // Name of the configuration file
        )                       // returns NULL or an error string pointer
{
    FILE  *fp;
    char  linestr[MAXCONFIGLINELEN+2];
    char  argstr[MAXCONFIGLINELEN+2];
    char  argstr2[MAXCONFIGLINELEN+2];
    char  station[MAXCONFIGLINELEN+2];
    char  chan[MAXCONFIGLINELEN+2];
    char  loc[MAXCONFIGLINELEN+2];
    int   bOK=1;
    int   bParsed=1;
    int   iLineNum;
    int   iArgs;
    int   count;
    int   i;
    int   iMaxLoopSize;
    int   parseType = PARSE_NONE;
    struct s_bufconfig *newbuf;
    struct s_mapstation *newmap;

    pDiskLoops         = map_new(256, NULL, NULL);
    pChanSizeShortcut  = map_new(128, NULL, NULL);
    pNoArchiveShortcut = map_new(128, NULL, NULL);
    pNoIDAShortcut     = map_new(128, NULL, NULL);
    pChannelToArchive  = map_new(128, NULL, NULL);
    pChannelToIDA      = map_new(128, NULL, NULL);

    // Open the configuration file
    if ((fp = fopen(filename, "r")) == NULL)
    {
        sprintf(looperrstr, "ParseDiskLoopConfig: Failed to open %s", filename);
        return looperrstr;
    } // error opening file

    // Loop through all lines in the file
    iLineNum=0;
    while (fgets(linestr, MAXCONFIGLINELEN+1, fp) != NULL)
    {
        bParsed = 0;
        iLineNum++;

        // Check for comment
        if (linestr[0] == '#')
        {
            bParsed = 1;
            continue;
        }

        // Blank line
        if (linestr[0] == '\n' || linestr[0] == 0)
        {
            bParsed = 1;
            continue;
        }

        if ((iArgs=sscanf(linestr, "Directory: %s", argstr)) == 1)
        {
            strcpy(loopDir, argstr);
            bParsed = 1;
            continue;
        }

        if ((iArgs=sscanf(linestr, "Record Size: %d", &count)) == 1)
        {
            iLoopRecordSize = count;
            bParsed = 1;
            continue;
        }

        //    Buffer: [[station]-][loc]/<chan> size
        //     NoIDA: [[station]-][loc]/<chan>
        // NoArchive: [[station]-][loc]/<chan>
        station[0] = 0;
        loc[0] = 0;
        chan[0] = 0;
        parseType = PARSE_NONE;
        if ((iArgs=sscanf(linestr, "Buffer: %s %d", argstr, &count)) == 2) {
            parseType = PARSE_BUFFER;
        }
        else if ((iArgs=sscanf(linestr, "NoArchive: %s", argstr)) == 1) {
            parseType = PARSE_NO_ARCHIVE;
        }
        else if ((iArgs=sscanf(linestr, "NoIDA: %s", argstr)) == 1) {
            parseType = PARSE_NO_IDA;
        }
        if (parseType != PARSE_NONE) {
            int i=0;
            int locStart = 0;
            int stFound = 0;
            // check for station name
            if (argstr[0] == '-')
            {
                station[0] = 0;
                locStart = 1;
            } 
            else
            {
                for (i=0; i < 6; i++) {
                    if (argstr[i] == '-') 
                    {
                        stFound = 1;
                        locStart = i+1;
                        station[i] = 0;
                        i=6;
                    }
                    else if (argstr[i] == 0) 
                    {
                        i=6;
                    }
                    else 
                    {
                        station[i] = argstr[i];
                    }
                }
                if (!stFound) {
                    station[0] = 0;
                }
            }
            // test for blank location code
            if (argstr[locStart] == '/')
            {
                strcpy(loc, "  ");
                for (i=locStart; i < (locStart+3); i++)
                    chan[i] = argstr[i+1];
            }
            else if (argstr[locStart+2] == '/')
            {
                loc[0] = argstr[locStart+0];
                loc[1] = argstr[locStart+1];
                loc[2] = 0;

                for (i=0; i < (3); i++)
                    chan[i] = argstr[locStart+3+i];
                chan[i] = 0;
            } // else location code is not blank
            else argstr[locStart] = 0;


            if (argstr[locStart] != 0)
            {
                /*
                   fprintf(stderr, "%s: %5.5s-%2.2s-%3.3s\n", 
                   parseType == PARSE_BUFFER     ? "Buffer" :
                   parseType == PARSE_NO_ARCHIVE ? "NoArchive" :
                   parseType == PARSE_NO_IDA     ? "NoIDA" : "None",
                   station, loc, chan);
                // */
                if ((parseType == PARSE_BUFFER) && (count < 4))
                {
                    sprintf(looperrstr, "Count %d < minimum of 4, Line %d in %s",
                            count, iLineNum, filename);
                    fprintf(stderr, "%s\n%s\n", looperrstr, linestr);
                }
                else
                {
                    // allocate space for new entry
                    newbuf = (struct s_bufconfig *) malloc(sizeof (struct s_bufconfig));
                    if (newbuf == NULL)
                    {
                        fprintf(stderr, "%s(%d): malloc in ParseDiskLoopConfig failed.\n",
                                __FILE__, __LINE__);
                        exit(1);
                    }

                    // Fill  in record
                    strncpy(newbuf->station, station, 8);
                    newbuf->station[7] = 0;
                    strncpy(newbuf->loc, loc, 4);
                    newbuf->loc[3] = 0;
                    strncpy(newbuf->chan, chan, 4);
                    newbuf->chan[3] = 0;
                    if (parseType == PARSE_BUFFER) 
                    {
                        newbuf->records = count;
                    } 
                    else 
                    {
                        newbuf->records = 0;
                    }

                    // Insert new record at head of the list
                    switch (parseType) {
                        case PARSE_BUFFER:
                            newbuf->next = pChanSizeList;
                            pChanSizeList = newbuf;
                            break;
                        case PARSE_NO_ARCHIVE:
                            newbuf->next = pNoArchiveList;
                            pNoArchiveList = newbuf;
                            break;
                        case PARSE_NO_IDA:
                            newbuf->next = pNoIDAList;
                            pNoIDAList = newbuf;
                            break;
                    }

                    bParsed = 1;
                    continue;
                } // else everything appears to be in order
            } // location/channel syntax appears to be okay
        }

        /* Replaced by cleaner approach above.
        // Buffer: [loc]/<chan> size
        loc[0] = 0;
        chan[0] = 0;
        if ((iArgs=sscanf(linestr, "Buffer: %s %d", argstr, &count)) == 2)
        {
        // test for blank location code
        int i=0;
        if (argstr[0] == '/')
        {
        strcpy(loc, "  ");
        for (i=0; i < 3; i++)
        chan[i] = argstr[i+1];
        }
        else if (argstr[2] == '/')
        {
        loc[0] = argstr[0];
        loc[1] = argstr[1];
        loc[2] = 0;

        for (i=0; i < 3; i++)
        chan[i] = argstr[i+3];
        chan[i] = 0;
        } // else location code is not blank
        else argstr[0] = 0;

        if (argstr[0] != 0)
        {
        if (count < 4)
        {
        sprintf(looperrstr, "Count %d < minimum of 4, Line %d in %s",
        count, iLineNum, filename);
        fprintf(stderr, "%s\n%s\n", looperrstr, linestr);
        }
        else
        {
        // allocate space for new entry
        newbuf = (struct s_bufconfig *) malloc(sizeof (struct s_bufconfig));
        if (newbuf == NULL)
        {
        fprintf(stderr, "%s(%d): malloc in ParseDiskLoopConfig failed.\n",
        __FILE__, __LINE__);
        exit(1);
        }

        // Fill  in record
        strncpy(newbuf->loc, loc, 4);
        newbuf->loc[3] = 0;
        strncpy(newbuf->chan, chan, 4);
        newbuf->chan[3] = 0;
        newbuf->records = count;

        // Insert new record at head of the list
        newbuf->next = pChanSizeList;
        pChanSizeList = newbuf;

        bParsed = 1;
        continue;
        } // else everything appears to be in order
        } // location/channel syntax appears to be okay
        } // Buffer: keyword parsed

        // NoArchive: [loc]/<chan>
        loc[0] = 0;
        chan[0] = 0;
        if ((iArgs=sscanf(linestr, "NoArchive: %s", argstr)) == 1)
        {
        // test for blank location code
        int i=0;
        if (argstr[0] == '/')
        {
        strcpy(loc, "  ");
        for (i=0; i < 3; i++)
            chan[i] = argstr[i+1];
    }
        else if (argstr[2] == '/')
        {
            loc[0] = argstr[0];
            loc[1] = argstr[1];
            loc[2] = 0;

            for (i=0; i < 3; i++)
                chan[i] = argstr[i+3];
            chan[i] = 0;
        } // else location code is not blank
        else argstr[0] = 0;

        if (argstr[0] != 0)
        {
            // allocate space for new entry
            newbuf = (struct s_bufconfig *) malloc(sizeof (struct s_bufconfig));
            if (newbuf == NULL)
            {
                fprintf(stderr, "%s(%d): malloc in ParseDiskLoopConfig failed.\n",
                        __FILE__, __LINE__);
                exit(1);
            }

            // Fill  in record
            strncpy(newbuf->loc, loc, 4);
            newbuf->loc[3] = 0;
            strncpy(newbuf->chan, chan, 4);
            newbuf->chan[3] = 0;
            newbuf->records = 0;

            // Insert new record at head of the list
            newbuf->next = pNoArchiveList;
            pNoArchiveList = newbuf;

            bParsed = 1;
            continue;
        } // location/channel syntax appears to be okay
    } // NoArchive: keyword parsed

    // NoIDA: [loc]/<chan>
    loc[0] = 0;
    chan[0] = 0;
    if ((iArgs=sscanf(linestr, "NoIDA: %s", argstr)) == 1)
    {
        // test for blank location code
        int i=0;
        if (argstr[0] == '/')
        {
            strcpy(loc, "  ");
            for (i=0; i < 3; i++)
                chan[i] = argstr[i+1];
        }
        else if (argstr[2] == '/')
        {
            loc[0] = argstr[0];
            loc[1] = argstr[1];
            loc[2] = 0;

            for (i=0; i < 3; i++)
                chan[i] = argstr[i+3];
            chan[i] = 0;
        } // else location code is not blank
        else argstr[0] = 0;

        if (argstr[0] != 0)
        {
            // allocate space for new entry
            newbuf = (struct s_bufconfig *) malloc(sizeof (struct s_bufconfig));
            if (newbuf == NULL)
            {
                fprintf(stderr, "%s(%d): malloc in ParseDiskLoopConfig failed.\n",
                        __FILE__, __LINE__);
                exit(1);
            }

            // Fill  in record
            strncpy(newbuf->loc, loc, 4);
            newbuf->loc[3] = 0;
            strncpy(newbuf->chan, chan, 4);
            newbuf->chan[3] = 0;
            newbuf->records = 0;

            // Insert new record at head of the list
            newbuf->next = pNoIDAList;
            pNoIDAList = newbuf;

            bParsed = 1;
            continue;
        } // location/channel syntax appears to be okay
    } // NoIDA: keyword parsed
    */

        if ((iArgs=sscanf(linestr, "MapStation: %s %s\n", argstr, argstr2)) == 2)
        {
            // Verify station name lengths
            count = strlen(argstr);
            if (count < 1 || count > 5)
            {
                sprintf(looperrstr,
                        "Invalid station name 1 length (%d not 1..5), Line %d in %s",
                        count, iLineNum, filename);
                fprintf(stderr, "%s\n%s\n", looperrstr, linestr);
                bOK = 0;
                continue;
            }

            count = strlen(argstr2);
            if (count < 1 || count > 5)
            {
                sprintf(looperrstr,
                        "Invalid station name 2 length (%d not 1..5), Line %d in %s",
                        count, iLineNum, filename);
                fprintf(stderr, "%s\n%s\n", looperrstr, linestr);
                bOK = 0;
                continue;
            }

            // Allocate memory for new mapping
            newmap = (struct s_mapstation *) malloc(sizeof (struct s_mapstation));
            if (newmap == NULL)
            {
                fprintf(stderr, "malloc in ParseDiskLoopConfig failed.\n");
                exit(1);
            }

            for (i=0; i < 5 && argstr[i] != 0; i++)
            {
                newmap->station_q330[i] = toupper((int)argstr[i]);
            }
            newmap->station_q330[i] = 0;

            for (i=0; i < 5 && argstr2[i] != 0; i++)
            {
                newmap->station[i] = toupper((int)argstr2[i]);
            }
            newmap->station[i] = 0;

            newmap->next = pMapStationList;
            pMapStationList = newmap;

            bParsed = 1;
            continue;
        } // MapStation: parsed

    if ((iArgs=sscanf(linestr, "LogStation: %s\n", argstr)) == 1)
    {
        strncpy(log_station, argstr, 5);
        for (i=0; i<5 && isalnum((int)argstr[i]); i++)
            ; // find end of station
        log_station[i] = 0;
        bParsed = 1;
        continue;
    } // LogStation: parsed

    if ((iArgs=sscanf(linestr, "LogNetwork: %s\n", argstr)) == 1)
    {
        strncpy(log_network, argstr, 2);
        log_network[2] = 0;
        bParsed = 1;
        continue;
    } // LogNetwork: parsed

    if ((iArgs=sscanf(linestr, "LogChannel: %s\n", argstr)) == 1)
    {
        strncpy(log_channel, argstr, 3);
        log_channel[3] = 0;
        bParsed = 1;
        continue;
    } // LogChannel: parsed

    if ((iArgs=sscanf(linestr, "LogLocation: %s\n", argstr)) == 1)
    {
        strncpy(log_location, argstr, 2);
        log_location[3] = 0;
        bParsed = 1;
        continue;
    } // LogLocation: parsed

    if ((iArgs=sscanf(linestr, "Logbuffer: %d", &count)) == 1)
    {
        iLogBufferDepth = count;
        bParsed = 1;
        continue;
    }

    if ((iArgs=sscanf(linestr, "Netbuffer: %d", &count)) == 1)
    {
        iNetBufferSize = count;
        bParsed = 1;
        continue;
    }

    if ((iArgs=sscanf(linestr, "Channelset Port: %d", &count)) == 1)
    {
        iChannelSetPort = count;
        bParsed = 1;
        continue;
    }

    if ((iArgs=sscanf(linestr, "Logserver Port: %d", &count)) == 1)
    {
        iLogServerPort = count;
        bParsed = 1;
        continue;
    }

    if ((iArgs=sscanf(linestr, "Falcon Username: %s", argstr)) == 1)
    {
        falcon_username = malloc(sizeof(argstr)+1);
        strcpy(falcon_username, argstr);
        bParsed = 1;
        continue;
    }

    if ((iArgs=sscanf(linestr, "Falcon Password: %s", argstr)) == 1)
    {
        falcon_password = malloc(sizeof(argstr)+1);
        strcpy(falcon_password, argstr);
        bParsed = 1;
        continue;
    }

    if ((iArgs=sscanf(linestr, "Falcon IP: %s", argstr)) == 1)
    {
        strcpy(falcon_IP, argstr);
        bParsed = 1;
        continue;
    }

    if ((iArgs=sscanf(linestr, "Falcon Port: %d", &count)) == 1)
    {
        falcon_port = count;
        bParsed = 1;
        continue;
    }

    // If line not recognized then return an error
    if (!bParsed)
    {
        // Only last parse error message will be returned, all will be printed
        sprintf(looperrstr, "Failed to parse line %d in %s",
                iLineNum, filename);
        fprintf(stderr, "%s\n%s\n", looperrstr, linestr);
        bOK = 0;
    } // if parsing errors
    } // while more lines of data to parse

    fclose (fp);

    // Make sure that none of the diskloop depths will exceed 2 GByte
    iMaxLoopSize = (0x7fffffff / iLoopRecordSize) - 2;
    for (newbuf = pChanSizeList; newbuf != NULL; newbuf = newbuf->next)
    {
        if (newbuf->records > iMaxLoopSize)
        {
            sprintf(looperrstr,
                    "Buffer: %s/%s %d -- Size exceeds 2 GByte limit, max %d!",
                    newbuf->loc, newbuf->chan, newbuf->records, iMaxLoopSize);
            fprintf(stderr, "%s\n", looperrstr);
            bOK = 0;
        } // buffer was too large
    } // loop trough all buffer size definitions

    // Set parse state dependent upon any errors detected
    if (bOK)
        parse_state = 1;
    else
        parse_state = -1;

    if (!bOK) return looperrstr;
    return NULL;
} // ParseDiskLoopConfig()

//////////////////////////////////////////////////////////////////////////////
// Returns how many bytes in each disk loop record
char *LoopRecordSize(
        int   *size             // return bytes in each disk loop record
        )                       // returns NULL or an error string pointer
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "LoopRecordSize: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    *size = iLoopRecordSize;
    return NULL;
} // LoopRecordSize()

//////////////////////////////////////////////////////////////////////////////
// Tell whether the given station-location/channel is on the supplied channel list
int CheckChannelList(
        const char          *station,   // Station Name
        const char          *chan,      // Channel ID
        const char          *loc,       // Location ID
        struct s_bufconfig  *ptr,       // List of entries
        struct s_bufconfig  **entry     // Matching entry if found
        )
{
    int bFound = 0;
    int bMatch = 0;
    int i;

    // Loop through list, return last entry to match
    for (; ptr != NULL; ptr = ptr->next)
    {
        bMatch = 1;

        // Check station name for match
        for (i=0; i < 5 && bMatch; i++)
        {
            if (ptr->station[0] == 0)
                break;
            if ((station[i] == 0) && (ptr->station[i] == 0)) {
                break;
            }
            else if ((station[i] == 0) || (ptr->station[i] == 0)) {
                bMatch = 0;
            }
            else if ((toupper(station[i]) != toupper(ptr->station[i])) &&
                    (ptr->station[i] != '?')) {
                bMatch = 0;
            }
        }

        // Check location code for match
        for (i=0; i < 2 && bMatch; i++)
        {
            if (ptr->loc[0] == 0)
                break;
            if ((toupper(loc[i]) != toupper(ptr->loc[i]))
                    && (ptr->loc[i] != '?'))
                bMatch = 0;
        }

        // Check channel name for match
        for (i=0; i < 3 && bMatch; i++)
        {
            if ((toupper(chan[i]) != toupper(ptr->chan[i]))
                    && (ptr->chan[i] != '?'))
                bMatch = 0;
        }

        if (bMatch)
        {
            bFound = 1;
            if (entry != NULL) {
                *entry = ptr;
            }
        }
    } // check all entries

    return bFound;
}


//////////////////////////////////////////////////////////////////////////////
// Add a channel to a bypass map
int SetChannel (
        Map         *map,       // Target map
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc,       // Location ID
        int          send       // Save to archive
        )
{
    char key[20];
    int *value = send ? &CHANNEL_ON : &CHANNEL_OFF;
    int result = *value;

    MakeKey(station, loc, chan, key);
    //fprintf(stderr, "SetChannel(): Adding key='%s' : value=%d\n", key, *value);
    if (!map_put(map, key, value)) {
        result = CHANNEL_NF;
    }
    return result;
}

//////////////////////////////////////////////////////////////////////////////
// Check whether this channel is listed in a bypass map.
int CheckChannel (
        Map         * map,      // Target map
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )
{
    char key[20];
    int *result = NULL;

    // Check keys starting with the most specific, and moving to the least 
    // spcific. This will allow the user to create blanket rules, then poke
    // holes with more specific exceptions.

    // Check station, location & channel
    MakeKey(station, loc, chan, key); // S-L-C
    result = (int *)map_get(map, key);

    if (result == NULL) {
        // check station & channel
        MakeKey(station, "", chan, key); // S--C
        result = (int *)map_get(map, key);
    }

    if (result == NULL) {
        // check station & location
        MakeKey(station, loc, "", key); // S-L-
        result = (int *)map_get(map, key);
    }

    if (result == NULL) {
        // check station
        MakeKey(station, "", "", key); // S--
        result = (int *)map_get(map, key);
    }

    if (result == NULL) {
        // check location & channel
        MakeKey("", loc, chan, key); // -L-C
        result = (int *)map_get(map, key);
    }

    if (result == NULL) {
        // check channel
        MakeKey("", "", chan, key); // --C
        result = (int *)map_get(map, key);
    }

    if (result == NULL) {
        // check location
        MakeKey("", loc, "", key); // -L-
        result = (int *)map_get(map, key);
    }

    if (result == NULL) {
        // check all data
        MakeKey("", "", "", key); // --
        result = (int *)map_get(map, key);
    }

    if (result == NULL) {
        result = &CHANNEL_NF;
    }
    //fprintf(stderr, "CheckChannel(): Checking key='%s', result=%d\n", key, *result);
    return *result;
}

//////////////////////////////////////////////////////////////////////////////
// Remove a channel from a bypass map
int DefaultChannel(
        Map         * map,      // Target map
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )
{
    char key[20];

    MakeKey(station, loc, chan, key);
    //fprintf(stderr, "DefaultChannel(): Removing key='%s'\n", key);
    return map_remove(map, key);
}

//////////////////////////////////////////////////////////////////////////////
// Add a channel to the Archive bypass map
int SetChannelToArchive(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc,       // Location ID
        int          send       // Save to archive
        )
{
    return SetChannel(pChannelToArchive, station, chan, loc, send);
}

//////////////////////////////////////////////////////////////////////////////
// Check whether this channel should be archived.
int CheckChannelToArchive(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )
{
    return CheckChannel(pChannelToArchive, station, chan, loc);
}

//////////////////////////////////////////////////////////////////////////////
// Remove a channel from the Archive bypass map
int DefaultChannelToArchive(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )
{
    return DefaultChannel(pChannelToArchive, station, chan, loc);
}

//////////////////////////////////////////////////////////////////////////////
// Add a channel to the IDA bypass map
int SetChannelToIDA(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc,       // Location ID
        int          send       // Save to IDA diskloop
        )
{
    return SetChannel(pChannelToIDA, station, chan, loc, send);
}

//////////////////////////////////////////////////////////////////////////////
// Check whether this channel should be added to the IDA diskloop.
int CheckChannelToIDA(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )
{
    return CheckChannel(pChannelToIDA, station, chan, loc);
}

//////////////////////////////////////////////////////////////////////////////
// Remove a channel from the IDA bypass map
int DefaultChannelToIDA(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )
{
    return DefaultChannel(pChannelToIDA, station, chan, loc);
}

//////////////////////////////////////////////////////////////////////////////
// Tell whether the given location/channel is on the NoArchive channel list
int CheckNoArchive(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )
{
    int bypass;
    int bFound = 0;
    int *state;
    struct s_bufconfig *entry;
    char key[20];

    if (parse_state != 0)
    {
        // Check if this channel is in the bypass map
        bypass = CheckChannelToArchive(station, chan, loc);
        if (bypass == CHANNEL_NF) {
            // If channel is not in the bypass map, check it against the config
            MakeKey(station, loc, chan, key);
            state = (int *)map_get(pNoArchiveShortcut, key);
            if (state == NULL) {
                //fprintf(stderr, "CheckNoArchive: record not found; performing list lookup\n");
                bFound = CheckChannelList(station, chan, loc, pNoArchiveList, &entry);
                if (bFound) {
                    map_put(pNoArchiveShortcut, key, &CHANNEL_OFF);
                } 
                else {
                    map_put(pNoArchiveShortcut, key, &CHANNEL_ON);
                }
            }
            else { 
                //fprintf(stderr, "CheckNoArchive: found record in shortcut map\n");
                if (*state == CHANNEL_OFF) {
                    bFound = 1;
                }
            }
        }
        else if (bypass == CHANNEL_OFF){
            //fprintf(stderr, "CheckNoArchive: found record in bypass map\n");
            bFound = 1;
        }
    }

    return bFound;
} // CheckNoArchive()

//////////////////////////////////////////////////////////////////////////////
// Tell whether the given location/channel is on the NoIDA channel list
int CheckNoIDA(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )
{
    int bypass;
    int bFound = 0;
    int *state;
    struct s_bufconfig *entry;
    char key[20];

    if (parse_state != 0)
    {
        // Check if this channel is in the bypass map
        bypass = CheckChannelToIDA(station, chan, loc);
        if (bypass == CHANNEL_NF) {
            // If channel is not in the bypass map, check it against the config
            MakeKey(station, loc, chan, key);
            state = (int *)map_get(pNoIDAShortcut, key);
            if (state == NULL) {
                //fprintf(stderr, "CheckNoIDA: record not found; performing list lookup\n");
                bFound = CheckChannelList(station, chan, loc, pNoIDAList, &entry);
                if (bFound) {
                    map_put(pNoIDAShortcut, key, &CHANNEL_OFF);
                } 
                else {
                    map_put(pNoIDAShortcut, key, &CHANNEL_ON);
                }
            }
            else { 
                //fprintf(stderr, "CheckNoIDA: found channel in shortcut map\n");
                if (*state == CHANNEL_OFF) {
                    bFound = 1;
                }
            }
        }
        else if (bypass == CHANNEL_OFF){
            //fprintf(stderr, "CheckNoIDA: found channel in bypass map\n");
            bFound = 1;
        }
    }

    return bFound;
} // CheckNoIDA()

//////////////////////////////////////////////////////////////////////////////
// Returns the number of records for the given channel
char *NumChanRecords(
        const char  *station,   // Station Name
        const char  *chan,      // Channel ID
        const char  *loc,       // Location ID
        int         *records
        )                       // returns NULL or an error string pointer
{
    int bFound = 0;
    struct s_bufconfig *entry;
    char key[20];

    if (parse_state == 0)
    {
        sprintf(looperrstr, "NumChanRecords: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    MakeKey(station, loc, chan, key);
    entry = (struct s_bufconfig *)map_get(pChanSizeShortcut, key);
    if (entry == NULL) {
        fprintf(stderr, "NumChanRecords: record not found in map, performing list lookup\n");
        bFound = CheckChannelList(station, chan, loc, pChanSizeList, &entry);
        if (bFound && (entry != NULL)) {
            map_put(pChanSizeShortcut, key, entry);
        }
    }
    else {
        fprintf(stderr, "NumChanRecords: found record in shortcupt map\n");
        bFound = 1;
    }

    if (bFound && (entry != NULL)) {
        *records = entry->records;
    }

    if (!bFound)
    {
        sprintf(looperrstr, "NumChanRecords: No Buffer: entries matching %s/%s",
                loc, chan);
        return looperrstr;
    }

    return NULL;
} // NumChanRecords()

//////////////////////////////////////////////////////////////////////////////
// Returns the number of LOG records
char *NumLogRecords(
        int *records            // stores how many records disk buffer stores
        )                       // returns NULL or an error string pointer
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "NumLogRecords: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    *records = iLogBufferDepth;
    return NULL;
} // NumLogRecords()

//////////////////////////////////////////////////////////////////////////////
// Returns the directory string where archive files are stored
char *LoopDirectory(
        char *dir               // Should have MAXCONFIGLINELEN+1 chars allocated
        )                       // returns NULL or an error string pointer
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "LoopDirectory: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    strcpy(dir, loopDir);
    return NULL;
} // LoopDirectory()

//////////////////////////////////////////////////////////////////////////////
// Returns the number of bytes to use for the netserver buffer
char *NetBufferSize(
        int *size               // return value for buffer size in bytes
        )
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "NetBufferSize: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    *size = iNetBufferSize;
    return NULL;
} // NetBufferSize()

//////////////////////////////////////////////////////////////////////////////
// Return the Station, Network, Channel, Location to use for log messages
char *LogSNCL(char *station, char *network, char *channel, char *location)
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "LogSNCL: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }
    strcpy(station, log_station);
    strcpy(network, log_network);
    strcpy(channel, log_channel);
    strcpy(location, log_location);
    return NULL;
} // LogSNCL()


//////////////////////////////////////////////////////////////////////////////
// Used to change the defaults returned by LogSNCL
char *SetLogSNCL(const char *station, const char *network,
        const char *channel, const char *location)
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "SetLogSNCL: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }
    strncpy(log_station, station, 5);
    log_station[4] = 0;
    strncpy(log_network, network, 2);
    log_network[2] = 0;
    strncpy(log_channel, channel, 3);
    log_channel[3] = 0;
    strncpy(log_location, location, 2);
    log_location[2] = 0;
    return NULL;
} // SetLogSNCL()

//////////////////////////////////////////////////////////////////////////////
// Returns the UDP port where channel set commands are sent to
char *ChannelSetPort(
        int *port               // returns port number
        )                       // returns NULL or an error string pointer
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "ChannelSetPort: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    *port = iChannelSetPort;
    return NULL;
} // ChannelSetPort()

//////////////////////////////////////////////////////////////////////////////
// Returns the TCP port where log messages are sent to
char *LogServerPort(
        int *port               // returns port number
        )                       // returns NULL or an error string pointer
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "LoopServerPort: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    *port = iLogServerPort;
    return NULL;
} // LoopServerPort()

//////////////////////////////////////////////////////////////////////////////
// Returns username to log into falcon, NULL means use station name
char *FalconUsername(char *username)
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "FalconUsername: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    if (falcon_username != NULL)
        strcpy(username, falcon_username);
    else
        username[0] = 0;

    return NULL;
} // FalconUsername()

//////////////////////////////////////////////////////////////////////////////
// Returns password to log into falcon, NULL means use station name
char *FalconPassword(char *password)
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "FalconPassword: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    if (falcon_password != NULL)
        strcpy(password, falcon_password);
    else
        password[0] = 0;

    return NULL;
} // FalconPassword()

//////////////////////////////////////////////////////////////////////////////
// Returns Falcon IP address
char *FalconIP(char *ip)
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "FalconIP: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    strcpy(ip, falcon_IP);

    return NULL;
} // FalconIP()

//////////////////////////////////////////////////////////////////////////////
// Returns Falcon port number
char *FalconPort(int *port)
{
    if (parse_state == 0)
    {
        sprintf(looperrstr, "FalconPort: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    *port = falcon_port;
    return NULL;
} // FalconPort()

//////////////////////////////////////////////////////////////////////////////
// Remaps the station name in the seed record if diskloop.config
// has an applicable MapStation: entry 
// Returns 1 on a remap, 0 on no remap
int RemapStationName(                      
        char *str_header)        // seed record pointer
{
    seed_header *pheader;
    char station[8];
    char mapStation[8];
    int  i;

    char *mapstation=NULL;
    struct s_mapstation *ptr;

    pheader = (seed_header *)str_header;

    // Parse station name
    for (i=0; i < 5 
            && isalnum((int)pheader->station_id_call_letters[i]); i++)
        station[i] = pheader->station_id_call_letters[i];
    station[i] = 0;

    // Now remap q330 station name if match found
    strcpy(mapStation, station);
    strToUpper(mapStation);
    ptr = (struct s_mapstation *)map_get(pMapStationShortcut, mapStation);
    if (ptr != NULL) {
        mapstation = ptr->station;
    }
    else {
        for (ptr=pMapStationList; ptr != NULL; ptr=ptr->next)
        {
            if (strcmp(station, ptr->station_q330) == 0)
            {
                mapstation = ptr->station;
                strcpy(mapStation, ptr->station_q330);
                strToUpper(mapStation);
                map_put(pMapStationShortcut, mapStation, ptr);
                break;
            }
        } // check all station name remap entries
    }

    // Change seed record station name if we are remapping
    if (mapstation != NULL)
    {
        for (i=0; i < 5 && mapstation[i] != 0; i++)
        {
            pheader->station_id_call_letters[i] = mapstation[i];
        }
        // blank pad the station name to 5 chars
        for (; i < 5; i++)
        {
            pheader->station_id_call_letters[i] = ' ';
        }
    }

    return (mapstation != NULL);

} // RemapSationName()

//////////////////////////////////////////////////////////////////////////////
// Station name from 330 contains Network ID, this code strips off network
// Also maps q330 station name to final station name if needed
// Returns pointer to string after network ID
char *StripNetworkID(
        const char  *station    // Station name possibly includeing network id
        )                       // returns pointer skipping past network id
{
    int i;
    char *retstation;
    struct s_mapstation *ptr;
    char mapStation[8];

    for (i=0; station[i] != 0 && station[i] != '-'; i++)
        ; // just find end of string, of network-station separator

    if (station[i] == '-')
        retstation = (char *)&station[i+1];
    else
        retstation = (char *)station;

    // Now remap q330 station name if match found
    strcpy(mapStation, retstation);
    strToUpper(mapStation);
    ptr = (struct s_mapstation *)map_get(pMapStationShortcut, mapStation);
    if (ptr != NULL) {
        retstation = ptr->station;
    }
    else {
        for (ptr=pMapStationList; ptr != NULL; ptr=ptr->next)
        {
            if (strcmp(retstation, ptr->station_q330) == 0)
            {
                retstation = ptr->station;
                strcpy(mapStation, ptr->station_q330);
                strToUpper(mapStation);
                map_put(pMapStationShortcut, mapStation, ptr);
                break;
            }
        } // check all station name remap entries
    }

    return retstation;
} // StripNetworkID()

//////////////////////////////////////////////////////////////////////////////
// Makes a canonical diskloop file name
// Ensures that the path to a diskloop is always created the same way
// format: archive_directory/SSSS/[LL_]CCC[suffix]
void DiskloopFileName(
        const char *station,  // station name
        const char *location, // location code
        const char *channel,  // channel name
        const char *suffix,   // file name suffix
        char *name // buffer with enough space allocated for complete path to file
        )
{
    char *p = name;
    const char *t;

    t = loopDir;
    // diskloop directory
    while (*t > 31) {
        *p = *t; p++;
        t++;
    }
    *p = '/'; p++;

    t = StripNetworkID(station);
    // Station
    while (ALNUM(*t)) {
        *p = toupper(*t); p++;
        t++;
    }
    *p = '/'; p++;

    t = location;
    // Location
    while (ALNUM(*t)) {
        *p = toupper(*t); p++;
        t++;
    }
    if (t != location) {
        *p = '_'; p++;
    }

    t = channel;
    // Channel
    while (ALNUM(*t)) {
        *p = toupper(*t); p++;
        t++;
    }

    t = suffix;
    // suffix
    while (*t > 31) {
        *p = *t; p++;
        t++;
    }

    *p = 0;
}


//////////////////////////////////////////////////////////////////////////////
// Called when we need to start a new buffer set
char *NewBuffer(
        const char  *station,   // station name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )                       // returns NULL or an error string pointer
{
    char  buf_filename[2*MAXCONFIGLINELEN+2];
    char  idx_filename[2*MAXCONFIGLINELEN+2];
    char  msg[2*MAXCONFIGLINELEN+2];
    FILE  *fp_buf;
    FILE  *fp_idx;
    int   iRecord;
    int   iMaxRecord;

    // Get names of buffer and index files
    DiskloopFileName(station, loc, chan, ".buf", buf_filename);
    DiskloopFileName(station, loc, chan, ".idx", idx_filename);

    // Create/truncate buffer file
    if ((fp_buf=fopen(buf_filename, "w")) == NULL)
    {
        // Buffer file does not exist so start a new set
        sprintf(looperrstr, "NewBuffer: failed to create %s",
                idx_filename);
        return looperrstr;
    }
    fclose(fp_buf);

    // Create/truncate index file
    if ((fp_idx=fopen(idx_filename, "w")) == NULL)
    {
        sprintf(looperrstr, "NewBuffer: failed to find %s",
                idx_filename);
        return looperrstr;
    } // unable to create index file

    // Get how many records this channel should have
    if (NumChanRecords(station, chan, loc, &iMaxRecord) != NULL)
    {
        fclose(fp_idx);
        return looperrstr;
    }

    // Update the index file
    iRecord = -1;
    sprintf(msg, "%d %d", iRecord, iMaxRecord);
    fprintf(fp_idx, "%-30.30s\n", msg);
    fprintf(fp_idx, "%-30.30s\n", msg);
    fprintf(fp_idx, "%-30.30s\n", msg);
    fclose(fp_idx);

    return NULL;
} // NewBuffer()

//////////////////////////////////////////////////////////////////////////////
// Parses str_header to get station, channel, location, time start/end
char *ParseSeedHeader(
        const char    *str_header,  // Header buffer
        char          *station,     // return station name
        char          *chan,        // return Channel ID
        char          *loc,         // return Location ID
        STDTIME2      *ptRecStart,  // Returns start time for record
        STDTIME2      *ptRecEnd,    // Returns end time for record
        int           *piSeqNum,    // Returns the sequence number for the record
        int           *piSamples    // Returns the number of samples
        )                         // returns NULL or an error string pointer
{
    int i;
    int iRateFactor;
    int iRateMult;
    int iSamples;
    int iSpanDay;
    int iSpanHour;
    int iSpanMin;
    int iSpanSec;
    int iSpanTMSec;
    double dSampleRate;
    seed_header *pheader;
    char str[10];

    pheader = (seed_header *)str_header;

    // Parse the sequence number
    *piSeqNum = 0;
    strncpy(str, pheader->sequence, 6);
    str[6] = 0;
    sscanf(str, "%d", piSeqNum);

    // Parse station name
    for (i=0; i < 5 
            && isalnum((int)pheader->station_id_call_letters[i]); i++)
        station[i] = pheader->station_id_call_letters[i];
    station[i] = 0;

    // Parse location name
    for (i=0; i < 2; i++)
        loc[i] = pheader->location_id[i];
    loc[i] = 0;

    // Parse channel name
    for (i=0; i < 3; i++)
        chan[i] = pheader->channel_id[i];
    chan[i] = 0;

    // Parse Record Start time
    ptRecStart->year = ntohs(pheader->yr);
    ptRecStart->day = ntohs(pheader->jday);
    ptRecStart->hour = (int)pheader->hr;
    ptRecStart->minute = (int)pheader->minute;
    ptRecStart->second = (int)pheader->seconds;
    ptRecStart->tenth_msec = ntohs(pheader->tenth_millisec);

    // Parse samples
    iSamples = (unsigned short)ntohs(pheader->samples_in_record);
    iRateFactor = (short)ntohs(pheader->sample_rate_factor);
    iRateMult = (short)ntohs(pheader->sample_rate_multiplier);

    *piSamples = iSamples;

    // Get sample rate, See SEED Reference Manual Chp 8
    // Fixed Section of Data Header for formulas
    dSampleRate = 0;
    if (iRateFactor > 0 && iRateMult > 0)
        dSampleRate = (double)(iRateFactor * iRateMult);

    if (iRateFactor > 0 && iRateMult < 0)
        dSampleRate = -((double)iRateFactor / (double)iRateMult);

    if (iRateFactor < 0 && iRateMult > 0)
        dSampleRate = -((double)iRateMult / (double)iRateFactor);

    if (iRateFactor < 0 && iRateMult < 0)
        dSampleRate = 1.0 / (double)(iRateFactor * iRateMult);

    // Get Span time in tenths of milliseconds
    if (iRateFactor != 0 && iRateMult != 0)
        iSpanTMSec = (int)((iSamples / dSampleRate) * 10000.0);
    else
        iSpanTMSec = 0;

    //fprintf(stdout, "Samp=%d Factor=%d Mult=%d dSampleRate=%.5f, TMSec=%d\n",
    //iSamples, iRateFactor, iRateMult, dSampleRate, iSpanTMSec);


    // Add to start time to get end time
    iSpanSec = iSpanTMSec/10000;
    iSpanTMSec = iSpanTMSec % 10000;
    iSpanMin = iSpanSec / 60;
    iSpanSec = iSpanSec % 60;
    iSpanHour = iSpanMin / 60;
    iSpanMin = iSpanMin % 60;
    iSpanDay = iSpanHour / 24;
    iSpanHour = iSpanHour % 24;
    //fprintf(stdout, "%s + ", ST_PrintDate2(*ptRecStart, 1));
    //fprintf(stdout, "%03d,%02d:%02d:%02d.%04d",
    //iSpanDay, iSpanHour, iSpanMin, iSpanSec, iSpanTMSec);
    *ptRecEnd = ST_AddToTime2(*ptRecStart, 
            iSpanDay, iSpanHour, iSpanMin, iSpanSec, iSpanTMSec);
    //fprintf(stdout, " = %s\n", ST_PrintDate2(*ptRecEnd, 1));
    return NULL;
} // ParseSeedHeader()

//////////////////////////////////////////////////////////////////////////////
// The index info is repeated 3 times.  So even if we read it during the
// middle of a write, either the first two, or second two lines should agree.
char *ParseIndexInfo(
        FILE *fp_idx,       // opened file pointer to index file
        int  *iFlipRecord,  // Returns current position within circular buffer
        int  *iMaxRecord    // Maximum size of circular buffer
        )                   // NULL okay, error string otherwise
{
    int flip1,flip2,flip3;
    int max1,max2,max3;
    int iArg;
    int iCount;
    char indexbuf[93];

    if ((iCount=fread(indexbuf, 1, 93, fp_idx)) != 93)
    {
        sprintf(looperrstr, "ParseIndexInfo:  Index data file size != 93 chars");
        return looperrstr;
    }

    if ((iArg = sscanf(&indexbuf[0], "%d %d", &flip1, &max1)) != 2)
    {
        sprintf(looperrstr, "ParseIndexInfo: format error in index file line 1");
        return looperrstr;
    }
    *iFlipRecord = flip1;
    *iMaxRecord = max1;
    if ((iArg = sscanf(&indexbuf[31], "%d %d", &flip2, &max2)) != 2)
    {
        sprintf(looperrstr, "ParseIndexInfo: format error in index file line 2");
        return looperrstr;
    }

    // If line 1 and 2 are in agreement we are done
    if (flip1 == flip2 || max1 == max2)
        return NULL;

    // Otherwise line 3 will contain uncontaminated data
    // Because write point had to be between line 1 and 2
    if ((iArg = sscanf(&indexbuf[62], "%d %d", &flip3, &max3)) != 2)
    {
        sprintf(looperrstr, "ParseIndexInfo: format error in index file line 3");
        return looperrstr;
    }

    *iFlipRecord = flip3;
    *iMaxRecord = max3;
    return NULL;
} // ParseIndexInfo()


void min_time(STDTIME2 *time)
{
    time->year   = 1; 
    time->day    = 1;
    time->hour   = 0;
    time->minute = 0;
    time->second = 0;
    time->tenth_msec = 1;
}

void max_time(STDTIME2 *time)
{
    time->year   = SHRT_MAX;
    time->day    = 365; // SHRT_MAX (32767) is not a leap year
    time->hour   = 23;
    time->minute = 59;
    time->second = 59;
    time->tenth_msec = 9999;
}

//////////////////////////////////////////////////////////////////////////////
// Opens a diskloop if it has not already been opened,
// otherwise it just uses the existing context
char *adl_open(
        diskloop_context_t **return_context, // caller's context ponter
        const char *station,  // station name
        const char *location, // location code
        const char *channel   // channel name
    )
{
    diskloop_context_t *context = NULL;
    char  key[20];

    int   loop_offset = 0;
    long  loop_size = 0;

    // locate context for this 
    MakeKey(station, location, channel, key);
    context = (diskloop_context_t *)map_get(pDiskLoops, key);

    // If there is no context for this channel, create it
    if (context == NULL) {
        if ((context = (diskloop_context_t *)calloc(sizeof(diskloop_context_t), 1)) == NULL) {
            strcpy(looperrstr, "Could not allocate memory for diskloop context.");
            goto error;
        }
        // Store a copy of the key in the context (needed by adl_close)
        MakeKey(station, location, channel, context->key);

        // Store the loop and index file names in the context for reporting purposes
        DiskloopFileName(station, location, channel, ".buf", context->loop_name);
        DiskloopFileName(station, location, channel, ".idx", context->index_name);

        // Make sure that buffer file exists
        if ((context->loop_fp=fopen(context->loop_name, "r")) == NULL)
        {
            // Buffer file does not exist so start a new set
            if (NewBuffer(station, channel, location) != NULL) {
                goto error;
            }
        }
        if (context->loop_fp != NULL) {
            fclose(context->loop_fp);
        }

        // Make sure that index file exists
        if ((context->index_fp=fopen(context->index_name, "r")) == NULL)
        {
            // Index file does not exist so start fresh
            if (NewBuffer(station, channel, location) != NULL) {
                goto error;
            }
            if ((context->index_fp=fopen(context->index_name, "r")) == NULL)
            {
                sprintf(looperrstr, "adl_open: failed to find %s",
                        context->index_name);
                goto error;
            }
            context->index = -1;
            context->size = 0;
        } // unable to open index file

        // Load index info
        if (ParseIndexInfo(context->index_fp, &context->index, &context->capacity) != NULL)
        {
            sprintf(looperrstr, "adl_open: Index file format error in %s",
                    context->index_name);
            fclose(context->index_fp);
            goto error;
        } // error reading index and max record value

        if (context->index < -1 || context->index >= context->capacity)
        {
            sprintf(looperrstr, "adl_open: Invalid index -1 <= %d < %d in %s",
                    context->index, context->capacity, context->index_name);
            fclose(context->index_fp);
            goto error;
        } // error reading index and max record value
        if (context->index_fp != NULL) {
            fclose(context->index_fp);
        }

        // Open the buffer file for read/write access
        if ((context->loop_fp=fopen(context->loop_name, "r+")) == NULL)
        {
            // Buffer file does not exist so start a new set
            sprintf(looperrstr, "adl_open: Failed to open diskloop %s for updating",
                    context->loop_name);
            goto error;
        }

        // Check the size of the diskloop file
        fseek(context->loop_fp, 0, SEEK_END);
        loop_size = ftell(context->loop_fp);

        context->size = context->capacity;
        // If the diskloop is not full, we have not yet looped around,
        // so start at the end.
        if ((loop_size / context->record_size) < context->capacity) {
            // Integer arithmetic will jump us to the end of the last complete
            // record, overwriting any partial record
            context->size = loop_size / context->record_size;
            context->index = context->size - 1;
            // Seek to the point at which the next record should be written
            fseek(context->loop_fp, context->size * context->record_size, SEEK_SET);
        }
        // If the diskloop is full, we continue with the following logic
        else if (context->index >= 0)
        {
        // TODO: Impmlement the following logic
        //        - read record from last position, and following record
        //          (following record may be at the beginning of the diskloop)
        //        - if the last record is younger than the following, we are okay
        //          because the idx file matches the expected order
        //        - however, if the last record is older than the following, we
        //          close without syncing the idx file, and must do a binary
        //          search for the youngest record, and replace it (because it
        //          could be only a partial record)
        //
        // Get the header for the last record written

            char      temp_A[8192];
            char      temp_B[8192];

            char      rec_A_station[6];
            char      rec_A_chan[4];
            char      rec_A_loc[4];
            STDTIME2  rec_A_start;
            STDTIME2  rec_A_end;
            int       rec_A_seqnum;
            int       rec_A_samples;

            char      rec_B_station[6];
            char      rec_B_chan[4];
            char      rec_B_loc[4];
            STDTIME2  rec_B_start;
            STDTIME2  rec_B_end;
            int       rec_B_seqnum;
            int       rec_B_samples;

            DELTA_T2  delta_t2;
            long      delta_ms;

            int tmp_index = context->index;

            STDTIME2  temp_time;
            
            // Read and parse the last record indicated in the index file
            if ((tmp_index) >= context->capacity) {
                tmp_index = 0;
            }
            if (adl_read(context, tmp_index, temp_A) == NULL) {
                sprintf(looperrstr, "adl_open: could not read newest record for %s", context->index_name);
                fclose(context->index_fp);
                goto error;
            }
            ParseSeedHeader(temp_A, rec_A_station, rec_A_chan, rec_A_loc,
                    &rec_A_start, &rec_A_end, &rec_A_seqnum, &rec_A_samples);

            // Read and parse the record following the last in the index file
            tmp_index++;
            if ((tmp_index) >= context->capacity) {
                tmp_index = 0;
            }
            if (adl_read(context, tmp_index, temp_A) == NULL) {
                sprintf(looperrstr, "adl_open: could read following record for %s", context->index_name);
                fclose(context->index_fp);
                goto error;
            }
            ParseSeedHeader(temp_B, rec_B_station, rec_B_chan, rec_B_loc,
                    &rec_B_start, &rec_B_end, &rec_B_seqnum, &rec_B_samples);

            // Test to see if this record Starts before last record ended
            // Only test records with data in them
            delta_t2 = ST_DiffTimes2(rec_A_start, rec_B_start);
            delta_ms = ST_DeltaToMS2(delta_t2);

            // If the next record is newer than the current, we need to
            // search for the newest record, update the index, and seek
            // to the correct position in the diskloop.
            max_time(&temp_time);
            if (delta_ms < 0) {
                adl_newest_index(context, &context->index);
            }
        } // previous record should exist

        // Go to desired position in the circular buffer file
        loop_offset = context->index * context->record_size;
        fseek(context->loop_fp, loop_offset, SEEK_SET);
        if (loop_offset != ftell(context->loop_fp))
        {
            sprintf(looperrstr, "adl_open: Unable to seek to record %d in %s",
                    context->index, context->loop_name);
            goto error;
        } // Failed to seek to required file buffer position

        // Open the index file for writing
        if ((context->index_fp=fopen(context->index_name, "r+")) == NULL)
        {
            // Index file does not exist so start fresh
            sprintf(looperrstr, "adl_open: failed to open %s for updating",
                    context->index_name);
            goto error;
        } // unable to open index file

        // Add this diskloop context to the map
        map_put(pDiskLoops, context->key, context);
    } // create new context

    // if we made it here, no error was encountered
    goto success;

 error:
    if (context) {
        free(context);
    }
    return looperrstr;

 success:
    *return_context = context;
    return NULL;
}

char *adl_write(
        diskloop_context_t *context, // diskloop context
        int index, // record index
        char *record // record buffer
    )
{
    int  new_index = 0;
    struct timeval now;
    uint64_t now_stamp;
    uint64_t flush_stamp;
    uint64_t index_stamp;

    seed_header *pheader;
    char      rec_station[6];
    char      rec_chan[4];
    char      rec_loc[4];
    STDTIME2  rec_start;
    STDTIME2  rec_end;
    int       rec_seqnum;
    int       rec_samples;
    DELTA_T2  delta_time;
    long      delta_ms;
    int       rate_factor;
    char     *issue = NULL;

    ParseSeedHeader(record, rec_station, rec_chan, rec_loc,
                    &rec_start, &rec_end,
                    &rec_seqnum, &rec_samples);

    delta_time = ST_DiffTimes2(rec_start, context->last_record_start);
    delta_ms = ST_DeltaToMS2(delta_time);
    pheader = (seed_header *)record;
    rate_factor = (short)ntohs(pheader->sample_rate_factor);
    if ( (rec_samples > 0) &&
         (context->last_record_samples > 0) &&
         (rate_factor != 0) )
    {
        if (delta_ms < -1) {
            issue = "overlap";
        } else if (delta_ms > 1) {
            issue = "gap";
        }

        if (issue) {
            printf("DEBUG detected %s (%ld) %s %s/%s new record %d vs %d.\n",
                    issue, delta_ms, rec_station, rec_loc, rec_chan,
                    rec_seqnum, context->last_record_seqnum);
            printf("%s < ", ST_PrintDate2(rec_start, TRUE));
            printf("%s\n", ST_PrintDate2(context->last_record_start, TRUE));
        }
    }

    if (index < 0) {
        new_index = context->index + 1;
        if (new_index >= context->capacity) {
            new_index = 0;
            fseek(context->loop_fp, 0, SEEK_SET);
        }
    }
    else {
        if (index >= context->capacity) {
            sprintf(looperrstr, "adl_write: Requested index [%d] is out of range", index);
            goto error;
        }
        new_index = index;
    }

    // Write the new record
    if (fwrite(record, context->record_size, 1, context->loop_fp) != 1)
    {
        sprintf(looperrstr, "adl_write: Unable to write to index %d", new_index);
        record[0] = 0; // TODO: determine if this is a good idea
        goto error;
    } // Failed to write record

    context->has_written = 1;
    // update context size if this is an append operation
    if ( (context->size < context->capacity) &&
         (context->size == new_index) ) {
        context->size++;
    }
    context->index = new_index;
    context->last_record_seqnum = rec_seqnum;
    context->last_record_samples = rec_samples;
    memcpy(&context->last_record_start, &rec_start, sizeof(STDTIME2));

    gettimeofday(&now, NULL);
    now_stamp   = (uint64_t)TIME_MICRO(now);
    flush_stamp = (uint64_t)TIME_MICRO(context->last_flush);
    index_stamp = (uint64_t)TIME_MICRO(context->last_index);

    // If the index interval has been met, we need to flush and 
    // record the current index.
    if ((now_stamp - index_stamp) >= TIME_MICRO(index_interval)) {
        adl_flush(context);
        adl_write_index(context);
    }
    // Otherwise, we just flush the diskloop if the flush interval
    // has been met.
    else if ((now_stamp - flush_stamp) >= TIME_MICRO(flush_interval)) {
        adl_flush(context);
    }

    // if we made it here, no error was encountered
    goto success;

 error:
    return looperrstr;

 success:
    return NULL;
}

char *adl_flush(
        diskloop_context_t *context // diskloop context
    )
{
    // Only flush if we have performed a write operation
    if (context->has_written) {
        fflush(context->loop_fp);
        gettimeofday(&context->last_flush, NULL);
    }

    return NULL;
}

char *adl_write_index(
        diskloop_context_t *context // diskloop context
    )
{
    char msg[2*MAXCONFIGLINELEN+2];

    // Only update the index if we have performed a write operation
    if (context->has_written) {
        fseek(context->index_fp, 0, SEEK_SET);
        sprintf(msg, "%d %d", context->index, context->capacity);
        fprintf(context->index_fp, "%-30.30s\n", msg);
        fprintf(context->index_fp, "%-30.30s\n", msg);
        fprintf(context->index_fp, "%-30.30s\n", msg);
        fflush(context->index_fp);
        gettimeofday(&context->last_index, NULL);

        // Reset write tracker in case this should be used as read-only
        // from here on.
        context->has_written = 0;
    }

    return NULL;
}

char *adl_read(
        diskloop_context_t *context, // diskloop context
        int index, // record index
        char *databuf // record buffer
    )
{
    int loop_seek = 0;

    if (index < 0) {
        index = context->index;
    }

    if (index >= context->capacity) {
        sprintf(looperrstr, "adl_read: Requested index [%d] is out of range", index);
        goto error;
    }

    loop_seek = index * context->record_size;
    fseek(context->loop_fp, loop_seek, SEEK_SET);
    if (ftell(context->loop_fp) != loop_seek) {
        sprintf(looperrstr, "adl_read: Could not seek to requested index [%d]", index);
        goto error;
    }
        
    if (fread(databuf, context->record_size, 1, context->loop_fp) != 1)
    {
        sprintf(looperrstr, "adl_read: Unable to read record at index %d", index);
        goto error;
    }

    // if we made it here, no error was encountered
    goto success;

 error:
    return looperrstr;

 success:
    return NULL;
}

char *adl_close(
        diskloop_context_t **close_context // diskloop context
    )
{
    diskloop_context_t *context = *close_context;

    map_remove(pDiskLoops, context->key);
    adl_flush(context);
    fclose(context->loop_fp);
    adl_write_index(context);
    fclose(context->index_fp);
    free(context);
    *close_context = NULL;

    return NULL;
}


void close_callback(const char *key, const void *value, const void *obj)
{
    adl_close((diskloop_context_t **)(&value));
}

char *adl_close_all()
{
    map_enum(pDiskLoops, close_callback, NULL);
    return NULL;
}

void swap(void **ptr_a, void **ptr_b)
{
    void *swap_temp;
    swap_temp = *ptr_a;
    *ptr_b = *ptr_a;
    *ptr_a = swap_temp;
}

int compare_dcc_times(STDTIME2 *time_a, STDTIME2 *time_b)
{
    return ST_DeltaToMS2(ST_DiffTimes2(*time_a, *time_b));
}

void prepare_record_info(record_info_t *info)
{
    ParseSeedHeader(info->buffer, info->station, info->channel, info->location,
                    &info->start_time, &info->end_time, &info->seqnum, &info->samples);
    info->rate_factor = (short)ntohs(((seed_header *)(&info->buffer))->sample_rate_factor);
}

char *adl_locate_index(
        diskloop_context_t *context,
        STDTIME2 *time,
        int whence,
        int *index
    )
{
    int seek_store = 0;
    int file_size = 0;

    int last_index = 0;
    int max_index = 0;
    int min_index = 0;
    int pivot_index = 0;

    int index_before = 0;
    int index_after = 0;

    STDTIME2 file_first_start_time; // time of first record in the file (not oldest)
    STDTIME2 file_first_end_time; // time of first record in the file (not oldest)
    STDTIME2 file_last_start_time;  // time of the last record in the file (not youngest)
    STDTIME2 file_last_end_time;  // time of the last record in the file (not youngest)

    record_info_t record_before;
    record_info_t this_record;
    record_info_t record_after;

    // Time comparisons
    // C = current record start time
    // T = target record start time
    // F = first record start time
    // L = last record start time
    long tdiff_C_T;
    long tdiff_C_F;
    long tdiff_C_L;
    long tdiff_T_F;
    long tdiff_T_L;


    // Save the original location to restore before returning
    seek_store = ftell(context->loop_fp);

    // Check the file size
    fseek(context->loop_fp, 0, SEEK_END);
    file_size = ftell(context->loop_fp);

    // If the file is less than a single record in size,
    // set the index to -1 indicating an empty diskloop
    if (file_size < context->record_size) {
        *index = -1;
        goto success;
    }

    // If the file contains only one record, the index is 0
    if (file_size < (context->record_size * 2)) {
        *index = 0;
        goto success;
    }

    // Determine the index of the last record
    last_index = file_size / context->record_size - 1;

    // Retrieve the first (start of file) record's times
    if (adl_read(context, 0, this_record.buffer) != NULL) {
        sprintf(looperrstr, "adl_locate_index: Could not read first (start of file) record from %s",
                context->loop_name);
        goto error;
    }
    prepare_record_info(&this_record);
    memcpy((void *)&file_first_start_time, (void *)&this_record.start_time, sizeof(STDTIME2));
    memcpy((void *)&file_first_end_time,   (void *)&this_record.end_time,   sizeof(STDTIME2));

    // Retrieve the last (end of file) record's times
    if (adl_read(context, last_index, this_record.buffer) != NULL)
    {
        sprintf(looperrstr, "adl_locate_index: Could not read last (end of file) record from %s",
                context->loop_name);
        goto error;
    }
    prepare_record_info(&this_record);
    memcpy((void *)&file_last_start_time, (void *)&this_record.start_time, sizeof(STDTIME2));
    memcpy((void *)&file_last_end_time,   (void *)&this_record.end_time,   sizeof(STDTIME2));

    // Check if we are looped, and which direction to check
    // SEARCH_LOOP_HIGH
    // SEARCH_LOOP_LOW

    min_index = 0;
    max_index = last_index;
    while (min_index < max_index) {
        pivot_index = min_index + ((max_index - min_index) / 2);
        // read record at the 
        if (adl_read(context, pivot_index, this_record.buffer) != NULL) {
            sprintf(looperrstr, "adl_locate_index: Could not read record at index %d from %s",
                    pivot_index, context->loop_name);
            goto error;
        }
        prepare_record_info(&this_record);

        tdiff_C_T = compare_dcc_times(&this_record.start_time, time);
        if (tdiff_C_T < 0) { // current < target
            tdiff_C_F = compare_dcc_times(&this_record.start_time, &file_first_start_time);
            if (tdiff_C_F < 0) { // current < first
                tdiff_T_L = compare_dcc_times(time, &file_last_start_time);
                if (tdiff_T_L > 0) { // target > last
                    max_index = pivot_index;
                }
                else { // target <= last
                    min_index = pivot_index;
                }
            }
            else { // current >= first (requires that: target > first)
                min_index = pivot_index;
            }
        }
        else if (tdiff_C_T > 0) { // current > target
            tdiff_T_F = compare_dcc_times(time, &file_first_start_time);
            if (tdiff_T_F < 0) { // target < first
                tdiff_C_L = compare_dcc_times(&this_record.start_time, &file_last_start_time);
                if (tdiff_C_L > 0 ) { // current > last
                    max_index = pivot_index;
                }
                else { // current <= last
                    min_index = pivot_index;
                }
            }
            else { // target >= first (requires that: current > first)
                max_index = pivot_index;
            }
        }
        else {
            min_index = max_index = pivot_index;
        }
    }

    index_before = (pivot_index > 0) ? (pivot_index - 1) : last_index;
    if (adl_read(context, index_before, record_before.buffer) != NULL) {
        sprintf(looperrstr, "adl_locate_index: Could not record before selected (%d - 1 = %d) from %s",
                pivot_index, index_before, context->loop_name);
        goto error;
    }
    prepare_record_info(&record_before);

    index_after = (pivot_index < last_index) ? (pivot_index + 1) : 0;
    if (adl_read(context, index_after, record_after.buffer) != NULL) {
        sprintf(looperrstr, "adl_locate_index: Could not record after selected (%d + 1 = %d) from %s",
                pivot_index, index_after, context->loop_name);
        goto error;
    }
    prepare_record_info(&record_after);

    switch (whence) {
        case INDEX_BEFORE_EXCLUSIVE:
            // the newest record with an end time earlier than the indicated time
            if ( compare_dcc_times(&this_record.end_time, time) < 0) {
                if ( (compare_dcc_times(&record_after.end_time, time) < 0) && 
                     (compare_dcc_times(&this_record.start_time, &record_after.start_time) < 0) )
                {
                    *index = index_after;
                }
                else {
                    *index = pivot_index;
                }
            }
            else if (compare_dcc_times(&record_before.end_time, time) < 0) {
                *index = index_before;
            }
            else {
                *index = -1;
            }
            break;

        case INDEX_BEFORE_INCLUSIVE:
            // the newest record with an start time at or earlier than the indicated time
            if ( compare_dcc_times(&this_record.start_time, time) <= 0) {
                if ( (compare_dcc_times(&record_after.start_time, time) <= 0) && 
                     (compare_dcc_times(&this_record.start_time, &record_after.start_time) < 0) )
                {
                    *index = index_after;
                }
                else {
                    *index = pivot_index;
                }
            }
            else if (compare_dcc_times(&record_before.start_time, time) <= 0) {
                *index = index_before;
            }
            else {
                *index = -1;
            }
            break;

        case INDEX_WITHIN:
            // the record that contains the indicated time (if any)
            if ( (compare_dcc_times(&this_record.start_time, time) <= 0) &&
                 (compare_dcc_times(&this_record.end_time,   time) >= 0) )
            {
                *index = pivot_index;
            }
            else if ( (compare_dcc_times(&record_before.start_time, time) <= 0) &&
                      (compare_dcc_times(&record_before.end_time,   time) >= 0) )
            {
                *index = index_before;
            }
            else if ( (compare_dcc_times(&record_after.start_time, time) <= 0) &&
                      (compare_dcc_times(&record_after.end_time,   time) >= 0) )
            {
                *index = index_after;
            }
            else {
                *index = -1;
            }
            break;

        case INDEX_AFTER_INCLUSIVE:
            // the oldest record with an end time at or later than the indicated time
            if ( compare_dcc_times(&this_record.end_time, time) >= 0) {
                if ( (compare_dcc_times(&record_before.end_time, time) >= 0) &&
                     (compare_dcc_times(&this_record.start_time, &record_before.start_time) > 0) )
                {
                    *index = index_before;
                }
                else {
                    *index = pivot_index;
                }
            }
            else if (compare_dcc_times(&record_after.end_time, time) >= 0) {
                *index = index_after;
            }
            else {
                *index = -1;
            }
            break;

        case INDEX_AFTER_EXCLUSIVE:
            // the oldest record with a start time later than the indicated time
            if ( compare_dcc_times(&this_record.start_time, time) > 0) {
                if ( (compare_dcc_times(&record_before.start_time, time) > 0) &&
                     (compare_dcc_times(&this_record.start_time, &record_before.start_time) > 0) )
                {
                    *index = index_before;
                }
                else {
                    *index = pivot_index;
                }
            }
            else if (compare_dcc_times(&record_after.start_time, time) > 0) {
                *index = index_after;
            }
            else {
                *index = -1;
            }
            break;

        default:
            sprintf(looperrstr, "adl_locate_index: invalid value for 'whence' [%d]",
                    whence);
            goto error;
    }

    // if we made it here, no error was encountered
    goto success;

 error:
    fseek(context->loop_fp, seek_store, SEEK_SET);
    return looperrstr;

 success:
    fseek(context->loop_fp, seek_store, SEEK_SET);
    return NULL;
}

char *adl_oldest_index(
        diskloop_context_t *context,
        int *index
    )
{
    STDTIME2 temp_time;
    min_time(&temp_time);
    return adl_locate_index(context, &temp_time, INDEX_BEFORE_INCLUSIVE, index);
}

char *adl_newest_index(
        diskloop_context_t *context,
        int *index
    )
{
    STDTIME2 temp_time;
    max_time(&temp_time);
    return adl_locate_index(context, &temp_time, INDEX_AFTER_INCLUSIVE, index);
}

char *adl_index_before(
        diskloop_context_t *context,
        STDTIME2  *time,
        int inclusive,
        int *index
    )
{
    int whence = inclusive ? INDEX_BEFORE_INCLUSIVE : INDEX_BEFORE_EXCLUSIVE;
    return adl_locate_index(context, time, whence, index);
}

char *adl_index_after(
        diskloop_context_t *context,
        STDTIME2  *time,
        int inclusive,
        int *index
    )
{
    int whence = inclusive ? INDEX_AFTER_INCLUSIVE : INDEX_AFTER_EXCLUSIVE;
    return adl_locate_index(context, time, whence, index);
}

char *adl_range_indices(
        diskloop_context_t *context,
        STDTIME2  *start_time,
        STDTIME2  *end_time,
        int inclusive,
        int *start_index,
        int *end_index
    )
{
    char *result = NULL;
    int start_whence = inclusive ? INDEX_AFTER_INCLUSIVE  : INDEX_AFTER_EXCLUSIVE;
    int end_whence   = inclusive ? INDEX_BEFORE_INCLUSIVE : INDEX_BEFORE_EXCLUSIVE;

    // TODO: make sure the start time is before the end time

    result = adl_locate_index(context, start_time, start_whence, start_index);
    // Only proceed if the start record was successfully located
    if (result == NULL) {
        result = adl_locate_index(context, end_time, end_whence, end_index);
    }
    return result;
}

//////////////////////////////////////////////////////////////////////////////
// Write a record of SEED data
// Hides all of the logic needed to run a circular buffer from main program
char *WriteChan(
        const char  *station,   // station name
        const char  *chan,      // Channel ID
        const char  *loc,       // Location ID
        char        *databuf    // Seed record pointer
        )                       // returns NULL or an error string pointer
{
    char  buf_filename[2*MAXCONFIGLINELEN+2];
    char  idx_filename[2*MAXCONFIGLINELEN+2];
    char  msg[2*MAXCONFIGLINELEN+2];
    FILE  *fp_buf;
    FILE  *fp_idx;
    int   iRecord;
    int   iMaxRecord;
    int   iSeek;
    //diskloop_context_t *context;

    // The configuration file must be parsed before this routine can work
    if (parse_state == 0)
    {
        sprintf(looperrstr, "WriteChan: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    /* XXX
    if (adl_open(station, loc, chan, &context) != NULL) {
        return looperrstr;
    }
    */

    // Get names of buffer and index files
    DiskloopFileName(station, loc, chan, ".buf", buf_filename);
    DiskloopFileName(station, loc, chan, ".idx", idx_filename);

    // Make sure that buffer file exists
    if ((fp_buf=fopen(buf_filename, "r")) == NULL)
    {
        // Buffer file does not exist so start a new set
        if (NewBuffer(station, chan, loc) != NULL)
            return looperrstr;
    }
    if (fp_buf != NULL) fclose(fp_buf);

    // Make sure that index file exists
    if ((fp_idx=fopen(idx_filename, "r")) == NULL)
    {
        // Index file does not exist so start fresh
        if (NewBuffer(station, chan, loc) != NULL)
            return looperrstr;
        if ((fp_idx=fopen(idx_filename, "r")) == NULL)
        {
            sprintf(looperrstr, "WriteChan: failed to find %s",
                    idx_filename);
            return looperrstr;
        }
    } // unable to open index file

    // Load index info
    if (ParseIndexInfo(fp_idx, &iRecord, &iMaxRecord) != NULL)
    {
        sprintf(looperrstr, "WriteChan: Index file format error in %s",
                idx_filename);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    if (iRecord < -1 || iRecord >= iMaxRecord)
    {
        sprintf(looperrstr, "WriteChan: Invalid index -1 <= %d < %d in %s",
                iRecord, iMaxRecord, idx_filename);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value
    fclose(fp_idx);

    // Get the header for the last record written
    if (iRecord >= 0)
    {
        char      tempbuf[8192];
        char      rec_station[6];
        char      rec_chan[4];
        char      rec_loc[4];
        STDTIME2  rec_tStart;
        STDTIME2  rec_tEnd;
        STDTIME2  cur_tStart;
        STDTIME2  cur_tEnd;
        DELTA_T2  tDelta;
        long      lMs;
        int       rec_seqnum;
        int       rec_samples;
        int       cur_seqnum;
        int       cur_samples;
        int       iRateFactor;
        seed_header *pheader;


        // A previous record exists
        if (ReadLast(station, chan, loc, tempbuf) == NULL)
        {
            // We successfully read last record so get timespan
            ParseSeedHeader(tempbuf, rec_station, rec_chan, rec_loc,
                    &rec_tStart, &rec_tEnd, &rec_seqnum, &rec_samples);
            // Get timespan for current record
            ParseSeedHeader(databuf, rec_station, rec_chan, rec_loc,
                    &cur_tStart, &cur_tEnd, &cur_seqnum, &cur_samples);

            // Test to see if this record Starts before last record ended
            // Only test records with data in them
            tDelta = ST_DiffTimes2(cur_tStart, rec_tEnd);
            lMs = ST_DeltaToMS2(tDelta);
            pheader = (seed_header *)databuf;
            iRateFactor = (short)ntohs(pheader->sample_rate_factor);
            if (cur_samples > 0 && rec_samples > 0 && iRateFactor != 0 &&
                    lMs < -1)
            {
                printf("DEBUG detected overlap (%ld) %s %s/%s new record %d vs %d.\n",
                        lMs, station, loc, chan, cur_seqnum, rec_seqnum);
                printf("%s < ", ST_PrintDate2(cur_tStart, TRUE));
                printf("%s\n", ST_PrintDate2(rec_tEnd, TRUE));
            }
            if (cur_samples > 0 && rec_samples > 0 && iRateFactor != 0 &&
                    lMs > 1)
            {
                printf("DEBUG detected gap (%ld) %s %s/%s new record %d vs %d.\n",
                        lMs, station, loc, chan, cur_seqnum, rec_seqnum);
                printf("%s > ", ST_PrintDate2(cur_tStart, TRUE));
                printf("%s\n", ST_PrintDate2(rec_tEnd, TRUE));
            }

            /* Remove former 4096 check for overwrite record
            // Check for matching record number and a greater number of samples
            if (rec_seqnum == cur_seqnum && rec_samples < cur_samples)
            {
            // overwrite older partial data
            // Cause an overwrite by decrementing pointer
            iRecord--;
            fprintf(stdout, "Replacing duplicate data for %s %s/%s\n",
            rec_station, rec_loc, rec_chan);
            } // sequence numbers match, and additional data
             */
        } // Able to read previuos record
    } // previous record should exist

    // Increment index to point to next free record
    iRecord = (iRecord + 1) % iMaxRecord;

    // Open the buffer file for write access
    if ((fp_buf=fopen(buf_filename, "r+")) == NULL)
    {
        // Buffer file does not exist so start a new set
        sprintf(looperrstr, "WriteChan: Failed to open %s for updating",
                buf_filename);
        return looperrstr;
    }

    // Go to desired position in the circular buffer file
    iSeek = iRecord * iLoopRecordSize;
    fseek(fp_buf, iSeek, SEEK_SET);
    if (iSeek != ftell(fp_buf))
    {
        sprintf(looperrstr, "WriteChan: Unable to seek to record %d in %s",
                iRecord, buf_filename);
        fclose(fp_buf);
        return looperrstr;
    } // Failed to seek to required file buffer position

    // Write the new record
    if (fwrite(databuf, iLoopRecordSize, 1, fp_buf) != 1)
    {
        sprintf(looperrstr, "WriteChan: Unable to write record %d in %s",
                iRecord, buf_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        return looperrstr;
    } // Failed to write record
    fclose(fp_buf);

    // Write the new index values
    if ((fp_idx=fopen(idx_filename, "r+")) == NULL)
    {
        // Index file does not exist so start fresh
        sprintf(looperrstr, "WriteChan: failed to open %s for updating",
                idx_filename);
        return looperrstr;
    } // unable to open index file

    // Update the index file
    // index is writen three times so reader can detect midstream update
    sprintf(msg, "%d %d", iRecord, iMaxRecord);
    fprintf(fp_idx, "%-30.30s\n", msg);
    fprintf(fp_idx, "%-30.30s\n", msg);
    fprintf(fp_idx, "%-30.30s\n", msg);
    fclose(fp_idx);

    //fprintf(stdout, "DEBUG WriteChan %s/%s to %d/%d\n",
    //loc, chan, iRecord, iMaxRecord);
    return NULL;
} // WriteChan()

//////////////////////////////////////////////////////////////////////////////
// Reads the seed record at the specified index location
// Nominal index is 0 .. iLoopSize-1, but code will wrap index to legal value
// Return NULL if no errors
// If no data exists , the first byte of databuf will be null
// Returns error string if something bad happened.
char *ReadIndex(
        const char  *station,   // station name
        const char  *chan,      // Channel ID
        const char  *loc,       // Location ID
        int         index,      // Index of record to read, will wrap index if needed
        char        *databuf    // Seed record pointer
        )                       // returns NULL or an error string pointer
{
    char  buf_filename[2*MAXCONFIGLINELEN+2];
    char  idx_filename[2*MAXCONFIGLINELEN+2];
    FILE  *fp_buf;
    FILE  *fp_idx;
    int   iRecord;
    int   iMaxRecord;
    int   iSeek;

    // The configuration file must be parsed before this routine can work
    if (parse_state == 0)
    {
        sprintf(looperrstr, "ReadLast: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    // Get names of buffer and index files
    DiskloopFileName(station, loc, chan, ".buf", buf_filename);
    DiskloopFileName(station, loc, chan, ".idx", idx_filename);

    // Make sure that buffer file exists
    if ((fp_buf=fopen(buf_filename, "r")) == NULL)
    {
        // Buffer file does not exist so no records to return
        databuf[0] = 0;
        return NULL;
    }

    // Make sure that index file exists
    if ((fp_idx=fopen(idx_filename, "r")) == NULL)
    {
        // Index file does not exist so no last record to return
        fclose(fp_buf);
        databuf[0] = 0;
        return NULL;
    }

    // Load index info
    if (ParseIndexInfo(fp_idx, &iRecord, &iMaxRecord) != NULL)
    {
        sprintf(looperrstr, "ReadLast: Data format error in %s",
                idx_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    if (iRecord < 0 || iRecord >= iMaxRecord)
    {
        sprintf(looperrstr, "ReadLast: Invalid index 0 <= %d < %d in %s",
                iRecord, iMaxRecord, idx_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    // Normalize user requested index
    if (index < 0)
        index += (1+index/iMaxRecord)*iMaxRecord;
    if (index >= iMaxRecord)
        index = index % iMaxRecord;

    // Get data at the specified index
    iSeek = index * iLoopRecordSize;
    fseek(fp_buf, iSeek, SEEK_SET);
    if (iSeek != ftell(fp_buf))
    {
        sprintf(looperrstr, "ReadLast: Unable to seek to record %d in %s",
                index, buf_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // Failed to seek to required file buffer position

    if (fread(databuf, iLoopRecordSize, 1, fp_buf) != 1)
    {
        sprintf(looperrstr, "ReadLast: Unable to read record %d in %s",
                iRecord, buf_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // Failed to read record

    //fprintf(stdout,"DEBUG ReadLast(%s,%s,%s) return %d/%d\n",
    //station, chan, loc, iRecord, iMaxRecord);
    fclose(fp_buf);
    fclose(fp_idx);
    return NULL;
} // ReadIndex()

//////////////////////////////////////////////////////////////////////////////
// Read the last record of data written to this channel
// Required to implement the MSA_GETARC callback from q330lib
char *ReadLast(
        const char  *station,   // station name
        const char  *chan,      // Channel ID
        const char  *loc,       // Location ID
        char        *databuf    // Seed record pointer
        )                       // returns NULL or an error string pointer
{
    char  buf_filename[2*MAXCONFIGLINELEN+2];
    char  idx_filename[2*MAXCONFIGLINELEN+2];
    FILE  *fp_buf;
    FILE  *fp_idx;
    int   iRecord;
    int   iMaxRecord;
    int   iSeek;

    // The configuration file must be parsed before this routine can work
    if (parse_state == 0)
    {
        sprintf(looperrstr, "ReadLast: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }


    // Get names of buffer and index files
    DiskloopFileName(station, loc, chan, ".buf", buf_filename);
    DiskloopFileName(station, loc, chan, ".idx", idx_filename);

    // Make sure that buffer file exists
    if ((fp_buf=fopen(buf_filename, "r")) == NULL)
    {
        // Buffer file does not exist so no last record to return
        databuf[0] = 0;
        return NULL;
    }

    // Make sure that index file exists
    if ((fp_idx=fopen(idx_filename, "r")) == NULL)
    {
        // Index file does not exist so no last record to return
        fclose(fp_buf);
        databuf[0] = 0;
        return NULL;
    }

    // Load index info
    if (ParseIndexInfo(fp_idx, &iRecord, &iMaxRecord) != NULL)
    {
        sprintf(looperrstr, "ReadLast: Data format error in %s",
                idx_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    if (iRecord < 0 || iRecord >= iMaxRecord)
    {
        sprintf(looperrstr, "ReadLast: Invalid index 0 <= %d < %d in %s",
                iRecord, iMaxRecord, idx_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    // Get data at the specified index
    iSeek = iRecord * iLoopRecordSize;
    fseek(fp_buf, iSeek, SEEK_SET);
    if (iSeek != ftell(fp_buf))
    {
        sprintf(looperrstr, "ReadLast: Unable to seek to record %d in %s",
                iRecord, buf_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // Failed to seek to required file buffer position

    if (fread(databuf, iLoopRecordSize, 1, fp_buf) != 1)
    {
        sprintf(looperrstr, "ReadLast: Unable to read record %d in %s",
                iRecord, buf_filename);
        databuf[0] = 0;
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // Failed to read record

    //fprintf(stdout,"DEBUG ReadLast(%s,%s,%s) return %d/%d\n",
    //station, chan, loc, iRecord, iMaxRecord);
    fclose(fp_buf);
    fclose(fp_idx);
    return NULL;
} // ReadLast()

//////////////////////////////////////////////////////////////////////////////
// Read the last record of data written to this channel
char *GetRecordRange(
        const char  *station,   // station name
        const char  *chan,      // Channel ID
        const char  *loc,       // Location ID
        STDTIME2    tBeginTime, // Start time
        STDTIME2    tEndTime,   // End time
        int         *iFirst,    // Returns the first record index within the time
        int         *iLast,     // Returns the last record index within the time
        int         *iCount,    // Returns number of records within the time
        int         *iLoopSize  // Returns size of circular buffer in records
        )                       // returns NULL or an error string pointer
// iFirst == -1 if no records were found
{
    char  buf_filename[2*MAXCONFIGLINELEN+2];
    char  idx_filename[2*MAXCONFIGLINELEN+2];
    char  str_header[FRAME_SIZE];
    char    recStation[8];
    char    recLoc[4];
    char    recChan[4];
    FILE  *fp_buf;
    FILE  *fp_idx;
    STDTIME2    tRecStart;
    STDTIME2    tRecEnd;
    int   iFlipRecord;
    int   iMaxRecord;
    int   iRecord;
    int   iSeek;
    int   iHigh;
    int   iLow;
    int   iMid;
    int   iPre;
    int   iPost;
    int   iCmp;
    int   iSeqNum;
    int   iSamples;

    *iFirst = *iLast = *iLoopSize = -1;
    *iCount = 0;

    // The configuration file must be parsed before this routine can work
    if (parse_state == 0)
    {
        sprintf(looperrstr, "GetRecordRange: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    // Get names of buffer and index files
    DiskloopFileName(station, loc, chan, ".buf", buf_filename);
    DiskloopFileName(station, loc, chan, ".idx", idx_filename);

    // Make sure that buffer file exists
    if ((fp_buf=fopen(buf_filename, "r")) == NULL)
    {
        // Buffer file does not exist so no last record to return
        return NULL;
    }

    // Make sure that index file exists
    if ((fp_idx=fopen(idx_filename, "r")) == NULL)
    {
        // Index file does not exist so no last record to return
        fclose(fp_buf);
        return NULL;
    }

    // Load index info
    if (ParseIndexInfo(fp_idx, &iFlipRecord, &iMaxRecord) != NULL)
    {
        sprintf(looperrstr, "GetRecordRange: Data format error in %s",
                idx_filename);
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    // Skip records just ahead of write point as a safety buffer
    iLow = LOOPDEADRECORDS+1;
    iHigh = iMaxRecord;

    // Check for special case of circular buffer has not yet been filled once
    // If missing records would compose our dead space we don't care
    fseek(fp_buf, 0, SEEK_END);
    iSeek = ftell(fp_buf);
    if ((iMaxRecord-LOOPDEADRECORDS) * iLoopRecordSize > iSeek)
    {
        iMaxRecord = iSeek/iLoopRecordSize;
        iHigh = iMaxRecord;
        iLow = 1;
    }

    *iLoopSize = iMaxRecord;
    if (iFlipRecord < 0 || iFlipRecord >= iMaxRecord)
    {
        sprintf(looperrstr, "GetRecordRange: Invalid index 0 <= %d < %d in %s",
                iFlipRecord, iMaxRecord, idx_filename);
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    //
    // Binary search for last record with End time before tBeginTime
    while (iLow <= iHigh)
    {
        // Get mid point for our binary search test
        iMid = (iHigh+iLow)/2;

        // Convert iMid to a record offset inside circular buffer
        iRecord = (iFlipRecord+iMid) % iMaxRecord;

        // Seek to the record position
        iSeek = iRecord * iLoopRecordSize;
        fseek(fp_buf, iSeek, SEEK_SET);
        if (iSeek != ftell(fp_buf))
        {
            // Seek should never fail if we set up correctly
            sprintf(looperrstr, "GetRecordRange: Unable to seek to record %d in %s",
                    iRecord, buf_filename);
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // Failed to seek to required file buffer position

        // Read in the header only
        if (fread(str_header, FRAME_SIZE, 1, fp_buf) != 1)
        {
            // read should never fail if we set up correctly
            sprintf(looperrstr, "GetRecordRange: Unable to read record %d in %s",
                    iRecord, buf_filename);
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // Failed to read record

        // parse out header string
        if (ParseSeedHeader(str_header, recStation, recChan, recLoc,
                    &tRecStart, &tRecEnd, &iSeqNum, &iSamples) != NULL)
        {
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // error parsing header

        // See if record End Time is after desired start time
        if ((iCmp=ST_TimeComp2(tRecEnd, tBeginTime)) >= 0)
        {
            // Need to find an earlier record
            iHigh = iMid-1;
        }
        else
        {
            // Need to find a later record
            iLow = iMid+1;
        }
    } // Binary search while loop

    // iHigh now points to first record completely before time span of interest
    iPre = iHigh;

    //
    // Binary search for first record with Start time after tEndTime
    iLow = iPre+1;
    iHigh = iMaxRecord;
    while (iLow <= iHigh)
    {
        // Get mid point for our binary search test
        iMid = (iHigh+iLow)/2;

        // Convert iMid to a record offset inside circular buffer
        iRecord = (iFlipRecord+iMid) % iMaxRecord;

        // Seek to the record position
        iSeek = iRecord * iLoopRecordSize;
        fseek(fp_buf, iSeek, SEEK_SET);
        if (iSeek != ftell(fp_buf))
        {
            sprintf(looperrstr, "GetRecordRange: Unable to seek to record %d in %s",
                    iRecord, buf_filename);
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // Failed to seek to required file buffer position

        // Read in the header only
        if (fread(str_header, FRAME_SIZE, 1, fp_buf) != 1)
        {
            // read should never fail if we set up correctly
            sprintf(looperrstr, "GetRecordRange: Unable to read record %d in %s",
                    iRecord, buf_filename);
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // Failed to read record

        // parse out header string
        if (ParseSeedHeader(str_header, recStation, recChan, recLoc,
                    &tRecStart, &tRecEnd, &iSamples, &iSeqNum) != NULL)
        {
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // error parsing header

        // See if record Start time is after desired end time
        if (ST_TimeComp2(tRecStart, tEndTime) > 0)
        {
            // Need to find an earlier record
            iHigh = iMid-1;
        }
        else
            iLow = iMid+1;
    } // Binary search while loop

    // iLow points to first record completely after time span of interest
    iPost = iLow;

    // Check for case of no records within desired time span
    *iCount = iPost - iPre - 1;
    if (*iCount < 1)
    {
        *iFirst = -1;
        *iCount = 0;
    }
    else
    {
        *iFirst = (iPre + 1 + iFlipRecord) % iMaxRecord;
        *iLast = (iPost - 1 + iFlipRecord) % iMaxRecord;
    }

    fclose(fp_buf);
    fclose(fp_idx);

    return NULL;
} // GetRecordRange()

//////////////////////////////////////////////////////////////////////////////
// Print a list of all the spans for the given channel
char *DumpSpans(
        const char  *station,   // station name
        const char  *chan,      // Channel ID
        const char  *loc        // Location ID
        )                       // returns NULL or an error string pointer
{
    char  buf_filename[2*MAXCONFIGLINELEN+2];
    char  idx_filename[2*MAXCONFIGLINELEN+2];
    char  str_header[FRAME_SIZE];
    char    recStation[8];
    char    recLoc[4];
    char    recChan[4];
    FILE  *fp_buf;
    FILE  *fp_idx;
    STDTIME2    tRecStart;
    STDTIME2    tRecEnd;
    DELTA_T2    tDeltaT;
    long        lDeltaTMS;
    int   iFlipRecord;
    int   iMaxRecord;
    int   iRecord;
    int   iSeek;
    int   iMid;
    int   iSpanIndex;
    int   bFirst=1;
    int   iCount;
    int   iSeqNum;
    int   iSamples;
    STDTIME2    tSpanStart;
    STDTIME2    tSpanEnd;


    // The configuration file must be parsed before this routine can work
    if (parse_state == 0)
    {
        sprintf(looperrstr, "DumpSpans: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    // Get names of buffer and index files
    DiskloopFileName(station, loc, chan, ".buf", buf_filename);
    DiskloopFileName(station, loc, chan, ".idx", idx_filename);

    // Make sure that buffer file exists
    if ((fp_buf=fopen(buf_filename, "r")) == NULL)
    {
        // Buffer file does not exist so no last record to return
        return NULL;
    }

    // Make sure that index file exists
    if ((fp_idx=fopen(idx_filename, "r")) == NULL)
    {
        // Index file does not exist so no last record to return
        fclose(fp_buf);
        return NULL;
    }

    // Load index info
    if (ParseIndexInfo(fp_idx, &iFlipRecord, &iMaxRecord) != NULL)
    {
        sprintf(looperrstr, "DumpSpans: Data format error in %s",
                idx_filename);
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    if (iFlipRecord < 0 || iFlipRecord >= iMaxRecord)
    {
        sprintf(looperrstr, "DumpSpans: Invalid index 0 <= %d < %d in %s",
                iFlipRecord, iMaxRecord, idx_filename);
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    // Verify what the last record is in case buffer has never been filled
    iMid = LOOPDEADRECORDS+1;
    fseek(fp_buf, 0, SEEK_END);
    iSeek = ftell(fp_buf);
    if ((iMaxRecord-LOOPDEADRECORDS) * iLoopRecordSize > iSeek)
    {
        printf("Circular buffer filling, %d of %d records total filled.\n",
                iSeek/iLoopRecordSize, iMaxRecord);
        iMaxRecord = iSeek/iLoopRecordSize;
        iMid = 1;
    }

    // Loop through all records in file
    bFirst = 1;
    iCount = 0;
    for (; iMid <= iMaxRecord; iMid++)
    {
        // Convert iMid to a record offset inside circular buffer
        iRecord = (iFlipRecord+iMid) % iMaxRecord;
        if (bFirst)
            iSpanIndex = iRecord;

        // Seek to the record position
        iSeek = iRecord * iLoopRecordSize;
        fseek(fp_buf, iSeek, SEEK_SET);
        if (iSeek != ftell(fp_buf))
        {
            // If seek failed, assume we hit the end of file
            sprintf(looperrstr, "DumpSpans: Unable to seek to header %d in %s",
                    iRecord, buf_filename);
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // Failed to seek to required file buffer position

        // Read in the header only
        if (fread(str_header, FRAME_SIZE, 1, fp_buf) != 1)
        {

            sprintf(looperrstr, "DumpSpans: Unable to read header %d in %s",
                    iRecord, buf_filename);
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // Failed to read record

        // parse out header info
        if (ParseSeedHeader(str_header, recStation, recChan, recLoc,
                    &tRecStart, &tRecEnd, &iSeqNum, &iSamples) != NULL)
        {
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // error parsing header

        // If not first record, check for gap
        if (!bFirst)
        {
            tDeltaT = ST_DiffTimes2(tRecStart, tSpanEnd);
            lDeltaTMS = ST_DeltaToMS2(tDeltaT);

            if (lDeltaTMS > 10 || lDeltaTMS < -10)
            {
                printf("%s %s/%s Span %s",
                        station, loc, chan, ST_PrintDate2(tSpanStart, 1));
                printf(" to %s %d records, start index %d, %ld tms gap\n",
                        ST_PrintDate2(tSpanEnd, 1), iCount, iSpanIndex, lDeltaTMS);

                // Remember new span start time
                tSpanStart = tRecStart;
                iSpanIndex = iRecord;
                iCount=0;
            }  // found a gap or overlap
        } // Not the first record
        else
        {
            tSpanStart = tRecStart;
            bFirst = 0;
        } // first record

        tSpanEnd = tRecEnd;
        iCount++;
    } // loop through all records

    // Print out last remaining span
    printf("%s %s/%s Span %s",
            station, loc, chan, ST_PrintDate2(tSpanStart, 1));
    printf(" to %s %d records, start index %d\n",
            ST_PrintDate2(tSpanEnd, 1), iCount, iSpanIndex);

    fclose(fp_buf);
    fclose(fp_idx);

    return NULL;
} // DumpSpans()

//////////////////////////////////////////////////////////////////////////////
// Print a list of all the spans for the given channel
char *RangeSpans(
        const char  *station,    // station name
        const char  *chan,       // Channel ID
        const char  *loc,        // Location ID
        int         firstRecord, // First record in range
        int         lastRecord   // Last record in range
        )                        // returns NULL or an error string pointer
{
    char  buf_filename[2*MAXCONFIGLINELEN+2];
    char  idx_filename[2*MAXCONFIGLINELEN+2];
    char  str_header[FRAME_SIZE];
    char    recStation[8];
    char    recLoc[4];
    char    recChan[4];
    FILE  *fp_buf;
    FILE  *fp_idx;
    STDTIME2    tRecStart;
    STDTIME2    tRecEnd;
    DELTA_T2    tDeltaT;
    long        lDeltaTMS;
    int   iFlipRecord;
    int   iMaxRecord;
    int   iRecord;
    int   iSeek;
    int   iMid;
    int   iSpanIndex;
    int   bFirst=1;
    int   iCount;
    int   iSeqNum;
    int   iSamples;
    STDTIME2    tSpanStart;
    STDTIME2    tSpanEnd;


    // The configuration file must be parsed before this routine can work
    if (parse_state == 0)
    {
        sprintf(looperrstr, "RangeSpans: ParseDiskLoopConfig not run yet");
        return looperrstr;
    }

    // Get names of buffer and index files
    DiskloopFileName(station, loc, chan, ".buf", buf_filename);
    DiskloopFileName(station, loc, chan, ".idx", idx_filename);

    // Make sure that buffer file exists
    if ((fp_buf=fopen(buf_filename, "r")) == NULL)
    {
        // Buffer file does not exist so no last record to return
        return NULL;
    }

    // Make sure that index file exists
    if ((fp_idx=fopen(idx_filename, "r")) == NULL)
    {
        // Index file does not exist so no last record to return
        fclose(fp_buf);
        return NULL;
    }

    // Load index info
    if (ParseIndexInfo(fp_idx, &iFlipRecord, &iMaxRecord) != NULL)
    {
        sprintf(looperrstr, "RangeSpans: Data format error in %s",
                idx_filename);
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    if (iFlipRecord < 0 || iFlipRecord >= iMaxRecord)
    {
        sprintf(looperrstr, "RangeSpans: Invalid index 0 <= %d < %d in %s",
                iFlipRecord, iMaxRecord, idx_filename);
        fclose(fp_buf);
        fclose(fp_idx);
        return looperrstr;
    } // error reading index and max record value

    // Loop through all records in file
    bFirst = 1;
    iCount = 0;
    for (iMid=firstRecord; (iMid%iMaxRecord) != ((lastRecord+1)%iMaxRecord);
            iMid++)
    {
        // Convert iMid to a record offset inside circular buffer
        iRecord = iMid % iMaxRecord;
        if (bFirst)
            iSpanIndex = iRecord;

        // Seek to the record position
        iSeek = iRecord * iLoopRecordSize;
        fseek(fp_buf, iSeek, SEEK_SET);
        if (iSeek != ftell(fp_buf))
        {
            // If seek failed, assume we hit the end of file
            sprintf(looperrstr, "RangeSpans: Unable to seek to header %d in %s",
                    iRecord, buf_filename);
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // Failed to seek to required file buffer position

        // Read in the header only
        if (fread(str_header, FRAME_SIZE, 1, fp_buf) != 1)
        {

            sprintf(looperrstr, "RangeSpans: Unable to read header %d in %s",
                    iRecord, buf_filename);
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // Failed to read record

        // parse out header info
        if (ParseSeedHeader(str_header, recStation, recChan, recLoc,
                    &tRecStart, &tRecEnd, &iSeqNum, &iSamples) != NULL)
        {
            fclose(fp_buf);
            fclose(fp_idx);
            return looperrstr;
        } // error parsing header

        // If not first record, check for gap
        if (!bFirst)
        {
            tDeltaT = ST_DiffTimes2(tRecStart, tSpanEnd);
            lDeltaTMS = ST_DeltaToMS2(tDeltaT);

            if (lDeltaTMS > 10 || lDeltaTMS < -10)
            {
                printf("%s %s/%s Span %s",
                        station, loc, chan, ST_PrintDate2(tSpanStart, 1));
                printf(" to %s %d records, start index %d, %ld tms gap\n",
                        ST_PrintDate2(tSpanEnd, 1), iCount, iSpanIndex, lDeltaTMS);

                // Remember new span start time
                tSpanStart = tRecStart;
                iSpanIndex = iRecord;
                iCount=0;
            }  // found a gap or overlap
        } // Not the first record
        else
        {
            tSpanStart = tRecStart;
            bFirst = 0;
        } // first record

        tSpanEnd = tRecEnd;
        iCount++;
    } // loop through all records

    // Print out last remaining span
    printf("%s %s/%s Span %s",
            station, loc, chan, ST_PrintDate2(tSpanStart, 1));
    printf(" to %s %d records, start index %d\n",
            ST_PrintDate2(tSpanEnd, 1), iCount, iSpanIndex);

    fclose(fp_buf);
    fclose(fp_idx);

    return NULL;
} // RangeSpans()

