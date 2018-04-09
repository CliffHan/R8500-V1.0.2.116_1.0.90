/*
 * MTD utility functions
 *
 * Copyright (C) 2014, Broadcom Corporation. All Rights Reserved.
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $Id: mtd.c 437682 2013-11-19 19:25:16Z $
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>

#ifdef LINUX26
#include <mtd/mtd-user.h>
#else /* LINUX26 */
#include <linux/mtd/mtd.h>
#endif /* LINUX26 */

#include <trxhdr.h>
#include <bcmutils.h>
#include <bcmendian.h>
#include <bcmnvram.h>
#include <shutils.h>

/*
 * Open an MTD device
 * @param	mtd	path to or partition name of MTD device
 * @param	flags	open() flags
 * @return	return value of open()
 */
int
mtd_open(const char *mtd, int flags)
{
	FILE *fp;
	char dev[PATH_MAX];
	int i;

	if ((fp = fopen("/proc/mtd", "r"))) {
		while (fgets(dev, sizeof(dev), fp)) {
			if (sscanf(dev, "mtd%d:", &i) && strstr(dev, mtd)) {
#ifdef LINUX26
				snprintf(dev, sizeof(dev), "/dev/mtd%d", i);
#else
				snprintf(dev, sizeof(dev), "/dev/mtd/%d", i);
#endif
				fclose(fp);
				return open(dev, flags);
			}
		}
		fclose(fp);
	}

	return open(mtd, flags);
}

/* Foxconn Bob added start for nand page write, 03/19/2014 */
int mtd_isbad(const char *mtd, const char *off)
{
    loff_t offset;
    int mtd_fd;
    int ret;
    
    /* Open MTD device */
	if ((mtd_fd = mtd_open(mtd, O_RDWR)) < 0) {
		perror(mtd);
		return errno;
	}
    
    offset = strtoull(off, NULL, 16);
    
    ret = ioctl(mtd_fd, MEMGETBADBLOCK, &offset);
    
    if(ret==0)
    {
        cprintf("This blosk is good block\n");
    }
    else if(ret==-1)
    {
        cprintf("This blosk is bad block\n");
    }
    else
    {
        perror(mtd);
		close(mtd_fd);
		return errno;
    }
    
    close(mtd_fd);
    return 0;
}

int mtd_markbad(const char *mtd, const char *off)
{
    loff_t offset;
    int mtd_fd;
    
    /* Open MTD device */
	if ((mtd_fd = mtd_open(mtd, O_RDWR)) < 0) {
		perror(mtd);
		return errno;
	}
    
    offset = strtoull(off, NULL, 16);
    
    if (ioctl(mtd_fd, MEMSETBADBLOCK, &offset) != 0) {
		perror(mtd);
		close(mtd_fd);
		return errno;
	}
    
    close(mtd_fd);
    return 0;
}

/*it doesn't work, not sure if it related to HW ECC, Bob comments on 03/19/2014 */
int mtd_write_oob(const char *mtd, const char *off) 
{
    loff_t offset;
    int mtd_fd;
    mtd_info_t mtd_info;
    struct mtd_oob_buf oob;
    char *buf;

    /* Open MTD device */
	if ((mtd_fd = mtd_open(mtd, O_RDWR)) < 0) {
		perror(mtd);
		return errno;
	}
	
	/* Get sector size */
	if (ioctl(mtd_fd, MEMGETINFO, &mtd_info) != 0) {
		perror(mtd);
		close(mtd_fd);
		return errno;
	}
	
	buf = malloc(mtd_info.oobsize);
	if(!buf)
	{
	    perror(mtd);
	    close(mtd_fd);
        return errno;
	}
	offset = strtoull(off, NULL, 16);
    
    memset(buf, 0xff, mtd_info.oobsize);    /* test purpose, alway write 0xff to this page oob. */
    oob.start = (__u32)offset;
	oob.length = mtd_info.oobsize;
    oob.ptr = buf;
    
    if (ioctl(mtd_fd, MEMWRITEOOB, &oob) != 0) {
        free(buf);
		perror(mtd);
		close(mtd_fd);
		return errno;
	}
    free(buf);
    close(mtd_fd);
    return 0;
}

