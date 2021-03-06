#pragma ident "$Id: init.c,v 1.2 2000/05/01 22:59:02 dec Exp $"
#include <stdio.h>
#include "rdio.h"

main(argc, argv)
int argc;
char *argv[];
{
RDISK *rd;

    if (argc != 2) {
        fprintf(stderr, "usage: %s raw-disk\n", argv[0]);
        exit(1);
    }

    if (!util_query("Are you sure? ")) exit(0);

    if ((rd = rdio_open(argv[1], "w")) == (RDISK *) NULL) {
        fprintf(stderr, "%s: %s: ", argv[0], argv[1]);
        perror("rdio_open");
        exit(1);
    }

    rdio_close(rd);

    exit(0);
}

/* Revision History
 *
 * $Log: init.c,v $
 * Revision 1.2  2000/05/01 22:59:02  dec
 * import existing sources
 *
 */
