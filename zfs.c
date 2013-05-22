/*-
 * Copyright (c) 2007 Doug Rabson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$FreeBSD: stable/9/sys/boot/zfs/zfs.c 243243 2012-11-18 17:09:29Z ae $
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: stable/9/sys/boot/zfs/zfs.c 243243 2012-11-18 17:09:29Z ae $");

/*
 *	Stand-alone file reading package.
 */
#include <stdlib.h>
#include <sys/disk.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <part.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stand.h>
#include <bootstrap.h>
#include <sysexits.h>

#include "libzfs.h"

#include "zfsimpl.c"

#undef SOPEN_RASIZE
#define	SOPEN_RASIZE	1024 * 128

static int	zfs_open(const char *path, struct open_file *f);
static int	zfs_write(struct open_file *f, void *buf, size_t size, size_t *resid);
static int	zfs_close(struct open_file *f);
static int	zfs_read(struct open_file *f, void *buf, size_t size, size_t *resid);
static off_t	zfs_seek(struct open_file *f, off_t offset, int where);
static int	zfs_stat(struct open_file *f, struct stat *sb);
static int	zfs_readdir(struct open_file *f, struct dirent *d);
static void		i386_zfs_probe(void);

struct devsw zfs_dev;

struct fs_ops zfs_fsops = {
	"zfs",
	zfs_open,
	zfs_close,
	zfs_read,
	zfs_write,
	zfs_seek,
	zfs_stat,
	zfs_readdir
};

/*
 * In-core open file.
 */
struct file {
	off_t		f_seekp;	/* seek pointer */
	dnode_phys_t	f_dnode;
	uint64_t	f_zap_type;	/* zap type for readdir */
	uint64_t	f_num_leafs;	/* number of fzap leaf blocks */
	zap_leaf_phys_t	*f_zap_leaf;	/* zap leaf buffer */
};

/*
 * Open a file.
 */
static int
zfs_open(const char *upath, struct open_file *f)
{
	struct zfsmount *mount = (struct zfsmount *)f->f_devdata;
	struct file *fp;
	int rc;

	if (f->f_dev != &zfs_dev)
		return (EINVAL);

	/* allocate file system specific data structure */
	fp = malloc(sizeof(struct file));
	bzero(fp, sizeof(struct file));
	f->f_fsdata = (void *)fp;

	rc = zfs_lookup(mount, upath, &fp->f_dnode);
	fp->f_seekp = 0;
	if (rc) {
		f->f_fsdata = NULL;
		free(fp);
	}
	return (rc);
}

static int
zfs_close(struct open_file *f)
{
	struct file *fp = (struct file *)f->f_fsdata;

	dnode_cache_obj = 0;
	f->f_fsdata = (void *)0;
	if (fp == (struct file *)0)
		return (0);

	free(fp);
	return (0);
}

/*
 * Copy a portion of a file into kernel memory.
 * Cross block boundaries when necessary.
 */
static int
zfs_read(struct open_file *f, void *start, size_t size, size_t *resid	/* out */)
{
	const spa_t *spa = ((struct zfsmount *)f->f_devdata)->spa;
	struct file *fp = (struct file *)f->f_fsdata;
	struct stat sb;
	size_t n;
	int rc;

	rc = zfs_stat(f, &sb);
	if (rc)
		return (rc);
	n = size;
	if (fp->f_seekp + n > sb.st_size)
		n = sb.st_size - fp->f_seekp;
	
	rc = dnode_read(spa, &fp->f_dnode, fp->f_seekp, start, n);
	if (rc)
		return (rc);

	if (0) {
	    int i;
	    for (i = 0; i < n; i++)
		putchar(((char*) start)[i]);
	}
	fp->f_seekp += n;
	if (resid)
		*resid = size - n;

	return (0);
}

/*
 * Don't be silly - the bootstrap has no business writing anything.
 */
static int
zfs_write(struct open_file *f, void *start, size_t size, size_t *resid	/* out */)
{

	return (EROFS);
}

static off_t
zfs_seek(struct open_file *f, off_t offset, int where)
{
	struct file *fp = (struct file *)f->f_fsdata;

	switch (where) {
	case SEEK_SET:
		fp->f_seekp = offset;
		break;
	case SEEK_CUR:
		fp->f_seekp += offset;
		break;
	case SEEK_END:
	    {
		struct stat sb;
		int error;

		error = zfs_stat(f, &sb);
		if (error != 0) {
			errno = error;
			return (-1);
		}
		fp->f_seekp = sb.st_size - offset;
		break;
	    }
	default:
		errno = EINVAL;
		return (-1);
	}
	return (fp->f_seekp);
}