int mtd_read_oob(const char *mtd, const char *off)
{
    loff_t offset;
    int i;
    int mtd_fd;
    mtd_info_t mtd_info;
    struct mtd_oob_buf oob;
    char *buf;

    /* Open MTD device */
	if ((mtd_fd = mtd_open(mtd, O_RDWR)) < 0) {
		perror(mtd);
		return errno;
	}
	/* Get sector size */
	if (ioctl(mtd_fd, MEMGETINFO, &mtd_info) != 0) {
		perror(mtd);
		close(mtd_fd);
		return errno;
	}
	
	buf = malloc(mtd_info.oobsize);
	if(!buf)
	{
	    perror(mtd);
	    close(mtd_fd);
        return errno;
	}
	offset = strtoull(off, NULL, 16);
    
    oob.start = (__u32)offset;
	oob.length = mtd_info.oobsize;
    oob.ptr = buf;
    
    if (ioctl(mtd_fd, MEMREADOOB, &oob) != 0) {
        free(buf);
		perror(mtd);
		close(mtd_fd);
		return errno;
	}

    for(i=0;i<mtd_info.oobsize;i++)
    {
        cprintf("%02x ", *(buf+i));
    }

    free(buf);
    close(mtd_fd);
    
    return 0;
}

int mtd_write_page(const char *mtd, const char *ofs)
{
    loff_t offset;
    int mtd_fd;
    struct mtd_oob_buf oob;
    mtd_info_t mtd_info;
    char *buf;

    /* Open MTD device */
	if ((mtd_fd = mtd_open(mtd, O_RDWR)) < 0) {
		perror(mtd);
		return errno;
	}
	
	/* Get sector size */
	if (ioctl(mtd_fd, MEMGETINFO, &mtd_info) != 0) {
		perror(mtd);
		close(mtd_fd);
		return errno;
	}
	
	buf = malloc(mtd_info.writesize);
	if(!buf)
	{
	    perror(mtd);
	    close(mtd_fd);
        return errno;
	}
	offset = strtoull(ofs, NULL, 16);

    memset(buf, 0x42, mtd_info.writesize);  /* test purpose, alway write 'B' to this page. */
    oob.start = (__u32)offset;
	oob.length = mtd_info.writesize;
    oob.ptr = buf;
    
    if (ioctl(mtd_fd, MEMWRITEPAGE, &oob) != 0) {
		free(buf);
		perror(mtd);
		close(mtd_fd);
		return errno;
	}
	
    free(buf);
    close(mtd_fd);
    return 0;
}
/* Foxconn Bob added end for nand page write, 03/19/2014 */

/*
 * Erase an MTD device
 * @param	mtd	path to or partition name of MTD device
 * @return	0 on success and errno on failure
 */
int
mtd_erase(const char *mtd)
{
	int mtd_fd;
	mtd_info_t mtd_info;
	erase_info_t erase_info;
	int cnt;
	int isNvram = 0;

	/* Open MTD device */
	if ((mtd_fd = mtd_open(mtd, O_RDWR)) < 0) {
		perror(mtd);
		return errno;
	}

	/* Get sector size */
	if (ioctl(mtd_fd, MEMGETINFO, &mtd_info) != 0) {
		perror(mtd);
		close(mtd_fd);
		return errno;
	}

    /* Foxconn Bob modified start, 09/06/2013, a workaround to avoid erase to next partition,
       unknown reason, erase bad block won't return error in the case of 15th block(last block of nvram partition) is bad block */
	if(!strcmp(mtd, "/dev/mtd1"))
	    isNvram = 1;
	erase_info.length = mtd_info.erasesize;
	cnt = 0;

	for (erase_info.start = 0;
	     erase_info.start < mtd_info.size;
	     erase_info.start += mtd_info.erasesize) {
		(void) ioctl(mtd_fd, MEMUNLOCK, &erase_info);
		if (ioctl(mtd_fd, MEMERASE, &erase_info) != 0) {
		    cprintf("%s: erase failed, could be bad block, continue next block!\n", mtd);
		        mtd_info.size -= mtd_info.erasesize;
		    continue;
			//perror(mtd);
			//close(mtd_fd);
			//return errno;
		}
		    cnt++;
		    if(isNvram==1 && cnt>=4)
		        break;
	}

	close(mtd_fd);
	return 0;
}

extern int http_get(const char *server, char *buf, size_t count, off_t offset);

