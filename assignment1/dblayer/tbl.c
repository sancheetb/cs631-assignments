

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "tbl.h"
#include "codec.h"
#include "../pflayer/pf.h"

#define SLOT_COUNT_OFFSET 2
#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(EXIT_FAILURE);}}

int  getLen(int slot, byte *pageBuf);
int  getNumSlots(byte *pageBuf);
void setNumSlots(byte *pageBuf, int nslots);
int  getNthSlotOffset(int slot, char* pageBuf);




/**
   Opens a paged file, creating one if it doesn't exist, and optionally
   overwriting it.
   Returns 0 on success and a negative error code otherwise.
   If successful, it returns an initialized Table*.
 */
int
Table_Open(char *dbname, Schema *schema, bool overwrite, Table **ptable)
{
    // Initialize PF, create PF file,
    // allocate Table structure  and initialize and return via ptable
    // The Table structure only stores the schema. The current functionality
    // does not really need the schema, because we are only concentrating
    // on record storage. 

    PF_Init();

    if(overwrite){
        PF_DestroyFile(dbname);
        PF_CreateFile(dbname);
    }

    int fd = PF_OpenFile(dbname);

    if(fd < 0) return fd;

    Table *table = malloc(sizeof(Table));
    table->schema = schema;
    table->fd = fd;

    *ptable = table;
    return 0;
}

void
Table_Close(Table *tbl) {
    // Unfix any dirty pages, close file.
    PF_CloseFile(tbl->fd);
    free(tbl);
}


int
Table_Insert(Table *tbl, byte *record, int len, RecId *rid) {
    // Allocate a fresh page if len is not enough for remaining space
    // Get the next free slot on page, and copy record in the free
    // space
    // Update slot and free space index information on top of page.

    int pg_num = -1; 
    byte *pg_buffer = NULL;

    int length_size = 2, offset_size = 2;
    int req = length_size + offset_size + len;

    int err = PF_GetFirstPage(tbl->fd, &pg_num, &pg_buffer);

    while(err == PFE_OK) {
        short free_ptr = DecodeShort(pg_buffer);
        short nslots = DecodeShort(pg_buffer + 2);

        int block_end = 4 + 2*nslots;
        int free_space = free_ptr - block_end;

        if(free_space >= req) break;

        PF_UnfixPage(tbl->fd, pg_num, FALSE);
        err = PF_GetNextPage(tbl->fd, &pg_num, &pg_buffer);
    }

    if(err == PFE_EOF) {
        err = PF_AllocPage(tbl->fd, &pg_num, &pg_buffer);
        if(err < 0) return err;
        EncodeShort(PF_PAGE_SIZE, pg_buffer);
        EncodeShort(0, pg_buffer + 2);
    }
    
    short free_ptr = DecodeShort(pg_buffer);
    short nslots = DecodeShort(pg_buffer + 2);
    short offset = free_ptr - (len + 2);
    
    EncodeShort(len, pg_buffer+offset);
    memcpy(pg_buffer+offset+2, record, len);

    EncodeShort(offset, pg_buffer+ 4 + 2*nslots);
    EncodeShort(nslots+1, pg_buffer + 2);
    EncodeShort(offset, pg_buffer);

    *rid = (pg_num << 16) | nslots;

    PF_UnfixPage(tbl->fd, pg_num, TRUE);
    return 0;
}

#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(EXIT_FAILURE);}}

/*
  Given an rid, fill in the record (but at most maxlen bytes).
  Returns the number of bytes copied.
 */
int
Table_Get(Table *tbl, RecId rid, byte *record, int maxlen) {
    int slot = rid & 0xFFFF;
    int pageNum = rid >> 16;

    // PF_GetThisPage(pageNum)
    // In the page get the slot offset of the record, and
    // memcpy bytes into the record supplied.
    // Unfix the page

    byte *pg_buffer;
    int err = PF_GetThisPage(tbl->fd, pageNum, &pg_buffer);
    if(err < 0) return err;

    short offset = DecodeShort(pg_buffer + 4 + 2*slot);
    short len = DecodeShort(pg_buffer + offset);

    if(len > maxlen) len = maxlen;
    memcpy(record, pg_buffer + offset + 2, len);

    PF_UnfixPage(tbl->fd, pageNum, FALSE);

    return len; // return size of record
}

void
Table_Scan(Table *tbl, void *callbackObj, ReadFunc callbackfn) {

    // For each page obtained using PF_GetFirstPage and PF_GetNextPage
    //    for each record in that page,
    //          callbackfn(callbackObj, rid, record, recordLen)

    int pg_num;
    byte *pg_buffer;
    int err = PF_GetFirstPage(tbl->fd, &pg_num, &pg_buffer);

    while(err == PFE_OK){
        short nslots = DecodeShort(pg_buffer+2);
        
        for(int i=0; i<nslots; i++){
            short offset = DecodeShort(pg_buffer + 4 + 2*i);
            short len = DecodeShort(pg_buffer + offset);
            RecId rid = (pg_num << 16) | i;
            callbackfn(callbackObj, rid, (byte*)(pg_buffer + offset + 2), len);
        }

        PF_UnfixPage(tbl->fd, pg_num, FALSE);
        err = PF_GetNextPage(tbl->fd, &pg_num, &pg_buffer);
    }
}