static int
zfs_stat(struct open_file *f, struct stat *sb)
{
	const spa_t *spa = ((struct zfsmount *)f->f_devdata)->spa;
	struct file *fp = (struct file *)f->f_fsdata;

	return (zfs_dnode_stat(spa, &fp->f_dnode, sb));
}

static int
zfs_readdir(struct open_file *f, struct dirent *d)
{
	const spa_t *spa = ((struct zfsmount *)f->f_devdata)->spa;
	struct file *fp = (struct file *)f->f_fsdata;
	mzap_ent_phys_t mze;
	struct stat sb;
	size_t bsize = fp->f_dnode.dn_datablkszsec << SPA_MINBLOCKSHIFT;
	int rc;

	rc = zfs_stat(f, &sb);
	if (rc)
		return (rc);
	if (!S_ISDIR(sb.st_mode))
		return (ENOTDIR);

	/*
	 * If this is the first read, get the zap type.
	 */
	if (fp->f_seekp == 0) {
		rc = dnode_read(spa, &fp->f_dnode,
				0, &fp->f_zap_type, sizeof(fp->f_zap_type));
		if (rc)
			return (rc);

		if (fp->f_zap_type == ZBT_MICRO) {
			fp->f_seekp = offsetof(mzap_phys_t, mz_chunk);
		} else {
			rc = dnode_read(spa, &fp->f_dnode,
					offsetof(zap_phys_t, zap_num_leafs),
					&fp->f_num_leafs,
					sizeof(fp->f_num_leafs));
			if (rc)
				return (rc);

			fp->f_seekp = bsize;
			fp->f_zap_leaf = (zap_leaf_phys_t *)malloc(bsize);
			rc = dnode_read(spa, &fp->f_dnode,
					fp->f_seekp,
					fp->f_zap_leaf,
					bsize);
			if (rc)
				return (rc);
		}
	}

	if (fp->f_zap_type == ZBT_MICRO) {
	mzap_next:
		if (fp->f_seekp >= bsize)
			return (ENOENT);

		rc = dnode_read(spa, &fp->f_dnode,
				fp->f_seekp, &mze, sizeof(mze));
		if (rc)
			return (rc);
		fp->f_seekp += sizeof(mze);

		if (!mze.mze_name[0])
			goto mzap_next;

		d->d_fileno = ZFS_DIRENT_OBJ(mze.mze_value);
		d->d_type = ZFS_DIRENT_TYPE(mze.mze_value);
		strcpy(d->d_name, mze.mze_name);
		d->d_namlen = strlen(d->d_name);
		return (0);
	} else {
		zap_leaf_t zl;
		zap_leaf_chunk_t *zc, *nc;
		int chunk;
		size_t namelen;
		char *p;
		uint64_t value;

		/*
		 * Initialise this so we can use the ZAP size
		 * calculating macros.
		 */
		zl.l_bs = ilog2(bsize);
		zl.l_phys = fp->f_zap_leaf;

		/*
		 * Figure out which chunk we are currently looking at
		 * and consider seeking to the next leaf. We use the
		 * low bits of f_seekp as a simple chunk index.
		 */
	fzap_next:
		chunk = fp->f_seekp & (bsize - 1);
		if (chunk == ZAP_LEAF_NUMCHUNKS(&zl)) {
			fp->f_seekp = (fp->f_seekp & ~(bsize - 1)) + bsize;
			chunk = 0;

			/*
			 * Check for EOF and read the new leaf.
			 */
			if (fp->f_seekp >= bsize * fp->f_num_leafs)
				return (ENOENT);

			rc = dnode_read(spa, &fp->f_dnode,
					fp->f_seekp,
					fp->f_zap_leaf,
					bsize);
			if (rc)
				return (rc);
		}

		zc = &ZAP_LEAF_CHUNK(&zl, chunk);
		fp->f_seekp++;
		if (zc->l_entry.le_type != ZAP_CHUNK_ENTRY)
			goto fzap_next;

		namelen = zc->l_entry.le_name_numints;
		if (namelen > sizeof(d->d_name))
			namelen = sizeof(d->d_name);

		/*
		 * Paste the name back together.
		 */
		nc = &ZAP_LEAF_CHUNK(&zl, zc->l_entry.le_name_chunk);
		p = d->d_name;
		while (namelen > 0) {
			int len;
			len = namelen;
			if (len > ZAP_LEAF_ARRAY_BYTES)
				len = ZAP_LEAF_ARRAY_BYTES;
			memcpy(p, nc->l_array.la_array, len);
			p += len;
			namelen -= len;
			nc = &ZAP_LEAF_CHUNK(&zl, nc->l_array.la_next);
		}
		d->d_name[sizeof(d->d_name) - 1] = 0;

		/*
		 * Assume the first eight bytes of the value are
		 * a uint64_t.
		 */
		value = fzap_leaf_value(&zl, zc);

		d->d_fileno = ZFS_DIRENT_OBJ(value);
		d->d_type = ZFS_DIRENT_TYPE(value);
		d->d_namlen = strlen(d->d_name);

		return (0);
	}
}