/*
 * Write a file to an MTD device
 * @param	path	file to write or a URL
 * @param	mtd	path to or partition name of MTD device
 * @return	0 on success and errno on failure
 */
int
mtd_write(const char *path, const char *mtd)
{
	int mtd_fd = -1;
	mtd_info_t mtd_info;
	erase_info_t erase_info;

	struct sysinfo info;
	struct trx_header trx;
	unsigned long crc;

	FILE *fp;
	char *buf = NULL;
	long count, len, off;
	int ret = -1;

	/* Examine TRX header */
	if ((fp = fopen(path, "r")))
		count = safe_fread(&trx, 1, sizeof(struct trx_header), fp);
	else
		count = http_get(path, (char *) &trx, sizeof(struct trx_header), 0);
	if (count < sizeof(struct trx_header)) {
		fprintf(stderr, "%s: File is too small (%ld bytes)\n", path, count);
		goto fail;
	}

	/* Open MTD device and get sector size */
	if ((mtd_fd = mtd_open(mtd, O_RDWR)) < 0 ||
	    ioctl(mtd_fd, MEMGETINFO, &mtd_info) != 0 ||
	    mtd_info.erasesize < sizeof(struct trx_header)) {
		perror(mtd);
		goto fail;
	}

	if (trx.magic != TRX_MAGIC ||
	    trx.len > mtd_info.size ||
	    trx.len < sizeof(struct trx_header)) {
		fprintf(stderr, "%s: Bad trx header\n", path);
		goto fail;
	}


	/* Allocate temporary buffer */
	/* See if we have enough memory to store the whole file */
	sysinfo(&info);
	if (info.freeram >= trx.len) {
		erase_info.length = ROUNDUP(trx.len, mtd_info.erasesize);
		if (!(buf = malloc(erase_info.length)))
			erase_info.length = mtd_info.erasesize;
	}
	/* fallback to smaller buffer */
	else {
		erase_info.length = mtd_info.erasesize;
		buf = NULL;
	}
	if (!buf && (!(buf = malloc(erase_info.length)))) {
		perror("malloc");
		goto fail;
	}

	/* Calculate CRC over header */
	crc = hndcrc32((uint8 *) &trx.flag_version,
	               sizeof(struct trx_header) - OFFSETOF(struct trx_header, flag_version),
	               CRC32_INIT_VALUE);

	if (trx.flag_version & TRX_NO_HEADER)
		trx.len -= sizeof(struct trx_header);

	/* Write file or URL to MTD device */
	for (erase_info.start = 0; erase_info.start < trx.len; erase_info.start += count) {
		len = MIN(erase_info.length, trx.len - erase_info.start);
		if ((trx.flag_version & TRX_NO_HEADER) || erase_info.start)
			count = off = 0;
		else {
			count = off = sizeof(struct trx_header);
			memcpy(buf, &trx, sizeof(struct trx_header));
		}
		if (fp)
			count += safe_fread(&buf[off], 1, len - off, fp);
		else
			count += http_get(path, &buf[off], len - off, erase_info.start + off);
		if (count < len) {
			fprintf(stderr, "%s: Truncated file (actual %ld expect %ld)\n", path,
				count - off, len - off);
			goto fail;
		}
		/* Update CRC */
		crc = hndcrc32((uint8 *)&buf[off], count - off, crc);
		/* Check CRC before writing if possible */
		if (count == trx.len) {
			if (crc != trx.crc32) {
				fprintf(stderr, "%s: Bad CRC\n", path);
				goto fail;
			}
		}
		/* Do it */
		(void) ioctl(mtd_fd, MEMUNLOCK, &erase_info);
		if (ioctl(mtd_fd, MEMERASE, &erase_info) != 0 ||
		    write(mtd_fd, buf, count) != count) {
			perror(mtd);
			goto fail;
		}
	}

#ifdef PLC
  eval("gigle_util restart");
  nvram_set("plc_pconfig_state", "2");
  nvram_commit();
#endif

	printf("%s: CRC OK\n", mtd);
	ret = 0;

fail:
	if (buf) {
		/* Dummy read to ensure chip(s) are out of lock/suspend state */
		(void) read(mtd_fd, buf, 2);
		free(buf);
	}

	if (mtd_fd >= 0)
		close(mtd_fd);
	if (fp)
		fclose(fp);
	return ret;
}
