# OS-COMP3231
> COMP3231 Assignments: Implementation of the file-related syscalls and the virtual memory subsystem.

## File-Related Syscalls

Functionalities

* Book-keeping using per-process file descriptor and global open file table.
* Implemented `sys-open`, `sys-close`, `sys-lseek`, `sys-read`, `sys-write`, `sys-dup2`
    * Book-keeping with open file table entries
    * Adapt VFS interface to syscall interface

## Virtual Memory Subsytem

Functionalities

* Per-process address space management: Book-keeping address space sections
* Per-process address translation using 3-level page table
* TLB management: Write mapping entries to TLB

