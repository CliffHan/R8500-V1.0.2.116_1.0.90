/*
 * NVRAM variable manipulation (Linux user mode half)
 *
 * Copyright (C) 2012, Broadcom Corporation. All Rights Reserved.
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
 * $Id: nvram_linux.c 365067 2012-10-26 15:51:28Z $
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <typedefs.h>
#include <bcmnvram.h>

#include "ambitCfg.h" //Foxconn add, FredPeng, 04/17/2009
#define PATH_DEV_NVRAM "/dev/nvram"
#define CODE_BUFF	16
#define HEX_BASE	16

#define VALIDATE_BIT(bit) do { if ((bit < 0) || (bit > 31)) return NULL; } while (0)

/* wklin added, 12/13/2006 */
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#define LOCK            -1
#define UNLOCK          1
#define NVRAM_SAVE_KEY  0x12345678
static int lock_shm (int semkey, int op)
{
    struct sembuf lockop = { 0, 0, SEM_UNDO } /* sem operation */ ;
    int semid;
        
    if (semkey == 0)
        return -1;

    /* create/init sem */ 
    if ((semid = semget (semkey, 1, IPC_CREAT | IPC_EXCL | 0666)) >= 0) 
    {
        /* initialize the sem vaule to 1 */
        if (semctl (semid, 0, SETVAL, 1) < 0)
        {
            return -1;
        }
    }
    else
    {
        /* sem maybe has createdAget the semid */
        if ((semid = semget (semkey, 1, 0666)) < 0)
        {
            return -1;
        }
    }

    lockop.sem_op = op;
    if (semop (semid, &lockop, 1) < 0)
        return -1;
    
    return 0;
}

#define NVRAM_LOCK()    //lock_shm(NVRAM_SAVE_KEY, LOCK)
#define NVRAM_UNLOCK()    //lock_shm(NVRAM_SAVE_KEY, UNLOCK)
/* wklin added, 12/13/2006 */



/* Globals */
static int nvram_fd = -1;
static char *nvram_buf = NULL;

int
nvram_init(void *unused)
{

	if (nvram_fd >= 0)
		return 0;

	if ((nvram_fd = open(PATH_DEV_NVRAM, O_RDWR)) < 0)
		goto err;

	/* Map kernel string buffer into user space */
	nvram_buf = mmap(NULL, MAX_NVRAM_SPACE, PROT_READ, MAP_SHARED, nvram_fd, 0);
	if (nvram_buf == MAP_FAILED) {
		close(nvram_fd);
		nvram_fd = -1;
		goto err;
	}

	(void)fcntl(nvram_fd, F_SETFD, FD_CLOEXEC);

	return 0;

err:
	perror(PATH_DEV_NVRAM);
	return errno;
}

char *
nvram_get(const char *name)
{
	ssize_t count = strlen(name) + 1;
	char tmp[100], *value;
	unsigned long *off = (unsigned long *) tmp;

	if (nvram_init(NULL))
		return NULL;

	if (count > sizeof(tmp)) {
		if (!(off = malloc(count)))
			return NULL;
	}

	/* Get offset into mmap() space */
	strcpy((char *) off, name);

	count = read(nvram_fd, off, count);

	if (count == sizeof(unsigned long))
		value = &nvram_buf[*off];
	else
		value = NULL;

	if (count < 0)
		perror(PATH_DEV_NVRAM);

	if (off != (unsigned long *) tmp)
		free(off);

	return value;
}


char *
nvram_get_bitflag(const char *name, const int bit)
{
	VALIDATE_BIT(bit);
	char *ptr = nvram_get(name);
	unsigned long nvramvalue = 0;
	unsigned long bitflagvalue = 1;

	if (ptr) {
		bitflagvalue = bitflagvalue << bit;
		nvramvalue = strtoul(ptr, NULL, HEX_BASE);
		if (nvramvalue) {
			nvramvalue = nvramvalue & bitflagvalue;
		}
	}
	return ptr ? (nvramvalue ? "1" : "0") : NULL;
}

int
nvram_set_bitflag(const char *name, const int bit, const int value)
{
	VALIDATE_BIT(bit);
	char nvram_val[CODE_BUFF];
	char *ptr = nvram_get(name);
	unsigned long nvramvalue = 0;
	unsigned long bitflagvalue = 1;

	memset(nvram_val, 0, sizeof(nvram_val));

	if (ptr) {
		bitflagvalue = bitflagvalue << bit;
		nvramvalue = strtoul(ptr, NULL, HEX_BASE);
		if (value) {
			nvramvalue |= bitflagvalue;
		} else {
			nvramvalue &= (~bitflagvalue);
		}
	}
	snprintf(nvram_val, sizeof(nvram_val)-1, "%lx", nvramvalue);
	return nvram_set(name, nvram_val);
}

int
nvram_getall(char *buf, int count)
{
	int ret;

	if (nvram_fd < 0)
		if ((ret = nvram_init(NULL)))
			return ret;

	if (count == 0)
		return 0;

	/* Get all variables */
	*buf = '\0';

	ret = read(nvram_fd, buf, count);

	if (ret < 0)
		perror(PATH_DEV_NVRAM);

	return (ret == count) ? 0 : ret;
}

static int
_nvram_set(const char *name, const char *value)
{
	size_t count = strlen(name) + 1;
	char tmp[100], *buf = tmp;
	int ret;

	if ((ret = nvram_init(NULL)))
		return ret;

	/* Unset if value is NULL */
	if (value)
		count += strlen(value) + 1;

	if (count > sizeof(tmp)) {
		if (!(buf = malloc(count)))
			return -ENOMEM;
	}

	if (value)
		sprintf(buf, "%s=%s", name, value);
	else
		strcpy(buf, name);

	ret = write(nvram_fd, buf, count);

	if (ret < 0)
		perror(PATH_DEV_NVRAM);

	if (buf != tmp)
		free(buf);

	return (ret == count) ? 0 : ret;
}

int
nvram_set(const char *name, const char *value)
{
	return _nvram_set(name, value);
}

int
nvram_unset(const char *name)
{
	return _nvram_set(name, NULL);
}

int
nvram_commit(void)
{
	int ret;

    /* foxconn wklin added start, 11/02/2010, show messag when doing commit */ 
    {
        FILE *fp;
        fp = fopen("/dev/console", "w");
        if (fp) {
            fprintf(fp, "Doing nvram commit by pid %d !\n",getpid());
            fclose(fp);
        }
    }
    /* foxconn wklin added end , 11/02/2010 */
	if ((ret = nvram_init(NULL)))
		return ret;

	ret = ioctl(nvram_fd, NVRAM_MAGIC, NULL);

	if (ret < 0)
		perror(PATH_DEV_NVRAM);

	return ret;
}

/* Foxconn added start Peter Ling 12/05/2005 */
#ifdef ACOS_MODULES_ENABLE
extern struct nvram_tuple router_defaults[];

int nvram_loaddefault (void)
{
    /* Foxconn add start, FredPeng, 04/14/2009 */
    char cmd[128];
    memset(cmd, 0, sizeof(cmd));
    /* Foxconn add end, FredPeng, 04/14/2009 */

    system("rm /tmp/ppp/ip-down"); /* added by EricHuang, 01/12/2007 */
    
    /* Foxconn modify start, FredPeng, 04/14/2009 */
    sprintf(cmd, "erase %s", NVRAM_MTD_WR);
    system(cmd);
    /* Foxconn modify end, FredPeng, 04/14/2009 */
    
    printf("Load default done!\n");
    
    return (0);
}
#endif
/* Foxconn added end Peter Ling 12/05/2005 */
