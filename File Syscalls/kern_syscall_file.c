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

// NOTE:
////////////////////////////////////////////////////////
//              open file table functions             //
////////////////////////////////////////////////////////

struct open_file_list *open_file_table = NULL;

// Initialize the global open file table
void open_file_table_create() {
    open_file_table = kmalloc(sizeof(struct open_file_list));
    if (open_file_table == NULL) {
        panic("Insufficient memory for open file table\n");
    }

    open_file_table->sentinel = kmalloc(sizeof(struct open_file_node));
    if (open_file_table->sentinel == NULL) {
        panic("Insufficient memory for open file table\n");
    }

    open_file_table->sentinel->prev = open_file_table->sentinel;
    open_file_table->sentinel->next = open_file_table->sentinel;
    open_file_table->sentinel->open_file = NULL;
}

static void free_open_file_node(struct open_file_node *node) {
    vfs_close(node->open_file->vnode);
    lock_destroy(node->open_file->mutex);
    kfree(node);
}

// Destroy the global open file table to prevent memory leak
void open_file_table_destroy() {
    // Free all the nodes
    struct open_file_node *sentinel = open_file_table->sentinel;
    struct open_file_node *curr;
    struct open_file_node *next;
    for (curr = sentinel->next; curr != sentinel; curr = next) {
        next = curr->next;
        free_open_file_node(curr);
    }

    kfree(sentinel);
    kfree(open_file_table);
}

/* Insert a new open file to the end of the open file list, 
and return the address of the node */
struct open_file_node *add_open_file(struct open_file *new) {
    // Malloc a new node
    struct open_file_node *new_node = kmalloc(sizeof(struct open_file_node));
    if (new_node == NULL) {
        kprintf("Insufficient memory for open file node\n");
        vfs_close(new->vnode);
        lock_destroy(new->mutex);
        return NULL;
    }

    new_node->open_file = new;
    struct open_file_node *sentinel = open_file_table->sentinel;
    
    // Add into the linked list
    new_node->next = sentinel;
    new_node->prev = sentinel->prev;
    sentinel->prev = new_node;
    new_node->prev->next = new_node;

    return new_node;
}

// Decrement reference count of the open file that the node contains
void close_open_file(struct open_file_node *node) {
    // Decrement the reference count
    node->open_file->reference_count--;

    // If no reference, remove the node
    if (node->open_file->reference_count == 0) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
        free_open_file_node(node);
    }
}

// Create a new open file
struct open_file *create_open_file() {
    struct open_file *new = kmalloc(sizeof(struct open_file));
    if (new == NULL) {
        kprintf("Insufficient memory for new open file");
        return NULL;
    }

    new->vnode = NULL;
    new->offset = 0;
    new->flags = 0;
    new->reference_count = 1;
    new->mutex = lock_create("mutex");

    return new;
}

// NOTE:
////////////////////////////////////////////////////////
//          file descriptor table functions           //
////////////////////////////////////////////////////////

struct file_descriptor_table *FD_table_create() {
    struct file_descriptor_table *FD_table = kmalloc(sizeof(struct file_descriptor_table));
    if (FD_table == NULL) {
        kprintf("Insufficient memory for file descriptor table");
        return NULL;
    }

    for (int i = 0; i < __OPEN_MAX; i++) {
        FD_table->OF_node_ptr_array[i] = NULL;
    }

    // Connect 1 to stdout, 2 to stderr.
    char stdout_path[] = "con:";
    struct open_file *stdout = create_open_file();
    int errno = vfs_open(stdout_path, O_WRONLY, 0, &stdout->vnode);
    if (errno != 0) {
        panic("stdout opened failed\n");
        return NULL;
    }

    struct open_file_node *new_node = NULL;
    if ((new_node = add_open_file(stdout)) == NULL) {
        panic("Insufficient memory for stdio initialization in fd table");
    } else {
	    FD_table->OF_node_ptr_array[1] = new_node;
        FD_table->OF_node_ptr_array[1]->open_file->flags = O_WRONLY;
    }

    char stderr_path[] = "con:";
    struct open_file *stderr = create_open_file();
    errno = vfs_open(stderr_path, O_WRONLY, 0, &stderr->vnode);
    if (errno != 0) {
        panic("stderr opened failed\n");
        return NULL;
    }
    