static int
vdev_read(vdev_t *vdev, void *priv, off_t offset, void *buf, size_t size)
{
	int fd;

	fd = (uintptr_t) priv;
	lseek(fd, offset, SEEK_SET);
	if (read(fd, buf, size) == size) {
		return 0;
	} else {
		return (EIO);
	}
}

static int
zfs_dev_init(void)
{
	spa_t *spa;
	spa_t *next;
	spa_t *prev;

	zfs_init();
  i386_zfs_probe();

	prev = NULL;
	spa = STAILQ_FIRST(&zfs_pools);
	while (spa != NULL) {
		next = STAILQ_NEXT(spa, spa_link);
		if (zfs_spa_init(spa)) {
			if (prev == NULL)
				STAILQ_REMOVE_HEAD(&zfs_pools, spa_link);
			else
				STAILQ_REMOVE_AFTER(&zfs_pools, prev, spa_link);
		} else
			prev = spa;
		spa = next;
	}
	return (0);
}

struct zfs_probe_args {
	int		fd;
	const char	*devname;
	uint64_t	*pool_guid;
	uint16_t	secsz;
};

static int
zfs_diskread(void *arg, void *buf, size_t blocks, off_t offset)
{
	struct zfs_probe_args *ppa;

	ppa = (struct zfs_probe_args *)arg;
	return (vdev_read(NULL, (void *)(uintptr_t)ppa->fd,
	    offset * ppa->secsz, buf, blocks * ppa->secsz));
}

static int
zfs_probe(int fd, uint64_t *pool_guid)
{
	spa_t *spa;
	int ret;

	ret = vdev_probe(vdev_read, (void *)(uintptr_t)fd, &spa);
	if (ret == 0 && pool_guid != NULL)
		*pool_guid = spa->spa_guid;
	return (ret);
}

static void
zfs_probe_partition(void *arg, const char *partname,
    const struct ptable_entry *part)
{
	struct zfs_probe_args *ppa, pa;
	struct ptable *table;
	char devname[32];
	int ret;

	/* Probe only freebsd-zfs and freebsd partitions */
	if (part->type != PART_FREEBSD &&
	    part->type != PART_FREEBSD_ZFS)
		return;

	ppa = (struct zfs_probe_args *)arg;
	strncpy(devname, ppa->devname, strlen(ppa->devname) - 1);
	devname[strlen(ppa->devname) - 1] = '\0';
	sprintf(devname, "%s%s:", devname, partname);
	pa.fd = open(devname, O_RDONLY);
	if (pa.fd == -1)
		return;
	ret = zfs_probe(pa.fd, ppa->pool_guid);
	if (ret == 0)
		return;
	/* Do we have BSD label here? */
	if (part->type == PART_FREEBSD) {
		pa.devname = devname;
		pa.pool_guid = ppa->pool_guid;
		pa.secsz = ppa->secsz;
		table = ptable_open(&pa, part->end - part->start + 1,
		    ppa->secsz, zfs_diskread);
		if (table != NULL) {
			ptable_iterate(table, &pa, zfs_probe_partition);
			ptable_close(table);
		}
	}
	close(pa.fd);
}

int
zfs_probe_dev(const char *devname, uint64_t *pool_guid)
{
	struct ptable *table;
	struct zfs_probe_args pa;
	off_t mediasz;
	int ret;

	pa.fd = open(devname, O_RDONLY);
	if (pa.fd == -1)
		return (ENXIO);
	/* Probe the whole disk */
	ret = zfs_probe(pa.fd, pool_guid);
	if (ret == 0)
		return (0);
	/* Probe each partition */
	ret = ioctl(pa.fd, DIOCGMEDIASIZE, &mediasz);
	if (ret == 0)
		ret = ioctl(pa.fd, DIOCGSECTORSIZE, &pa.secsz);
	if (ret == 0) {
		pa.devname = devname;
		pa.pool_guid = pool_guid;
		table = ptable_open(&pa, mediasz / pa.secsz, pa.secsz,
		    zfs_diskread);
		if (table != NULL) {
			ptable_iterate(table, &pa, zfs_probe_partition);
			ptable_close(table);
		}
	}
	close(pa.fd);
	return (0);
}

