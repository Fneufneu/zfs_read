zfs_read
========

tool for read and recover files from zfs pool without import it 

Only works on FreeBSD amd64

use FreeBSD bootloader for ZFS access
support ZFSv28 with features
can read data from a corrupted pool like:
status: The pool metadata is corrupted and the pool cannot be opened.
action: Destroy and re-create the pool from
        a backup source.

