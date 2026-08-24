
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
    PF_Init();

    if (overwrite) {
        PF_DestroyFile(dbname);
        PF_CreateFile(dbname);
    }

    int fd = PF_OpenFile(dbname);
    if (fd < 0)
        return fd;

    Table *tbl = malloc(sizeof(Table));

    tbl->schema = schema;
    tbl->fd = fd;

    *ptable = tbl;

    return 0;
}

void
Table_Close(Table *tbl)
{
    PF_CloseFile(tbl->fd);
    free(tbl);
}

int
Table_Insert(Table *tbl, byte *record, int len, RecId *rid)
{
    int pageNum = -1;
    char *pageBuf = NULL;
    int err;

    int required = len + 2 + 2;   // record len + slot entry

    err = PF_GetFirstPage(tbl->fd, &pageNum, &pageBuf);

    while (err == PFE_OK) {

        short freePtr = DecodeShort(pageBuf);
        short nslots  = DecodeShort(pageBuf + 2);

        int headerEnd = 4 + 2 * nslots;
        int freeSpace = freePtr - headerEnd;

        if (freeSpace >= required)
            break;

        PF_UnfixPage(tbl->fd, pageNum, FALSE);
        err = PF_GetNextPage(tbl->fd, &pageNum, &pageBuf);
    }

    if (err == PFE_EOF) {

        err = PF_AllocPage(tbl->fd, &pageNum, &pageBuf);
        if (err < 0)
            return err;

        EncodeShort(PF_PAGE_SIZE, pageBuf);
        EncodeShort(0, pageBuf + 2);
    }

    short freePtr = DecodeShort(pageBuf);
    short nslots  = DecodeShort(pageBuf + 2);

    short newOffset = freePtr - (len + 2);

    EncodeShort(len, pageBuf + newOffset);
    memcpy(pageBuf + newOffset + 2, record, len);

    EncodeShort(newOffset,
                pageBuf + 4 + 2 * nslots);

    EncodeShort(nslots + 1,
                pageBuf + 2);

    EncodeShort(newOffset,
                pageBuf);

    *rid = (pageNum << 16) | nslots;

    PF_UnfixPage(tbl->fd, pageNum, TRUE);

    return 0;
}

#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(EXIT_FAILURE);}}

/*
  Given an rid, fill in the record (but at most maxlen bytes).
  Returns the number of bytes copied.
 */
int
Table_Get(Table *tbl, RecId rid, byte *record, int maxlen)
{
    int slot = rid & 0xFFFF;
    int pageNum = rid >> 16;

    char *pageBuf;
    int err;

    err = PF_GetThisPage(tbl->fd,
                         pageNum,
                         &pageBuf);

    if (err < 0)
        return err;

    short offset =
        DecodeShort(pageBuf + 4 + 2 * slot);

    short len =
        DecodeShort(pageBuf + offset);

    if (len > maxlen)
        len = maxlen;

    memcpy(record,
           pageBuf + offset + 2,
           len);

    PF_UnfixPage(tbl->fd,
                 pageNum,
                 FALSE);

    return len;
}

void
Table_Scan(Table *tbl, void *callbackObj, ReadFunc callbackfn)
{
    int pageNum;
    char *pageBuf;

    int err =
        PF_GetFirstPage(tbl->fd,
                        &pageNum,
                        &pageBuf);

    while (err == PFE_OK) {

        short nslots =
            DecodeShort(pageBuf + 2);

        for (int slot = 0;
             slot < nslots;
             slot++) {

            short offset =
                DecodeShort(pageBuf + 4 + 2 * slot);

            short len =
                DecodeShort(pageBuf + offset);

            RecId rid =
                (pageNum << 16) | slot;

            callbackfn(callbackObj,
                       rid,
                       (byte *)(pageBuf + offset + 2),
                       len);
        }

        PF_UnfixPage(tbl->fd,
                     pageNum,
                     FALSE);

        err =
            PF_GetNextPage(tbl->fd,
                           &pageNum,
                           &pageBuf);
    }
}


short getFreePtr(byte *page)
{
    return DecodeShort(page);
}

void setFreePtr(byte *page, short ptr)
{
    EncodeShort(ptr, page);
}

int getNumSlots(byte *page)
{
    return DecodeShort(page + SLOT_COUNT_OFFSET);
}

void setNumSlots(byte *page, int nslots)
{
    EncodeShort((short)nslots,
                page + SLOT_COUNT_OFFSET);
}

int getNthSlotOffset(int slot, char *page)
{
    return DecodeShort(page + 4 + 2*slot);
}

void setNthSlotOffset(int slot,
                      short offset,
                      char *page)
{
    EncodeShort(offset,
                page + 4 + 2*slot);
}

int getLen(int slot, byte *page)
{
    int off = getNthSlotOffset(slot, page);
    return DecodeShort(page + off);
}