/*
 * Print information about ZFS pools
 */
static void
zfs_dev_print(int verbose)
{
	spa_t *spa;
	char line[80];

	if (verbose) {
		spa_all_status();
		return;
	}
	STAILQ_FOREACH(spa, &zfs_pools, spa_link) {
		sprintf(line, "    zfs:%s\n", spa->spa_name);
#ifdef BOOT2
		pager_printf(line);
#else
		pager_output(line);
#endif
	}
}

/*
 * Attempt to open the pool described by (dev) for use by (f).
 */
static int
zfs_dev_open(struct open_file *f, ...)
{
	va_list		args;
	struct zfs_devdesc	*dev;
	struct zfsmount	*mount;
	spa_t		*spa;
	int		rv;

	va_start(args, f);
	dev = va_arg(args, struct zfs_devdesc *);
	va_end(args);

	if (dev->pool_guid == 0)
		spa = STAILQ_FIRST(&zfs_pools);
	else
		spa = spa_find_by_guid(dev->pool_guid);
	if (!spa)
		return (ENXIO);
	mount = malloc(sizeof(*mount));
	rv = zfs_mount(spa, dev->root_guid, mount);
	if (rv != 0) {
		free(mount);
		return (rv);
	}
	if (mount->objset.os_type != DMU_OST_ZFS) {
		printf("Unexpected object set type %ju\n",
		    (uintmax_t)mount->objset.os_type);
		free(mount);
		return (EIO);
	}
	f->f_devdata = mount;
	free(dev);
	return (0);
}

static int
zfs_dev_close(struct open_file *f)
{

	free(f->f_devdata);
	f->f_devdata = NULL;
	return (0);
}

static int
zfs_dev_strategy(void *devdata, int rw, daddr_t dblk, size_t size, char *buf, size_t *rsize)
{

	return (ENOSYS);
}

struct devsw zfs_dev = {
	.dv_name = "zfs",
	.dv_type = DEVT_ZFS,
	.dv_init = zfs_dev_init,
	.dv_strategy = zfs_dev_strategy,
	.dv_open = zfs_dev_open,
	.dv_close = zfs_dev_close,
	.dv_ioctl = noioctl,
	.dv_print = zfs_dev_print,
	.dv_cleanup = NULL
};

int
zfs_parsedev(struct zfs_devdesc *dev, const char *devspec, const char **path)
{
	static char	rootname[ZFS_MAXNAMELEN];
	static char	poolname[ZFS_MAXNAMELEN];
	spa_t		*spa;
	const char	*end;
	const char	*np;
	const char	*sep;
	int		rv;

	np = devspec;
	if (*np != ':')
		return (EINVAL);
	np++;
	end = strchr(np, ':');
	if (end == NULL)
		return (EINVAL);
	sep = strchr(np, '/');
	if (sep == NULL || sep >= end)
		sep = end;
	memcpy(poolname, np, sep - np);
	poolname[sep - np] = '\0';
	if (sep < end) {
		sep++;
		memcpy(rootname, sep, end - sep);
		rootname[end - sep] = '\0';
	}
	else
		rootname[0] = '\0';

	spa = spa_find_by_name(poolname);
	if (!spa)
		return (ENXIO);
	dev->pool_guid = spa->spa_guid;
	rv = zfs_lookup_dataset(spa, rootname, &dev->root_guid);
	if (rv != 0) {
		printf("zfs_lookup_dataset failed\n");
		return (rv);
	}
	if (path != NULL)
		*path = (*end == '\0') ? end : end + 1;
	dev->d_dev = &zfs_dev;
	dev->d_type = zfs_dev.dv_type;
	return (0);
}