    if ((new_node = add_open_file(stderr)) == NULL) {
        panic("Insufficient memory for stdio initialization in fd table");
    } else {
	    FD_table->OF_node_ptr_array[2] = new_node;
        FD_table->OF_node_ptr_array[2]->open_file->flags = O_WRONLY;
    }
	
    // Let the next fd start from 3
    FD_table->next = 3;

    return FD_table;
}

void FD_table_destroy(struct file_descriptor_table *FD_table) {
    for (int i = 0; i < __OPEN_MAX; i++) {
        close_fd(FD_table, i);
    }
    kfree(FD_table);
}

// Check if a fd table is full, 0 if not, 1 if full
int is_fd_table_full(struct file_descriptor_table *FD_table) {
    return FD_table->next == -1;
}

// Return an available fd
int get_next_fd(struct file_descriptor_table *FD_table) {
    int free = FD_table->next;

    // Find the next free fd
    int next_free = free + 1;
    int has_free = 0;
    while (next_free < __OPEN_MAX) {
        // If there is free space, cache it
        if (FD_table->OF_node_ptr_array[next_free] == NULL) {
            FD_table->next = next_free;
            has_free = 1;
            break;
        }
        next_free++;
    }

    // If there's no free space
    if (has_free == 0) {
        FD_table->next = -1;
    }

    return free;
}

// Return the open file given by the fd
struct open_file *get_open_file(struct file_descriptor_table *FD_table, int fd) {
    struct open_file_node *node = FD_table->OF_node_ptr_array[fd];
    if (node != NULL) return node->open_file;
    else return NULL;
}

/* If the fd is linked to an open file, close the fd and decrement
 * open file's reference count.
 */
void close_fd(struct file_descriptor_table *FD_table, int fd) {
    if (FD_table->OF_node_ptr_array[fd] != NULL) {
        close_open_file(FD_table->OF_node_ptr_array[fd]);
        FD_table->OF_node_ptr_array[fd] = NULL;
        FD_table->next = fd;
    }
}

/* Validate if a fd is valid, 0 if valid, -1 if not.
 * A valid fd is within range and has an associated open file
 */
int validate_fd(struct file_descriptor_table *FD_table, int fd) {
    // Need to be within bound
    if (fd < 0 || fd >= __OPEN_MAX) return -1;
    
    // Need to have an entry
    if (FD_table->OF_node_ptr_array[fd] == NULL) return -1;

    return 0;
}

// NOTE:
////////////////////////////////////////////////////////
//                   syscall function                 //
////////////////////////////////////////////////////////

int32_t sys_open(userptr_t filename, int flags, mode_t mode, int *errno) {
    //copy user path into os kernel memory
    int fd = 0;
    char path[__NAME_MAX];
    *errno = copyinstr(filename, path, __NAME_MAX, NULL);
    if(*errno){
        return -1;
    }

    //check if fd_table is full
    struct file_descriptor_table *FD_table = curproc->FD_table;
    if(is_fd_table_full(FD_table)){
        *errno = EMFILE;
        return -1;
    }
    fd = get_next_fd(curproc->FD_table);

    //vfs_open to deal with vnode
    struct open_file *new_open_file = create_open_file();
    *errno = vfs_open(path, flags, mode, &new_open_file->vnode);
    if(*errno){
        return -1;
    }

    //add to open file linked-list and map to fd, record the open flags
    struct open_file_node *new_node = NULL;
    if ((new_node = add_open_file(new_open_file)) == NULL) {
        // If not enough memory, the open file table is full
        *errno = ENFILE;
        return -1;
    }

    // Let the fd points to the new file and set the flags
    curproc->FD_table->OF_node_ptr_array[fd] = new_node;
    curproc->FD_table->OF_node_ptr_array[fd]->open_file->flags = flags;

    if ((flags & O_APPEND) == O_APPEND) {
        struct stat stat;
        VOP_STAT(new_open_file->vnode, &stat);
        new_open_file->offset = stat.st_size;
    }

    return fd;
}

int32_t sys_close(int fd, int *errno){
    if (validate_fd(curproc->FD_table, fd) != 0) {
        *errno = EBADF;
        return -1;
    }

    close_fd(curproc->FD_table, fd);

    return 0;
}

