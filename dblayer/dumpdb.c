#include <stdio.h>
#include <stdlib.h>
#include "codec.h"
#include "tbl.h"
#include "util.h"
#include "../pflayer/pf.h"
#include "../amlayer/am.h"
#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(1);}}


void
printRow(void *callbackObj, RecId rid, byte *row, int len) {
    Schema *schema = (Schema *) callbackObj;
    byte *cursor = row;

    for (int i = 0; i < schema->numColumns; i++) {

        if (i > 0)
            printf(",");

        switch (schema->columns[i]->type) {

        case VARCHAR: {
            char buf[256];
            int slen = DecodeCString(cursor,
                                     buf,
                                     sizeof(buf));

            printf("%s", buf);
            cursor += slen + sizeof(short);
            break;
        }

        case INT: {
            int v = DecodeInt(cursor);
            printf("%d", v);
            cursor += sizeof(int);
            break;
        }

        case LONG: {
            long v = DecodeLong(cursor);
            printf("%ld", v);
            cursor += sizeof(long);
            break;
        }

        default:
            fprintf(stderr, "Unknown type\n");
            exit(1);
        }
    }

    printf("\n");
}
#define DB_NAME "data.db"
#define INDEX_NAME "data.db.0"
	 
void
index_scan(Table *tbl, Schema *schema, int indexFD, int op, int value) {

    int scanDesc =
        AM_OpenIndexScan(indexFD,
                         'i',
                         sizeof(int),
                         op,
                         (char *)&value);

    if (scanDesc < 0) {
        AM_PrintError("AM_OpenIndexScan");
        exit(1);
    }

    while (1) {

        RecId rid = AM_FindNextEntry(scanDesc);

        if (rid == AME_EOF)
            break;

        byte record[4000];

        int len = Table_Get(tbl,
                            rid,
                            record,
                            sizeof(record));

        printRow(schema,
                 rid,
                 record,
                 len);
    }

    AM_CloseIndexScan(scanDesc);
}

int
main(int argc, char **argv) {
    char *schemaTxt = "Country:varchar,Capital:varchar,Population:int";
    Schema *schema = parseSchema(schemaTxt);
    Table *tbl;

    int err = Table_Open(DB_NAME,
                     schema,
                     false,
                     &tbl);

    checkerr(err);
    if (argc == 2 && *(argv[1]) == 's') {
	Table_Scan(tbl,
           schema,
           printRow);
	// invoke Table_Scan with printRow, which will be invoked for each row in the table.
    } else {
	// index scan by default
	int indexFD = PF_OpenFile(INDEX_NAME);
	checkerr(indexFD);

	// Ask for populations less than 100000, then more than 100000. Together they should
	// yield the complete database.
	index_scan(tbl, schema, indexFD, LESS_THAN_EQUAL, 100000);
	index_scan(tbl, schema, indexFD, GREATER_THAN, 100000);
    }
    Table_Close(tbl);
}