char *
zfs_fmtdev(void *vdev)
{
	static char		rootname[ZFS_MAXNAMELEN];
	static char		buf[2 * ZFS_MAXNAMELEN + 8];
	struct zfs_devdesc	*dev = (struct zfs_devdesc *)vdev;
	spa_t			*spa;

	buf[0] = '\0';
	if (dev->d_type != DEVT_ZFS)
		return (buf);

	if (dev->pool_guid == 0) {
		spa = STAILQ_FIRST(&zfs_pools);
		dev->pool_guid = spa->spa_guid;
	} else
		spa = spa_find_by_guid(dev->pool_guid);
	if (spa == NULL) {
		printf("ZFS: can't find pool by guid\n");
		return (buf);
	}
	if (dev->root_guid == 0 && zfs_get_root(spa, &dev->root_guid)) {
		printf("ZFS: can't find root filesystem\n");
		return (buf);
	}
	if (zfs_rlookup(spa, dev->root_guid, rootname)) {
		printf("ZFS: can't find filesystem by guid\n");
		return (buf);
	}

	if (rootname[0] == '\0')
		sprintf(buf, "%s:%s:", dev->d_dev->dv_name, spa->spa_name);
	else
		sprintf(buf, "%s:%s/%s:", dev->d_dev->dv_name, spa->spa_name,
		    rootname);
	return (buf);
}

int
zfs_list(const char *name)
{
	static char	poolname[ZFS_MAXNAMELEN];
	uint64_t	objid;
	spa_t		*spa;
	const char	*dsname;
	int		len;
	int		rv;

	len = strlen(name);
	dsname = strchr(name, '/');
	if (dsname != NULL) {
		len = dsname - name;
		dsname++;
	} else
		dsname = "";
	memcpy(poolname, name, len);
	poolname[len] = '\0';

	spa = spa_find_by_name(poolname);
	if (!spa) {
		printf("pool %s not found\n", poolname);
		return (ENXIO);
	}
	rv = zfs_lookup_dataset(spa, dsname, &objid);
	if (rv != 0) {
		printf("dataset %s not found\n", dsname);
		return (rv);
	}
	rv = zfs_list_dataset(spa, objid);
	return (rv);
}

#define MAXDEV		31		/* maximum number of distinct devices */
#define MAXBDDEV	MAXDEV

static void
i386_zfs_probe(void)
{
    char devname[32];
    int unit;
		
    /*
     * Open all the disks we can find and see if we can reconstruct
     * ZFS pools from them.
     */
    for (unit = 0; unit < MAXBDDEV; unit++) {
#if 0
	if (bd_unit2bios(unit) == -1)
	    break;
	sprintf(devname, "disk%d:", unit);
#endif
			sprintf(devname, "/dev/ada%d", unit);
			if (zfs_probe_dev(devname, NULL) != 0)
				break;
    }
    for (unit = 0; unit < MAXBDDEV; unit++) {
			sprintf(devname, "/dev/da%d", unit);
			if (zfs_probe_dev(devname, NULL) != 0)
				break;
    }
}

/* ARGSUSED */
int
noioctl(f, cmd, data)
	struct open_file *f;
	u_long cmd;
	void *data;
{
	return (EINVAL);
}

/*
 * i386 fully-qualified device descriptor.
 * Note, this must match the 'struct devdesc' declaration
 * in bootstrap.h and also with struct zfs_devdesc for zfs
 * support.
 */
struct i386_devdesc
{
    struct devsw	*d_dev;
    int			d_type;
    int			d_unit;
    union 
    {
	struct 
	{
	    void	*data;
	    int		slice;
	    int		partition;
	    off_t	offset;
	} biosdisk;
	struct
	{
	    void	*data;
	} bioscd;
	struct
	{
	    void	*data;
	    uint64_t	pool_guid;
	    uint64_t	root_guid;
	} zfs;
    } d_kind;
};

static int	i386_parsedev(struct i386_devdesc **dev, const char *devspec, const char **path);

/* 
 * Point (dev) at an allocated device specifier for the device matching the
 * path in (devspec). If it contains an explicit device specification,
 * use that.  If not, use the default device.
 */
int
i386_getdev(/*void*/struct devdesc **vdev, const char *devspec, const char **path)
{
    //struct i386_devdesc **dev = (struct i386_devdesc **)vdev;
    struct i386_devdesc **dev = (struct i386_devdesc **)vdev;
    int				rv;
    
    /*
     * If it looks like this is just a path and no
     * device, go with the current device.
     */
    if ((devspec == NULL) || 
	(devspec[0] == '/') || 
	(strchr(devspec, ':') == NULL)) {

	if (((rv = i386_parsedev(dev, getenv("currdev"), NULL)) == 0) &&
	    (path != NULL))
		*path = devspec;
	return(rv);
    }
    
    /*
     * Try to parse the device name off the beginning of the devspec
     */
    return(i386_parsedev(dev, devspec, path));
}