ssize_t sys_read(int fd, userptr_t buf, size_t buflen, int *errno) {
    // Validate fd
    if (validate_fd(curproc->FD_table, fd) != 0) {
        *errno = EBADF;
        return -1;
    }

    // flags need to be one of R or R/W
    struct open_file *file = get_open_file(curproc->FD_table, fd);
    int flags = file->flags;
    if (((flags & O_ACCMODE) != O_RDONLY) && ((flags & O_ACCMODE) != O_RDWR)) {
        *errno = EBADF;
        return -1;
    }

    // Set up struct to be used in vop_read
    struct uio uio;
    struct iovec iovec;
    uio_kinit(&iovec, &uio, buf, buflen, file->offset, UIO_READ);

    lock_acquire(file->mutex);

    *errno = VOP_READ(file->vnode, &uio);
    if (*errno != 0) return -1;

    // Update the offset to the open file
    ssize_t num_bytes_read = buflen - uio.uio_resid;
    file->offset = uio.uio_offset;

    lock_release(file->mutex);

    return num_bytes_read;
}

ssize_t sys_write(int fd, userptr_t buf, size_t nbytes, int *errno) {
    // Validate fd
    if (validate_fd(curproc->FD_table, fd) != 0) {
        *errno = EBADF;
        return -1;
    }

    // flags need to be one of W or R/W
    struct open_file *file = get_open_file(curproc->FD_table, fd);
    int flags = file->flags;
    if (((flags & O_ACCMODE) != O_WRONLY) && ((flags & O_ACCMODE) != O_RDWR)) {
        *errno = EBADF;
        return -1;
    }

    // Set up struct to be used in vop_read
    struct uio uio;
    struct iovec iovec;
    uio_kinit(&iovec, &uio, buf, nbytes, file->offset, UIO_WRITE);

    lock_acquire(file->mutex);

    *errno = VOP_WRITE(file->vnode, &uio);
    if (*errno != 0) return -1;

    // Update the offset to the open file
    ssize_t num_bytes_wrote = nbytes - uio.uio_resid;
    file->offset = uio.uio_offset;

    lock_release(file->mutex);

    return num_bytes_wrote;
}

int sys_dup2(int oldfd, int newfd, int *errno) {
    // Validate fd
    if (validate_fd(curproc->FD_table, oldfd) != 0 || (newfd < 0 || newfd >= __OPEN_MAX)) {
        *errno = EBADF;
        return -1;
    }

    // No effect if the two fd are equal
    if (oldfd == newfd) return newfd;

    struct file_descriptor_table *FD_table = curproc->FD_table;

    // Close the newfd if it is linked to an open file
    if (FD_table->OF_node_ptr_array[newfd] != NULL) {
        close_fd(FD_table, newfd);

        // Move to another free fd
        get_next_fd(FD_table);
    }

    // Let newfd point to where oldfd points, and increment the reference count
    FD_table->OF_node_ptr_array[newfd] = FD_table->OF_node_ptr_array[oldfd];
    
    struct open_file *file = get_open_file(curproc->FD_table, newfd);
    lock_acquire(file->mutex);

    file->reference_count++;

    lock_release(file->mutex);

    return newfd;
}

uint64_t sys_lseek(int fd, uint64_t pos, int whence, int *errno) {
    struct stat stat;
    struct open_file *opf;
    off_t  newpos;

    //check if fd is valid
    if(validate_fd(curproc->FD_table, fd) != 0){
        *errno = EBADF;
        return -1;
    }

    //check if vnode is seekable
    opf = get_open_file(curproc->FD_table, fd);
    if (VOP_ISSEEKABLE(opf->vnode) == 0) {
        *errno = ESPIPE;
        return -1;
    }

    if (VOP_STAT(opf->vnode,&stat) != 0) {
        *errno = EINVAL;
        return -1;
    }

    //case whence
    switch(whence){
        case SEEK_SET:
            newpos = pos;
            break;
        case SEEK_CUR:
            newpos = pos + opf->offset;
            break;
        case SEEK_END:
            newpos = stat.st_size + pos;
            break;
        default:
            *errno = EINVAL;
            return -1;
    }

    if(newpos<0){
        *errno = EINVAL;
        return -1;
    }

    opf->offset = newpos;
    return newpos;
}