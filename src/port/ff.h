// ff.h for SMS Plus's system.c, which saves and loads its states through
// FatFs calls: here a FIL is whatever the caller makes of it (the SMS
// plugin streams a state to and from the card's sectors, test/sms_host.c
// to and from memory). Only what system.c uses.
#pragma once
#include <stddef.h>

typedef unsigned int UINT;
typedef enum { FR_OK = 0, FR_DISK_ERR = 1 } FRESULT;

typedef struct FIL {
    // move n bytes between buf and wherever the FIL is (write: from buf);
    // returns how many it moved
    UINT (*io)(struct FIL *f, void *buf, UINT n, int write);
    void *ctx;
} FIL;

static inline FRESULT f_read(FIL *f, void *buf, UINT n, UINT *done) {
    *done = f->io(f, buf, n, 0);
    return *done == n ? FR_OK : FR_DISK_ERR;
}

static inline FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *done) {
    *done = f->io(f, (void *)buf, n, 1);
    return *done == n ? FR_OK : FR_DISK_ERR;
}