/*
 * Point (dev) at an allocated device specifier matching the string version
 * at the beginning of (devspec).  Return a pointer to the remaining
 * text in (path).
 *
 * In all cases, the beginning of (devspec) is compared to the names
 * of known devices in the device switch, and then any following text
 * is parsed according to the rules applied to the device type.
 *
 * For disk-type devices, the syntax is:
 *
 * disk<unit>[s<slice>][<partition>]:
 * 
 */
static int
i386_parsedev(struct i386_devdesc **dev, const char *devspec, const char **path)
{
    struct i386_devdesc *idev;
    struct devsw	*dv;
    int		err;
    const char		*np;

    /* minimum length check */
    if (strlen(devspec) < 2)
	return(EINVAL);

#if 0
    /* look for a device that matches */
    for (i = 0, dv = NULL; devsw[i] != NULL; i++) {
	if (!strncmp(devspec, devsw[i]->dv_name, strlen(devsw[i]->dv_name))) {
	    dv = devsw[i];
	    break;
	}
    }
#endif
		dv = &zfs_dev;
    if (dv == NULL)
	return(ENOENT);
    idev = malloc(sizeof(struct i386_devdesc));
    err = 0;
    np = (devspec + strlen(dv->dv_name));
        
    switch(dv->dv_type) {
    case DEVT_ZFS:
	err = zfs_parsedev((struct zfs_devdesc *)idev, np, path);
	if (err != 0)
	    goto fail;
	break;
    default:
	err = EINVAL;
	goto fail;
    }
    idev->d_dev = dv;
    idev->d_type = dv->dv_type;
    if (dev == NULL) {
	free(idev);
    } else {
	*dev = idev;
    }
    return(0);

 fail:
    free(idev);
    return(err);
}

ssize_t
myread(struct open_file *f, void *dest, size_t bcount)
{
    size_t		resid;

    /*
     * Optimise reads from regular files using a readahead buffer.
     * If the request can't be satisfied from the current buffer contents,
     * check to see if it should be bypassed, or refill the buffer and complete
     * the request.
     */
    resid = bcount;
    for (;;) {
			size_t	ccount, cresid;
			/* how much can we supply? */
			ccount = imin(f->f_ralen, resid);
			if (ccount > 0) {
					bcopy(f->f_rabuf + f->f_raoffset, dest, ccount);
					f->f_raoffset += ccount;
					f->f_ralen -= ccount;
					resid -= ccount;
					if (resid == 0)
				return(bcount);
					dest = (char *)dest + ccount;
			}

			/* will filling the readahead buffer again not help? */
			if (resid >= SOPEN_RASIZE) {
					/* bypass the rest of the request and leave the buffer empty */
					if ((errno = (f->f_ops->fo_read)(f, dest, resid, &cresid)))
						return (-1);
					return(bcount - cresid);
			}

			/* fetch more data */
			if ((errno = (f->f_ops->fo_read)(f, f->f_rabuf, SOPEN_RASIZE, &cresid)))
					return (-1);
			f->f_raoffset = 0;
			f->f_ralen = SOPEN_RASIZE - cresid;
			/* no more data, return what we had */
			if (f->f_ralen == 0)
					return(bcount - resid);
    }	
}

/*
 * Returns 1 if a directory has been created,
 * 2 if it already existed, and 0 on failure.
 */
int
build(char *path, mode_t omode)
{
	struct stat sb;
	mode_t numask, oumask;
	int first, last, retval;
	char *p;
	int vflag = 0;

	p = path;
	oumask = 0;
	retval = 1;
	if (p[0] == '/')		/* Skip leading '/'. */
		++p;
	for (first = 1, last = 0; !last ; ++p) {
		if (p[0] == '\0')
			last = 1;
		else if (p[0] != '/')
			continue;
		*p = '\0';
		if (!last && p[1] == '\0')
			last = 1;
		if (first) {
			/*
			 * POSIX 1003.2:
			 * For each dir operand that does not name an existing
			 * directory, effects equivalent to those caused by the
			 * following command shall occcur:
			 *
			 * mkdir -p -m $(umask -S),u+wx $(dirname dir) &&
			 *    mkdir [-m mode] dir
			 *
			 * We change the user's umask and then restore it,
			 * instead of doing chmod's.
			 */
			oumask = umask(0);
			numask = oumask & ~(S_IWUSR | S_IXUSR);
			(void)umask(numask);
			first = 0;
		}
		if (last)
			(void)umask(oumask);
		if (mkdir(path, last ? omode : S_IRWXU | S_IRWXG | S_IRWXO) < 0) {
			if (errno == EEXIST || errno == EISDIR) {
				if (stat(path, &sb) < 0) {
					printf("%s", path);
					retval = 0;
					break;
				} else if (!S_ISDIR(sb.st_mode)) {
					if (last)
						errno = EEXIST;
					else
						errno = ENOTDIR;
					printf("%s", path);
					retval = 0;
					break;
				}
				if (last)
					retval = 2;
			} else {
				printf("%s", path);
				retval = 0;
				break;
			}
		} else if (vflag)
			printf("%s\n", path);
		if (!last)
		    *p = '/';
	}
	if (!first && !last)
		(void)umask(oumask);
	return (retval);
}

