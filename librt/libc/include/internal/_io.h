#ifndef __INTERNAL_IO_H__
#define __INTERNAL_IO_H__

#include <internal/_evt.h>
#include <internal/_ipc.h>
#include <internal/_ioset.h>
#include <internal/_pipe.h>
#include <internal/_socket.h>
#include <os/osdefs.h>
#include <os/spinlock.h>
#include <os/types/process.h>
#include <stdio.h>

#ifndef _IOCOMMIT
#define _IOCOMMIT 0x4000
#endif

// Values for wxflag
#define WX_OPEN             0x01U
#define WX_ATEOF            0x02U
#define WX_READNL           0x04U  // read started with \n
#define WX_READEOF          0x04U  // like ATEOF, but for underlying file rather than buffer
#define WX_PIPE             0x08U
#define WX_READCR           0x10U  // underlying file is at \r
#define WX_DONTINHERIT      0x20U
#define WX_APPEND           0x40U
#define WX_TTY              0x80U
#define WX_TEXT             0x100U
#define WX_WIDE             0x200U
#define WX_UTF              (WX_TEXT | 0x400U)
#define WX_INHERITTED       0x800U
#define WX_PERSISTANT       0x1000U

#define INTERNAL_BUFSIZ     4096
#define INTERNAL_MAXFILES   1024

#define STDIO_HANDLE_INVALID    0
#define STDIO_HANDLE_PIPE       1
#define STDIO_HANDLE_FILE       2
#define STDIO_HANDLE_SOCKET     3
#define STDIO_HANDLE_IPCONTEXT  4
#define STDIO_HANDLE_SET        5
#define STDIO_HANDLE_EVENT      6

typedef struct stdio_handle stdio_handle_t;

// Inheritable handle that is shared with child processes
// should contain only portable information
typedef struct stdio_object {
    UUId_t handle;
    int    type;
    union {
        struct socket    socket;
        struct ipcontext ipcontext;
        struct pipe      pipe;
        struct ioset     ioset;
        struct evt       evt;
    } data;
} stdio_object_t;

#define STDIO_CLOSE_INHERIT 0
#define STDIO_CLOSE_FULL    1

// Stdio descriptor operations
typedef OsStatus_t(*stdio_inherit)(stdio_handle_t*);
typedef OsStatus_t(*stdio_read)(stdio_handle_t*, void*, size_t, size_t*);
typedef OsStatus_t(*stdio_write)(stdio_handle_t*, const void*, size_t, size_t*);
typedef OsStatus_t(*stdio_resize)(stdio_handle_t*, long long);
typedef OsStatus_t(*stdio_seek)(stdio_handle_t*, int, off64_t, long long*);
typedef OsStatus_t(*stdio_ioctl)(stdio_handle_t*, int, va_list);
typedef OsStatus_t(*stdio_close)(stdio_handle_t*, int);

typedef struct stdio_ops {
    stdio_inherit inherit;
    stdio_read    read;
    stdio_write   write;
    stdio_resize  resize;
    stdio_seek    seek;
    stdio_ioctl   ioctl;
    stdio_close   close;
} stdio_ops_t;

// Local to application handle that also handles state, stream and buffer
// support for a handle.
typedef struct stdio_handle {
    int            fd;
    spinlock_t     lock;
    stdio_object_t object;
    stdio_ops_t    ops;
    unsigned short wxflag;
    char           lookahead[3];
    FILE*          buffered_stream;
} stdio_handle_t;

typedef struct stdio_inheritation_block {
    int                 handle_count;
    struct stdio_handle handles[];
} stdio_inheritation_block_t;

// io-object interface
extern int             stdio_handle_create(int, int, stdio_handle_t**);
extern int             stdio_handle_set_handle(stdio_handle_t*, UUId_t);
extern int             stdio_handle_set_ops_type(stdio_handle_t*, int);
extern int             stdio_handle_set_buffered(stdio_handle_t*, FILE*, unsigned int);
extern int             stdio_handle_destroy(stdio_handle_t*, int);
extern int             stdio_handle_activity(stdio_handle_t*, int);
extern stdio_handle_t* stdio_handle_get(int iod);

// io-buffer interface
extern OsStatus_t os_alloc_buffer(FILE* file);
extern OsStatus_t os_flush_buffer(FILE* file);
extern int        os_flush_all_buffers(int mask);
extern OsStatus_t add_std_buffer(FILE* file);
extern void       remove_std_buffer(FILE* file);

// io-operation types
extern void stdio_get_null_operations(stdio_ops_t* ops);
extern void stdio_get_pipe_operations(stdio_ops_t* ops);
extern void stdio_get_file_operations(stdio_ops_t* ops);
extern void stdio_get_net_operations(stdio_ops_t* ops);
extern void stdio_get_ipc_operations(stdio_ops_t* ops);
extern void stdio_get_set_operations(stdio_ops_t* ops);
extern void stdio_get_evt_operations(stdio_ops_t* ops);

// helpers
extern int  stdio_bitmap_initialize(void);
extern int  stdio_bitmap_allocate(int fd);
extern void stdio_bitmap_free(int fd);
extern int  _flsbuf(int ch, FILE *stream);
extern int  _flswbuf(int ch, FILE *stream);

// Must be reentrancy spinlocks (critical sections)
#define LOCK_FILES() do { } while(0)
#define UNLOCK_FILES() do { } while(0)

extern OsStatus_t
StdioCreateInheritanceBlock(
	_In_  ProcessConfiguration_t* Configuration,
    _Out_ void**                  InheritationBlock,
    _Out_ size_t*                 InheritationBlockLength);

#endif //!__INTERNAL_IO_H__
