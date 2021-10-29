/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>
#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <lib.h>
#include <uio.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>

#include <proc.h>
#include <spinlock.h>

////////////////////////////////////////////////////////
//              open file table structures            //
////////////////////////////////////////////////////////

// The open file table struct, a doubly linked list
struct open_file_list {
    struct open_file_node *sentinel;
};

struct open_file_node {
    struct open_file_node *prev;
    struct open_file_node *next;

    struct open_file *open_file;
};

// Open file details, stored in the node
struct open_file{
    struct vnode    *vnode;      

    // Bookkeeping info.
    off_t           offset;         // #offset in the vnode
    int             flags;
    int             reference_count;

    // Lock
    struct lock *mutex;
};

// open file list relate functions
void open_file_table_create(void);
void open_file_table_destroy(void);
struct open_file_node *add_open_file(struct open_file *new);
void close_open_file(struct open_file_node *node);

// open file entry relate functions
struct open_file *create_open_file(void);

////////////////////////////////////////////////////////
//         file descriptor table structures           //
////////////////////////////////////////////////////////

// Per process FD_table
struct file_descriptor_table {
    // the fd table contains pointers to node 
    // node points to the details of the open file
    struct open_file_node *OF_node_ptr_array[__OPEN_MAX];
    // struct open_file_node **OF_node_ptr_array;

    int next;    // -1 if full
};

struct file_descriptor_table * FD_table_create(void);
void FD_table_destroy(struct file_descriptor_table *FD_table);
int is_fd_table_full(struct file_descriptor_table *FD_table);
int get_next_fd(struct file_descriptor_table *FD_table);
struct open_file *get_open_file(struct file_descriptor_table *FD_table, int fd);
void close_fd(struct file_descriptor_table *FD_table, int fd);
int validate_fd(struct file_descriptor_table *FD_table, int fd);

////////////////////////////////////////////////////////
//              syscall function prototypes           //
////////////////////////////////////////////////////////

/****************************************************/
/*
 * Put your function declarations and data types here ...
 */

/* 
 * open(char *filename, int flags, mode_t mode)
 * int close(int fd)
 * ssize_t read(int fd, void *buf, size_t buflen)
 * ssize_t write(int fd, const void *buf, size_t nbytes);
 * off_t lseek(int fd, off_t pos, int whence);
 * int dup2(int oldfd, int newfd);
 */

int32_t sys_open(userptr_t filename, int flags, mode_t mode, int *errno);
int32_t sys_close(int fd, int *errno);
ssize_t sys_read(int fd, userptr_t buf, size_t buflen, int *errno);
ssize_t sys_write(int fd, userptr_t buf, size_t nbytes, int *errno); 
uint64_t sys_lseek(int fd, uint64_t pos, int whence, int *errno);
int sys_dup2(int oldfd, int newfd, int *errno);

#endif /* _FILE_H_ */