#define	READ_BL_SIZE	32*1024
char *dirname(const char *);
char *fname = NULL;
const char *destfolder = NULL;
int	opt_copy = 0;
int opt_recursive = 0;

int
zfs_recover(const char *file, struct open_file *f)
{
	int ret = 0;

	//printf("zfs_recover file=%s\n", file);
	ret = zfs_open(file, f);
	if (ret != 0) {
		printf("zfs_open failed: %s\n", strerror(ret));
		return (1);
	}
	f->f_ops = &zfs_fsops;
	f->f_rabuf = malloc(READ_BL_SIZE);
	f->f_ralen = 0;
	f->f_raoffset = 0;

	if (!(f->f_flags & F_READ)) {
		printf("no F_READ flag\n");
		free(f->f_rabuf);
		return (1);
	}

	struct stat sb;
	if (0 != zfs_stat(f, &sb)) {
		printf("zfs_stat failed\n");
		free(f->f_rabuf);
		return (1);
	}

	if (S_ISDIR(sb.st_mode)) {
			struct dirent d;
			const char * d_name;
			SLIST_HEAD(slisthead, entry) head =
				SLIST_HEAD_INITIALIZER(head);
			struct entry {
				SLIST_ENTRY(entry) entries;
				char *file;
			} *n1;
			SLIST_INIT(&head);
			while ((ret = zfs_readdir(f, &d)) == 0) {
				d_name = d.d_name;
				n1 = malloc(sizeof(struct entry));
				n1->file = malloc(strlen(file) + strlen(d_name) + 2);
				sprintf(n1->file, "%s/%s", file, d_name);
				SLIST_INSERT_HEAD(&head, n1, entries);
			}
			struct file *fp = (struct file *)f->f_fsdata;
			free(fp->f_zap_leaf);
			//free(f->f_fsdata);
			free(f->f_rabuf);
			zfs_close(f);
			while (!SLIST_EMPTY(&head)) {
				n1 = SLIST_FIRST(&head);
				SLIST_REMOVE_HEAD(&head, entries);
				d_name = n1->file;
				while (d_name[0] == '/' && d_name[1] == '/')
					++d_name;
				printf("%s\n", d_name);
				if (opt_recursive)
						ret = zfs_recover(d_name, f);
				free(n1->file);
				free(n1);
				if (opt_recursive && ret != 0)
					return (ret);
			}
	} else if (S_ISREG(sb.st_mode)) {
		//printf("file size=%d\n", (int)sb.st_size);
		if (!opt_copy) {
			printf("%s\n", file);
			free(f->f_rabuf);
			zfs_close(f);
			return (0);
		}
		char *dest;
		const char *lc;
		const char *tmpd;
		char dataset[256];
		lc = strchr(fname, ':');
		if (lc == NULL)
			return (1);
		tmpd = strchr(lc, '/');
		lc = strchr(++lc, ':');
		if (lc == NULL)
			return (1);
		if (tmpd == NULL || ((lc - tmpd) < 0)) {
			strcpy(dataset, "");
		} else {
			++tmpd;
			memset(dataset, 0, 256);
			strncpy(dataset, tmpd, lc - tmpd);
		}
		dest = malloc(strlen(destfolder) + 1 + strlen(dataset) + strlen(file) + 1);
		sprintf(dest, "%s/%s%s", destfolder, dataset, file);
		char * folder;
		folder = dirname(dest);
		ret = build(folder, 0755);
		if (ret == 0) {
			printf("failed to mkdir %s\n", folder);
			free(dest);
			free(f->f_rabuf);
			zfs_close(f);
			return (1);
		}
		struct stat sbdest;
		ret = stat(dest, &sbdest);
		if (ret == 0
		&& S_ISREG(sbdest.st_mode)
		&& sb.st_size == sbdest.st_size) {
			printf("mtime = %d\n", sb.st_mtim.tv_sec);
			printf("dest mtime = %d\n", sbdest.st_mtim.tv_sec);
			if (sb.st_mtim.tv_sec != 0 &&
			sb.st_mtim.tv_sec != sbdest.st_mtim.tv_sec) {
				struct timeval times[2];
				TIMESPEC_TO_TIMEVAL(&times[0], &sb.st_atim);
				TIMESPEC_TO_TIMEVAL(&times[1], &sb.st_mtim);
				ret = utimes(dest, times);
				if (ret == 0)
					printf("%s mtime updated\n", dest);
			}
			free(dest);
			free(f->f_rabuf);
			zfs_close(f);
			return (0);
		}
		printf("writting in %s\n", dest);
#define O_CREAT   0x0200
		int fd = open(dest, O_WRONLY | O_CREAT);
		if (fd < 0) {
			printf("cannot open %s: %s\n", dest, strerror(errno));
			free(dest);
			free(f->f_rabuf);
			zfs_close(f);
			return (1);
		}
		fchmod(fd, 0644);
		ssize_t r;
		char buf[32*1024];
		while ((r = myread(f, buf, sizeof(buf))) > 0) {
			ret = write(fd, buf, r);
			if (ret != r)
				break;
		}
		close(fd);

		/* mtime */
		struct timeval times[2];
		TIMESPEC_TO_TIMEVAL(&times[0], &sb.st_atim);
		TIMESPEC_TO_TIMEVAL(&times[1], &sb.st_mtim);
		(void)utimes(dest, times);

		free(dest);
		//free(f->f_fsdata);
		free(f->f_rabuf);
		zfs_close(f);
	}
	return (0);
}

static void
usage(void)
{
	
	printf("%s\n%s\n%s\n\n",
		"usage: zfs_read -d",
		"       zfs_read -l <pool_name>",
		"       zfs_read [-c <dest_folder>] [-r] -f <pool/dataset:folder>");
	printf("OPTIONS\n");
	printf("\t%-18s%s\n", "-d", "look for pool in device /dev/da* and /dev/ada*");
	printf("\t%-18s%s\n", "-l pool_name", "list dataset for pool_name");
	printf("\t%-18s%s\n", "-f dataset:folder", "working folder, example: mypool/Pictures:/");
	printf("\t%-18s%s\n", "-c destfolder", "restore a copie of all files in destfolder");
	printf("\t%-18s%s\n", "-r", "recursive list or copie");

	exit(EX_USAGE);
}

int
main(int argc, char **argv)
{
	int ret, ch, opt_devprint = 0;
	const char * opt_zfs_list = NULL;

	while ((ch = getopt(argc, argv, "df:rc:l:")) != -1) {
		switch (ch) {
			case 'd':
				opt_devprint = 1;
				break;
			case 'f':
				fname = malloc(strlen("zfs:") + strlen(optarg) + 1);
				(void)strcpy(fname, "zfs:");
				(void)strcat(fname, optarg);
				break;
			case 'c':
				opt_copy = 1;
				destfolder = optarg;
				break;
			case 'r':
				opt_recursive = 1;
				break;
			case 'l':
				opt_zfs_list = optarg;
				break;
			default:
				usage();
		}
	}
	ret = zfs_dev_init();
	if (ret != 0)
		return (1);
	if (opt_devprint) {
		zfs_dev_print(0);
		return (0);
	}
	/* zfs list cmd */
	if (opt_zfs_list != NULL) {
			printf("dataset list %s:\n", opt_zfs_list);
			ret = zfs_list(opt_zfs_list);
			return (ret);
	}
	/* normal cmd */
	if (fname == NULL)
		usage();
	const char *file;
	int mode = O_RDONLY;
	struct open_file f;
	f.f_flags = mode + 1;
	f.f_dev = (struct devsw *)0;
	f.f_ops = (struct fs_ops *)0;
	f.f_offset = 0;
	f.f_devdata = NULL;
	file = NULL;
	struct devdesc *dev;
	ret = i386_getdev(/*(void **)*/&dev, fname, &file);
	if (ret) {
		printf("%s\n", strerror(ret));
		return (1);
	}
	f.f_devdata = dev;
	ret = dev->d_dev->dv_open(&f, dev); // => zfs_dev_open
	if (ret != 0) {
		f.f_devdata = NULL;
		free(dev);
		return (1);
	}
	f.f_dev = dev->d_dev;
	if (file == NULL || *file == '\0') {
		printf("raw device\n");
		return (1);
	}
	ret = zfs_recover(file, &f);

	return (0);
}

