/*
 * USB hotplug service
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
 * $Id: usb_hotplug.c 379640 2013-01-18 10:18:40Z $
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h> /* foxconn wklin added, 03/16/2011 */
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <typedefs.h>
#include <shutils.h>
#include <bcmconfig.h>
#include <bcmparams.h>
#include <wlutils.h>
#include <bcmgpio.h>

#if defined(__CONFIG_DLNA_DMS__)
#include <bcmnvram.h>
#endif	

#define WL_DOWNLOADER_4323_VEND_ID "a5c/bd13/1"
#define WL_DOWNLOADER_43236_VEND_ID "a5c/bd17/1"
#define WL_DOWNLOADER_43526_VEND_ID "a5c/bd1d/1"
#define WL_DOWNLOADER_43556_VEND_ID "a5c/bd23/1"
#define WL_DOWNLOADER_4360_VEND_ID "a5c/bd1d/1"

static int usb_start_services(void);
static int usb_stop_services(void);

#ifdef LINUX26
char *mntdir = "/media";
#else
char *mntdir = "/mnt";
static int usb_mount_ufd(void);
#endif

#ifdef HOTPLUG_DBG
int hotplug_pid = -1;
FILE *fp = NULL;
#define hotplug_dbg(fmt, args...) (\
{ \
	char err_str[100] = {0}; \
	char err_str2[100] = {0}; \
	if (hotplug_pid == -1) hotplug_pid = getpid(); \
	if (!fp) fp = fopen("/tmp/usb_err", "a+"); \
	sprintf(err_str, fmt, ##args); \
	sprintf(err_str2, "PID:%d %s", hotplug_pid, err_str); \
	fwrite(err_str2, strlen(err_str2), 1,  fp); \
	fflush(fp); \
} \
)
#else
#define hotplug_dbg(fmt, args...)
#endif /* HOTPLUG_DBG */

/* Foxconn add start, Max Ding, 12/07/2010 */
int is_dev_mounted(char *devname)
{
    FILE *fp = NULL;
    char line[256] = "";
 
    fp = fopen("/proc/mounts", "r");
 
    if (fp != NULL)
    {
        while (fgets(line, sizeof(line), fp) != NULL)
        {
            if (strstr(line, devname) != NULL)
            {
                fclose(fp);
                return 1;
            }
        }
        fclose(fp);
    }
    return 0;
}

int wrapped_mount(char *source, char *target)
{
    int rval = 0;
    char buf[128];
    /* foxconn wklin added start, 03/16/2011 */
    struct stat fs;
    int wait_count = 0;
    /* foxconn wklin added end, 03/16/2011 */
    //------------------------
    //start mount with sync
    //------------------------       
    //rval = mount(source, target, "vfat",0, "iocharset=utf8"); //stanley add,09/14/2009
    /* foxconn wklin added start, 03/16/2011 */
    /* have to wait until usb-used tmp file created in /tmp/mnt,
     * this happends when using static-linked usb module, when system startup
     * we got the events but the target tmp files haven't been initialized yet.. 
     * so wait for a while before we can mount to the target.
     */
retry:
    if (stat(target, &fs) != 0) {
        sleep(1);
#ifdef USB_DEBUG
        cprintf("target %s not available wait another seconds..\n", target);
#endif
        if (++wait_count < 50) /* wait for 50 seconds at most */
            goto retry;
    } 
    /* foxconn wklin added end, 03/16/2011 */
    /* Use mlabel to read VFAT volume label after successful mount */
    if ((rval = mount(source, target, "vfat",0, "iocharset=utf8")) == 0)
    {
        /* Foxconn, add-start by MJ., for downsizing WNDR3400v2, 2011.02.21.  */
#if (defined WNDR3400v2) || (defined R6300v2) || (defined R7000) || defined(R8000)
        snprintf(buf, 128, "/lib/udev/vol_id %s", source);
/* Foxconn added start pling 12/26/2011, for WNDR4000AC */
#elif (defined WNDR4000AC) || defined(R6250) || defined(R6200v2)
        get_vol_id(source);
        memset(buf, 0, sizeof(buf));
/* Foxconn added end pling 12/26/2011, for WNDR4000AC */
#else
        /* Foxconn, add-end by MJ., for downsizing WNDR3400v2, 2011.02.21.  */
        snprintf(buf, 128, "/usr/bin/mlabel -i %s -s ::", source);
#endif
        system(buf);
    }
/*Foxconn added start by Kathy, 08/05/2014 @ support XFS requested by Netgear Bing*/
    else if ((rval = mount(source, target, "xfs", 0, "")) == 0)
    {
        //printf("Mount xfs for '%s' success!\n", target);
        get_vol_id(source);
    }
/*Foxconn added end by Kathy, 08/05/2014 @ support XFS requested by Netgear Bing*/
    /* Foxconn added start pling 05/19/2010 */
    /* Try to mount HFS+ and ext2/ext3 */
    //rval = mount(source, target, "hfsplus", 0, "");
    else if ((rval = mount(source, target, "hfsplus", 0, "")) == 0)
    {
        //printf("Mount hfs+ for '%s' success!\n", target);
        get_vol_id(source);
    }

    //rval = mount(source, target, "hfs", 0, "");
    else if ((rval = mount(source, target, "hfs", 0, "")) == 0)
    {
        //printf("Mount hfs for '%s' success!\n", target);
        get_vol_id(source);
    }

    //rval = mount(source, target, "ext3", 0, "");
    else if ((rval = mount(source, target, "ext3", 0, "")) == 0)
    {
        //printf("Mount ext3 for '%s' success!\n", target);
        get_vol_id(source);
    }

    //rval = mount(source, target, "ext2", 0, "");
    else if ((rval = mount(source, target, "ext2", 0, "")) == 0)
    {
        //printf("Mount ext2 for '%s' success!\n", target);
        get_vol_id(source);
    }
    /* Foxconn added end pling 05/19/2010 */
    /* Foxconn added start Antony  06/24/2013 enable the ext4 pen drive */
    else if ((rval = mount(source, target, "ext4", 0, "")) == 0)
    {
//        printf("Mount ext4 for '%s' success!\n", target);
        get_vol_id(source);
    }
    /* Foxconn added end Antony 06/24/2013 */

    if (rval<0 && errno == EINVAL)
    {
        int ret;
        /* Use NTFS-3g driver to provide NTFS r/w function */
/* Foxconn Silver added start, 2011/12/21, for kernel_ntfs */       
#ifdef USE_KERNEL_NTFS        
        snprintf(buf, 128, "mount -t ufsd -o force %s %s", source, target);
        cprintf("***[%s] %s\n", __FUNCTION__, buf);
#else        
        snprintf(buf, 128, "/bin/ntfs-3g -o large_read %s %s 2> /dev/null", source, target);
#endif        
/* Foxconn Silver added end, 2011/12/21, for kernel_ntfs */       
        ret = system(buf);

#ifdef USE_KERNEL_NTFS        
        if (ret == 0 )        // foxconn add by ken chen, 11/13/2013, fix crash issue when mount fs failed.
		    get_vol_id(source);
#endif        

        /* can't judge if mount is successful*/
        if (is_dev_mounted(source))
        {
            return 0;
        }
        else
        {
            return -1;
        }
    }
    else if (rval == 0)
    {
        return 0;
    }
    else
    {
        return -1;
    }
}
/* Foxconn add end, Max Ding, 12/07/2010 */

/* Foxconn modify start, Max Ding, 12/07/2010 */
/* Foxconn add start, Tony W.Y. Wang, 10/20/2010 */
#define LOCK           -1
#define UNLOCK          1 
#define MNT_DETACH 0x00000002 /* Foxconn add , Jenny Zhao, 04/22/2009 @usb dir */

#define USB_LOCK()      usb_sem_lock(LOCK)
#define USB_UNLOCK()    usb_sem_lock(UNLOCK)

/* Foxconn added start pling 05/26/2010 */
#define VOL_ID_TMP_FILE     "/tmp/vol_id.out"
#define VOL_NAME_FILE       "/tmp/usb_vol_name/%s"
#define VOL_LABEL           "ID_FS_LABEL="
int get_vol_id(char *dev)
{
    FILE *fp = NULL;
    char line[128];
    char command[128];
    char filename[128];
    char *p;
    char *label = NULL;

    sprintf(command, "rm %s", VOL_ID_TMP_FILE);
    system(command);
    sprintf(command, "/lib/udev/vol_id %s > %s", dev, VOL_ID_TMP_FILE);
    system(command);

    fp = fopen(VOL_ID_TMP_FILE, "r");
    if (fp)
    {
        while (!feof(fp))
        {
            fgets(line, sizeof(line), fp);
            p = strstr(line, VOL_LABEL);
            if (p != NULL)
            {
                label = p + strlen(VOL_LABEL);
                break;
            }
        }
        fclose(fp);
    }
    else
        printf("Unable to open '%s' (%d)\n", VOL_ID_TMP_FILE, errno);

    /* 'dev' is a string in the format '/dev/sdxn" */
    /* The usb_vol_name file should be /tmp/usb_vol_name/sdxn
     * So we skip the first 5 characters.
     */
    if (strncmp(dev, "/dev/", 5) == 0)
    {
        sprintf(filename, VOL_NAME_FILE, dev+5);
        fp = fopen(filename, "w+"); /* wklin modified, w -> w+ */
        if (fp)
        {
            if (label !=NULL)
                fprintf(fp, "%s\n", label);
            else
                fprintf(fp, "\n");
            fclose(fp);
        }
        else
            printf("Unable to open '%s' (%d)\n", filename, errno);
    }

    return 0;
}
/* Foxconn added end pling 05/26/2010 */

static int approved_serial(char *dev_name)
{
    /* check approved device by comparing serial number */
    char sn_path[] = "/sys/block/sdx/device/../../../../serial";
    char nvram[]="approved_usb_100";
    char nvram_val[256];
    char buf[256];
    FILE *fd;
    int i;

    fprintf(stderr, "%s:%d getting sn for %s\n", 
            __func__, __LINE__, dev_name);
    /* dev_name is something like this /dev/sdax */
    snprintf(sn_path, sizeof(sn_path), "/sys/block/sd%c/device/../../../../serial",
            *(dev_name+7));
    fprintf(stderr, "%s:%d sn_path=%s\n", __func__, __LINE__, sn_path);

    fd = fopen(sn_path,"r");
    if (fd) {
#define MAX_SN_LEN 128
        fgets(buf, MAX_SN_LEN, fd);
        fclose(fd);
        if (strlen(buf) > 1)
            buf[strlen(buf)-1]='\0'; /* strip '\n' */
    } else {
        fprintf(stderr, "%s:%d error opening file %s\n", __func__, __LINE__,
                sn_path);
    }
    fprintf(stderr, "%s:%d serial number for %s is %s\n", 
            __func__, __LINE__, dev_name, buf);
    
    /* TODO: 20 has to be defined as MAX_APPROVED_DEV */
    for (i=0; i<20; i++) {
        sprintf(nvram, "approved_usb_%d", i);
        strcpy(nvram_val, acosNvramConfig_get(nvram));
        if (strlen (nvram_val) != 0) {
            if (strncmp(nvram_val, buf, strlen(buf))==0)
                return 1;
        }
    }
    return 0;
}

/* Bob added start 04/01/2011, to log usb information */
typedef enum log_category_enum
{
    LOG_CATEGORY_NONE       = (0),
    
    LOG_CATEGORY_ALLOW      = (1 << 0),
    LOG_CATEGORY_BLOCK      = (1 << 1),
    LOG_CATEGORY_LOGIN      = (1 << 2),
    LOG_CATEGORY_INFO       = (1 << 3),
    LOG_CATEGORY_FW         = (1 << 4),
    LOG_CATEGORY_PFPT       = (1 << 5),
    LOG_CATEGORY_WL         = (1 << 6),
    LOG_CATEGORY_AUTOCONN   = (1 << 7),
    LOG_CATEGORY_WLSCHE     = (1 << 8),

    LOG_CATEGORY_ALL        = (0xFFFF)
} log_category;

typedef enum log_event_enum
{
    LOG_EVEN_NONE               = (LOG_CATEGORY_NONE << 16) | 0,
    
    LOG_EVENT_INIT              = (LOG_CATEGORY_INFO << 16) | 1,
    LOG_EVENT_TIME_SYNC         = (LOG_CATEGORY_INFO << 16) | 2,
    LOG_EVENT_INET_CONN         = (LOG_CATEGORY_INFO << 16) | 3,
    LOG_EVENT_INET_DISCONN      = (LOG_CATEGORY_INFO << 16) | 4,
    LOG_EVENT_IDLE_TIMEOUT      = (LOG_CATEGORY_INFO << 16) | 5,
    LOG_EVENT_WLAN_REJ          = (LOG_CATEGORY_WL << 16) | 6,
    LOG_EVENT_ASSIGN_IP         = (LOG_CATEGORY_INFO << 16) | 7,
    LOG_EVENT_UPNP_SET          = (LOG_CATEGORY_INFO << 16) | 8,
    LOG_EVENT_DDNS              = (LOG_CATEGORY_INFO << 16) | 9,
    LOG_EVENT_ADMIN_LOGIN       = (LOG_CATEGORY_LOGIN << 16) | 10,
    LOG_EVENT_REMOTE_LOGIN      = (LOG_CATEGORY_LOGIN << 16) | 11,
    LOG_EVENT_ADMIN_LOGIN_FAIL  = (LOG_CATEGORY_LOGIN << 16) | 12,
    LOG_EVENT_REMOTE_LOGIN_FAIL = (LOG_CATEGORY_LOGIN << 16) | 13,
    LOG_EVENT_SITE_ALLOW        = (LOG_CATEGORY_ALLOW << 16) | 14,
    LOG_EVENT_SITE_BLOCK        = (LOG_CATEGORY_BLOCK << 16) | 15,
    LOG_EVENT_SERV_BLOCK        = (LOG_CATEGORY_BLOCK << 16) | 16,
    LOG_EVENT_EMAIL_SEND        = (LOG_CATEGORY_INFO << 16) | 17,
    LOG_EVENT_EMAIL_FAIL        = (LOG_CATEGORY_INFO << 16) | 18,
    LOG_EVENT_WLAN_ACL_DENY     = (LOG_CATEGORY_WL << 16) | 19,
    LOG_EVENT_WLAN_ACL_ALLOW    = (LOG_CATEGORY_WL << 16) | 20,
    LOG_EVENT_ATTACK            = (LOG_CATEGORY_FW << 16) | 21,
    LOG_EVENT_REMOTE_ACCESS_LAN = (LOG_CATEGORY_PFPT << 16) | 22,
    LOG_EVENT_TM_WATER_MARK     = (LOG_CATEGORY_INFO << 16) | 23,
    LOG_EVENT_TM_REACH_LIMIT    = (LOG_CATEGORY_INFO << 16) | 24,
    LOG_EVENT_PKT_ACCEPT        = (LOG_CATEGORY_ALLOW << 16) | 25,
    LOG_EVENT_PKT_DROP          = (LOG_CATEGORY_BLOCK << 16) | 26,
    LOG_EVENT_CLEAR_LOG         = (LOG_CATEGORY_INFO << 16) | 27,
    LOG_EVENT_INET_AUTO_CONN     = (LOG_CATEGORY_AUTOCONN << 16) | 28,
    LOG_EVENT_WL_BY_SCHE         = (LOG_CATEGORY_WLSCHE << 16) | 29,
    LOG_EVENT_USB_ATTACH         = (LOG_CATEGORY_INFO << 16) | 30,
    LOG_EVENT_USB_DETACH         = (LOG_CATEGORY_INFO << 16) | 31,

    LOG_EVENT_ALL               = (LOG_CATEGORY_ALL << 16) | 0xFFFF
} log_event;

#define MAX_USB_DEV_NUM     20		/* defined in usbWizCgi2.c */
#define MAX_USB_DEV_NAME    32		/* defined in usbWizCgi2.c */
#define MAX_USB_VOL_NAME    32		/* defined in usbWizCgi2.c */

#define ATTACHED_USB_STORAGE_INFORMATION	"/tmp/attached_usb_storage_info"

typedef struct usbEntry_s
{
    int     majorNum;
    int		minorNum;
    char    devName[MAX_USB_DEV_NAME];
    char    volumeName[MAX_USB_VOL_NAME];
}usbEntry_t;
/* Bob added end 04/01/2011, to log usb information */

/* Foxconn modified start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
int usb_mount_block(int major_no, int minor_no, char *mntdev, char *pUsbPort)
#else /* R6300v2 */
int usb_mount_block(int major_no, int minor_no, char *mntdev)
#endif /* R6300v2 */
/* Foxconn modified end, Wins, 04/11/2011 */
{
    char source[128];
    char target[128],target0[128];
    char buf[128];
    FILE *fp = NULL;
    int i;
    char line[150] = "";
    int  usb_approved_flag = 1; /* approved device by default */
    char max_sectors_file[128];
    char letter = 'a' + 16*(major_no/64) + minor_no/16;

    /* check approved devices */
    if (acosNvramConfig_match("enable_any_usb_dev", "0")) {
        if ((approved_serial(mntdev))==1)
            usb_approved_flag = 1;
        else
            usb_approved_flag = 0;
    }
    sprintf(source, "%s", mntdev);
    if (usb_approved_flag == 1) {
        snprintf(target, 128, "/tmp/mnt/usb%d/part%d", 16*(major_no/64) + minor_no/16, minor_no%16);
    } else {
        snprintf(target, 128, "/tmp/mnt/not_approved%dpart%d", 16*(major_no/64) + minor_no/16, minor_no%16);
        mkdir(target, 0777);
    }
    
    snprintf(target0, 128, "/tmp/mnt/usb%d/part0", 16*(major_no/64) + minor_no/16, minor_no);
    if(minor_no%16)
    	umount(target0);

#ifdef USB_DEBUG
    cprintf("%s:%d mount source =%s, target=%s\n", __func__, __LINE__, source, target);
#endif
    if (0 != wrapped_mount(source, target)) {
#ifdef USB_DEBUG
        cprintf("%s:%d error mouting disk\n",__func__, __LINE__);
#endif
        system("killall -SIGHUP httpd"); /* foxconn add ken chen, 11/08/2013, re-enable smb just in case other usb disks are live */
        return 0;
    }
    /* Bob added start 04/01/2011, to log usb activities */
    usbEntry_t curUsbDev[MAX_USB_DEV_NUM] = {0};
    char volume_path[128];
    char vendor_path[] = "/sys/block/sdx/device/vendor";
    char model_path[] = "/sys/block/sdx/device/model";
    char log_buf[256];
    char tmp[128];
    int j;
   
    if (fp = fopen (ATTACHED_USB_STORAGE_INFORMATION, "r"))
    {
    	fread( (char *)curUsbDev, sizeof(curUsbDev), 1, fp);
    	fclose (fp);
    }
    
    for(i=0; i<MAX_USB_DEV_NUM; i++)
    {
    	if(curUsbDev[i].majorNum==0 && curUsbDev[i].minorNum==0)
    	{
    		curUsbDev[i].majorNum = major_no;
    		curUsbDev[i].minorNum = minor_no;
    		sprintf(volume_path, VOL_NAME_FILE, mntdev+5);
    		if (fp = fopen (volume_path, "r"))
        	{
            	fread (curUsbDev[i].volumeName, MAX_USB_VOL_NAME-1, 1, fp);
            	fclose (fp);
            	if (strlen(curUsbDev[i].volumeName) > 1)
            	{
            		for(j=strlen(curUsbDev[i].volumeName)-1; j>0; j--)
            		{
            			if(curUsbDev[i].volumeName[j] != '\n' && curUsbDev[i].volumeName[j] != ' ')
            			break;
            		}
            		curUsbDev[i].volumeName[j+1]='\0'; /* strip '\n' and ' ' */
            	}
        	}
        	
        	/* mntdev is something like this /dev/sdax */
    		snprintf(vendor_path, sizeof(vendor_path), "/sys/block/sd%c/device/vendor", *(mntdev+7));
            if (fp = fopen (vendor_path, "r"))
        	{
        		fgets(tmp, sizeof(tmp), fp);
            	fclose (fp);
            	
            	if (strlen(tmp) > 1)
            	{
            		for(j=strlen(tmp)-1; j>0; j--)
            		{
            			if(tmp[j] != '\n' && tmp[j] != ' ')
            			break;
            		}
            		tmp[j+1]='\0'; /* strip '\n' and ' ' */
            	}
            	sprintf(buf, "%s ",tmp); /* vendor */
        	}
        	
        	
        	/* mntdev is something like this /dev/sdax */
    		snprintf(model_path, sizeof(model_path), "/sys/block/sd%c/device/model", *(mntdev+7));
            if (fp = fopen (model_path, "r"))
        	{
            	fgets(tmp, sizeof(tmp), fp);
            	fclose (fp);
            	if (strlen(tmp) > 1)
            	{
            		for(j=strlen(tmp)-1; j>0; j--)
            		{
            			if(tmp[j] != '\n' && tmp[j] != ' ')
            			break;
            		}
            		tmp[j+1]='\0'; /* strip '\n' and ' ' */
            	}
        	}
        	strcat(buf, tmp);
        	strcpy(curUsbDev[i].devName, buf);
    		break;
    	}
    }
    
    if (fp = fopen (ATTACHED_USB_STORAGE_INFORMATION, "w+"))
    {
    	fwrite( (char *)curUsbDev, sizeof(curUsbDev), 1, fp);
    	fclose (fp);
    }
    
    if(i < MAX_USB_DEV_NUM)
    {
    	sprintf(log_buf, "[USB device attached] The USB storage device %s (%s) is attached to the router,", curUsbDev[i].devName, curUsbDev[i].volumeName);
        ambitWriteLog(log_buf, strlen(log_buf), LOG_EVENT_USB_ATTACH);
    }
    /* Bob added end 04/01/2011, to log usb activities */
    
    /* Foxconn added start pling 02/08/2010 */
    /* Modify the USB max_sector parameter to improve performance.
     * Rule: if < 128, set to 128
     *       if between 128 and 512, then double.
     */
    sprintf(max_sectors_file, "/sys/block/sd%c/device/max_sectors", letter);

    fp = fopen(max_sectors_file, "r");
    if (fp != NULL) {
        int max_sectors;
        fgets(line, sizeof(line), fp);
        fclose(fp);
        max_sectors = atoi(line);
        if (max_sectors < 128) {
            sprintf(buf, "echo 128 > %s", max_sectors_file);
            system(buf);
        } else if (max_sectors > 128 && max_sectors < 512) {
            sprintf(buf, "echo %d > %s", max_sectors * 2, max_sectors_file);
            system(buf);
        }
    }
    /* Foxconn added end pling 02/08/2010 */

    nvram_set("usb_dev_no_change", "0");
    /* send signal to httpd ,create link ,2009/05/07, jenny*/
    nvram_set("usb_mount_signal", "1");
    /* to avoid user can't detect USB device when power on,
     * check httpd's status before send signal to httpd
     */
    for (i=0; i<5; i++) {
        if (0 == access("/var/run/httpd.pid", R_OK))
            break;
        sleep(2);
    }
    /* kill -SIGHUP to httpd, so usbLoadSettings() can collect the 
     * updated info */
    system("killall -SIGHUP httpd"); /* wklin modified, 03/23/2011 */
#ifdef INCLUDE_USB_LED
    /* Foxconn modified start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
    int nDevice, nPart;
    char usb_device[4], usb_part[4];
    parse_target_path(target, &nDevice, &nPart);
    sprintf(usb_device, "%d", nDevice);
    sprintf(usb_part, "%d", nPart);
    add_into_mnt_file(pUsbPort, usb_device, usb_part);
    usb_dual_led();
#else /* R6300v2 */
    usb_led();
#endif /* R6300v2 */
    /* Foxconn modified end, Wins, 04/11/2011 */
#endif
    return 0;
}

/* Foxconn modified start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
int usb_umount_block(int major_no, int minor_no, char *pUsbPort)
#else /* R6300v2 */
int usb_umount_block(int major_no, int minor_no)
#endif /* R6300v2 */
/* Foxconn modified end, Wins, 04/11/2011 */
{
    int rval;
    int rval2;
    char path[128];
    int device, part;
    
	/* Bob added start 04/01/2011, to log usb activities */
	int i;
	FILE *fp = NULL;
	char log_buf[256];
	usbEntry_t curUsbDev[MAX_USB_DEV_NUM];
	
	if (fp = fopen (ATTACHED_USB_STORAGE_INFORMATION, "r+"))
    {
    	fread( (char *)curUsbDev, sizeof(curUsbDev), 1, fp);
    	for(i=0; i<MAX_USB_DEV_NUM; i++)
    	{
    		if(curUsbDev[i].majorNum==major_no && curUsbDev[i].minorNum==minor_no)
    		{
    			curUsbDev[i].majorNum = 0;
    			curUsbDev[i].minorNum = 0;
    			fseek(fp, 0, SEEK_SET);
    			fwrite( (char *)curUsbDev, sizeof(curUsbDev), 1, fp);
    			sprintf(log_buf, "[USB device detached] The USB storage device %s (%s) is removed from the router,", curUsbDev[i].devName, curUsbDev[i].volumeName);
        		ambitWriteLog(log_buf, strlen(log_buf), LOG_EVENT_USB_DETACH);
    		}
    	}
    	fclose (fp);
    }

    /* Bob added end 04/01/2011, to log usb activities */

    snprintf(path, 128, "/tmp/mnt/usb%d/part%d", 16*(major_no/64) + minor_no/16, minor_no%16);
    rval = umount2(path, MNT_DETACH);
    snprintf(path, 128, "/tmp/mnt/not_approved%dpart%d", 16*(major_no/64) + minor_no/16, minor_no%16);
    rval2 = umount2(path, MNT_DETACH);

    if (rval != 0 && rval2 != 0) {
        system("killall -SIGHUP httpd");    /* foxconn add ken chen, 11/08/2013, re-enable smb just in case other usb disks are live */
        return 0;
    }
		
    /* only remove the volume name file of the unmounted device */
    device = minor_no/16;
    part = minor_no%16;

    if (part)
        sprintf(path, "/tmp/usb_vol_name/sd%c%d", 'a' + device, part);
    else
        sprintf(path, "/tmp/usb_vol_name/sd%c", 'a' + device);
    unlink(path);

    system("killall -SIGHUP httpd"); /* wklin modified, 03/25/2011 */
#ifdef INCLUDE_USB_LED
/* Foxconn modified start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
    char usb_device[4];
    char usb_part[4];
    sprintf(usb_device, "%d", device);
    sprintf(usb_part, "%d", part);
    remove_from_mnt_file(pUsbPort, usb_device, usb_part);
    usb_dual_led();
#else /* R6300v2 */
    usb_led();
#endif /* R6300v2 */
/* Foxconn modified end, Wins, 04/11/2011 */
#endif    
    nvram_set("usb_dev_no_change", "0");
    return 0;
}
/* Foxconn add end, Tony W.Y. Wang, 10/20/2010 */
/* Foxconn modify end, Max Ding, 12/07/2010 */

#define LOCK_FILE      "/tmp/hotplug_lock"

/* foxconn wklin added start, 01/27/2011 */
static int in_mount_list(char *dev_name) 
{
    FILE *fp;
    char buf[256];
    int found = 0;

    if (!dev_name)
        return 0;

    fp = fopen("/proc/mounts", "r");
    if (fp) {
        while (fgets(buf, sizeof(buf), fp) != NULL) {
			if (0 == strncmp(dev_name, buf, strlen(dev_name))) {
                found = 1;
				break;
			}
		}
		fclose(fp);
	}
    if (found)
        return 1;
    else
        return 0;
}
/* foxconn wklin added end, 01/27/2011 */
#if defined(LINUX_2_6_36)

#define PHYSDEVPATH_DEPTH_USB_LPORT	4	/* Depth is from 0 */
#define UMOUNT_CMD	"umount -l %s"		/* Use lazy command to solve device busy issue */


/* Optimize performance */
#define READ_AHEAD_KB_BUF	1024
#define READ_AHEAD_CONF	"/sys/block/%s/queue/read_ahead_kb"

static void
optimize_block_device(char *devname)
{
	char blkdev[8] = {0};
	char read_ahead_conf[64] = {0};
	char cmdbuf[64] = {0};
	int err;

	memset(blkdev, 0, sizeof(blkdev));
	strncpy(blkdev, devname, 3);
	sprintf(read_ahead_conf, READ_AHEAD_CONF, blkdev);
	sprintf(cmdbuf, "echo %d > %s", READ_AHEAD_KB_BUF, read_ahead_conf);
	err = system(cmdbuf);
	hotplug_dbg("err = %d\n", err);

	if (err) {
		hotplug_dbg("read ahead: unsuccess %d!\n", err);
	}
}
#else
#define UMOUNT_CMD	"umount %s"
#define	optimize_block_device(devname)
#endif	/* LINUX_2_6_36 */


/* hotplug block, called by LINUX26 */
int
hotplug_block(void)
{
	char *action = NULL, *minor = NULL;
	char *major = NULL, *driver = NULL;
    /* Foxconn added start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
	char *devpath = NULL;
#endif /* R6300v2 */
    /* Foxconn added end, Wins, 04/11/2011 */
	int minor_no, major_no, device, part;
	int err;
	int retry = 3, lock_fd = -1;
	char cmdbuf[64] = {0};
	char mntdev[32] = {0};
	char mntpath[32] = {0};
	char devname[10] = {0};
	struct flock lk_info = {0};

	if (!(action = getenv("ACTION")) ||
	    !(minor = getenv("MINOR")) ||
	    !(driver = getenv("PHYSDEVDRIVER")) ||
        /* Foxconn added start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
	    !(devpath = getenv("PHYSDEVPATH")) ||
#endif /* R6300v2 */
        /* Foxconn added end, Wins, 04/11/2011 */
	    !(major = getenv("MAJOR")))
	{
		return EINVAL;
	}

	hotplug_dbg("env %s %s!\n", action, driver);
	if (strncmp(driver, "sd", 2))
	{
		return EINVAL;
	}

	if ((lock_fd = open(LOCK_FILE, O_RDWR|O_CREAT, 0666)) < 0) {
		hotplug_dbg("Failed opening lock file LOCK_FILE: %s\n", strerror(errno));
		return -1;
	}

	while (--retry) {
		lk_info.l_type = F_WRLCK;
		lk_info.l_whence = SEEK_SET;
		lk_info.l_start = 0;
		lk_info.l_len = 0;
		if (!fcntl(lock_fd, F_SETLKW, &lk_info)) break;
	}

	if (!retry) {
		hotplug_dbg("Failed locking LOCK_FILE: %s\n", strerror(errno));
		close(lock_fd);
		unlink(LOCK_FILE);
		return -1;
	}

    /* Foxconn added start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
    devpath = getenv("PHYSDEVPATH");
    char usb_port[512];
    memset(usb_port, 0x0, sizeof(usb_port));    /* Foxconn added, Wins, 06/30/2011 */
    get_usb_port(devpath, usb_port);
#endif /* R6300v2 */
    /* Foxconn added end, Wins, 04/11/2011 */

	major_no = atoi(major);
	minor_no = atoi(minor);
	device = minor_no/16;
	part = minor_no%16;

    /* foxconn wklin modified start, 01/17/2011 */
	/* sprintf(devname, "%s%c%d", driver, 'a' + device, part); */
    if (part)
        sprintf(devname, "%s%c%d", driver, 'a' + device, part);
    else
        sprintf(devname, "%s%c", driver, 'a' + device);
    /* foxconn wklin modified end, 01/17/2011 */
	sprintf(mntdev, "/dev/%s", devname);
	sprintf(mntpath, "/media/%s", devname);
	if (!strcmp(action, "add")) {
		if ((devname[2] > 'd') || (devname[2] < 'a')) {
			hotplug_dbg("bad dev!\n");
			goto exit;
		}

		hotplug_dbg("adding disk...\n");

		err = mknod(mntdev, S_IRWXU|S_IFBLK, makedev(major_no, minor_no));
		hotplug_dbg("err = %d\n", err);

		/* foxconn wklin modified start, 01/17/2011 */
        /*
		err = mkdir(mntpath, 0777);
		hotplug_dbg("err %s= %s\n", mntpath, strerror(errno));
        sprintf(cmdbuf, "mount -t vfat %s %s", mntdev, mntpath);
		err = system(cmdbuf);
        */
		USB_LOCK();
        /* foxconn wklin added start, 01/19/2011, avoid zombie issue */
        system("killall -9 minidlna.exe");
        system("killall -9 bftpd 2> /dev/null");
#ifndef CONFIG_SAMBA_NO_RESTART
        system("killall -9 smbd 2> /dev/null");
#endif
        system("killall -9 nmbd 2> /dev/null");
        usleep(500000);
        /* foxconn wklin added end, 01/19/2011 */
        /* Foxconn modified start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined (R7000) || defined(R8000)
		err = usb_mount_block(major_no, minor_no, mntdev, usb_port);
#else /* R6300v2 */
		err = usb_mount_block(major_no, minor_no, mntdev);
#endif /* R6300v2 */
        /* Foxconn modified end, Wins, 04/11/2011 */
		USB_UNLOCK();
        /* foxconn wklin modified end, 01/17/2011 */
		hotplug_dbg("err = %d\n", err);
		/*Foxconn add start by Hank 08/27/2013*/
		/*add for change read_ahead_kb to speed up USB throughput*/
		optimize_block_device(devname);
		/*Foxconn add end by Hank 08/27/2013*/

		if (err) {
			hotplug_dbg("unsuccess %d!\n", err);
			unlink(mntdev);
			rmdir(mntpath);
		}
		else {
			/* Start usb services */
            /* foxconn wklin removed start, 01/18/2011, not used for home router */
			/* usb_start_services(); */
            /* foxconn wklin removed end, 01/18/2011 */
			/* Optimize performance */
			optimize_block_device(devname);
		}
	} else if (!strcmp(action, "remove")) {
		/* Stop usb services */
        /* foxconn wklin removed start, 01/18/2011, not used for home router */
		/* usb_stop_services(); */
        /* foxconn wklin removed end, 01/18/2011 */
		hotplug_dbg("removing disk %s...\n", devname);
        /* foxconn wklin added start, 01/27/2011 */
        /* check if devname is in mount list */
        sprintf(mntdev, "/dev/%s", devname);
        if (!in_mount_list(mntdev)) {
#ifdef USB_DEBUG
            cprintf("%s:%d %s not in mount list, just return...\n",
                    __func__, __LINE__, devname);
#endif
            /* Foxconn added start, Wins, 06/30/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
            if ((minor_no%16) > 0)
            {
                char usb_device[4], usb_part[4];
                memset(usb_device, 0x0, sizeof(usb_device));
                memset(usb_part, 0x0, sizeof(usb_part));
                sprintf(usb_device, "%d", (minor_no/16));
                sprintf(usb_part, "%d", (minor_no%16));
                remove_from_mnt_file(usb_port, usb_device, usb_part);
            }
#endif /* R6300v2 */
            /* Foxconn added end, Wins, 06/30/2011 */
            return 0;
        }
        /* foxconn wklin added end, 01/27/2011 */
		/* foxconn wklin modified start, 01/17/2011 */
        /* 
        sprintf(cmdbuf, "umount %s", mntpath);
		err = system(cmdbuf);
        */
		USB_LOCK();
        /* foxconn wklin added start, 01/19/2011, avoid zombie issue */
        system("killall -9 minidlna.exe");
        system("killall -9 bftpd 2> /dev/null");
#ifndef CONFIG_SAMBA_NO_RESTART
        system("killall -9 smbd 2> /dev/null");
#endif
        system("killall -9 nmbd 2> /dev/null");
        usleep(500000);
        /* foxconn wklin added end, 01/19/2011 */
        /* Foxconn modified start, Wins, 04/11/2011 */
#if defined(R6300v2) || defined(R7000) || defined(R8000)
		usb_umount_block(major_no, minor_no, usb_port);
#else /* R6300v2 */
		usb_umount_block(major_no, minor_no);
#endif /* R6300v2 */
        /* Foxconn modified end, Wins, 04/11/2011 */
		USB_UNLOCK();
        /* foxconn wklin modified end, 01/17/2011 */
		hotplug_dbg("err = %d\n", err);

		if (err) {
			hotplug_dbg("unsuccess %d!\n", err);
			unlink(mntdev);
			rmdir(mntpath);
		}
		else {
			/* Start usb services */
            /* foxconn wklin removed start, 01/18/2011, not used for home router */
			/* usb_start_services(); */
            /* foxconn wklin removed end, 01/18/2011 */
		}
	} else {
		hotplug_dbg("not support action!\n");
	}

exit:
	close(lock_fd);
	unlink(LOCK_FILE);

#ifdef __CONFIG_SAMBA__
#if defined(LINUX_2_6_36)
	/* Restart samba service since USB device is plugged or unplugged */
	if (err == 0)
		restart_samba();
#endif
#endif /* __CONFIG_SAMBA__ */

	return 0;
}

/* hotplug usb, called by LINUX24 or USBAP */
int
hotplug_usb(void)
{
	char *device, *interface;
	char *action;
	int class, subclass, protocol;
	char *product;
	int need_interface = 1;

	if (!(action = getenv("ACTION")))
		return EINVAL;

	product = getenv("PRODUCT");

	cprintf("hotplug detected product:  %s\n", product);

	if ((device = getenv("TYPE"))) {
		sscanf(device, "%d/%d/%d", &class, &subclass, &protocol);
		if (class != 0)
			need_interface = 0;
	}

	if (need_interface) {
		if (!(interface = getenv("INTERFACE")))
			return EINVAL;
		sscanf(interface, "%d/%d/%d", &class, &subclass, &protocol);
		if (class == 0)
			return EINVAL;
	}

#ifndef LINUX26
	/* If a new USB device is added and it is of storage class */
	if (class == 8 && subclass == 6 && !strcmp(action, "add")) {
		/* Mount usb disk */
		if (usb_mount_ufd() != 0)
			return ENOENT;
		/* Start services */
		usb_start_services();
		return 0;
	}

	/* If a new USB device is removed and it is of storage class */
	if (class == 8 && subclass == 6 && !strcmp(action, "remove")) {
		/* Stop services */
		usb_stop_services();

		eval("/bin/umount", mntdir);
		return 0;
	}
#endif

#ifdef __CONFIG_USBAP__
	/* download the firmware and insmod wl_high for USBAP */
	if ((!strcmp(product, WL_DOWNLOADER_43236_VEND_ID)) ||
		(!strcmp(product, WL_DOWNLOADER_43526_VEND_ID)) ||
		(!strcmp(product, WL_DOWNLOADER_43556_VEND_ID)) ||
		(!strcmp(product, WL_DOWNLOADER_4360_VEND_ID))) {
		if (!strcmp(action, "add")) {
			eval("rc", "restart");
		} else if (!strcmp(action, "remove")) {
			cprintf("wl device removed\n");
		}
	}
#endif /* __CONFIG_USBAP__ */

	return 0;
}

/*
 * Process the file in /proc/mounts to get
 * the mount path and device.
 */
static char mntpath[128] = {0};
static char devpath[128] = {0};

static void
get_mntpath()
{
	FILE *fp;
	char buf[256];

	memset(mntpath, 0, sizeof(mntpath));
	memset(devpath, 0, sizeof(devpath));

	if ((fp = fopen("/proc/mounts", "r")) != NULL) {
		while (fgets(buf, sizeof(buf), fp) != NULL) {
			if (strstr(buf, mntdir) != NULL) {
				sscanf(buf, "%s %s", devpath, mntpath);
				break;
			}
		}
		fclose(fp);
	}
}

static void
dump_disk_type(char *path)
{
	char *argv[3];

	argv[0] = "/usr/sbin/disktype";
	argv[1] = path;
	argv[2] = NULL;
	_eval(argv, ">/tmp/disktype.dump", 0, NULL);

	return;
}

#ifndef LINUX26
/*
 * Check if the UFD is still connected because the links
 * created in /dev/discs are not removed when the UFD is
 * unplugged.
 */
static bool
usb_ufd_connected(char *str)
{
	uint host_no;
	char proc_file[128];

	/* Host no. assigned by scsi driver for this UFD */
	host_no = atoi(str);

	sprintf(proc_file, "/proc/scsi/usb-storage-%d/%d", host_no, host_no);

	if (eval("/bin/grep", "-q", "Attached: Yes", proc_file) == 0)
		return TRUE;
	else
		return FALSE;
}

static int
usb_mount_ufd(void)
{
	DIR *dir;
	struct dirent *entry;
	char path[128];

	/* Is this linux24? */
	if ((dir = opendir("/dev/discs")) == NULL)
		return EINVAL;

	/* Scan through entries in the directories */
	while ((entry = readdir(dir)) != NULL) {
		if ((strncmp(entry->d_name, "disc", 4)))
			continue;

		/* Files created when the UFD is inserted are not
		 * removed when it is removed. Verify the device
		 * is still inserted.
		 * Strip the "disc" and pass the rest of the string.
		 */
		if (usb_ufd_connected(entry->d_name+4) == FALSE)
			continue;

		sprintf(path, "/dev/discs/%s/disc", entry->d_name);

		dump_disk_type(path);

		/* Check if it has FAT file system */
		if (eval("/bin/grep", "-q", "FAT", "/tmp/disktype.dump") == 0) {
			/* If it is partioned, mount first partition else raw disk */
			if (eval("/bin/grep", "-q", "Partition", "/tmp/disktype.dump") == 0)
			{
				char part[10], *partitions, *next;
				struct stat tmp_stat;

				partitions = "part1 part2 part3 part4";
				foreach(part, partitions, next) {
					sprintf(path, "/dev/discs/%s/%s", entry->d_name, part);
					if (stat(path, &tmp_stat) == 0)
						break;
				}

				/* Not found, no need to do further prcoessing */
				if (part[0] == 0)
					return EINVAL;
			}

			/* Mount here */
			eval("/bin/mount", "-t", "vfat", path, "/mnt");
			return 0;
		}
	}

	return EINVAL;
}
#endif	/* !LINUX26 */

/*
 * Mount the path and look for the WCN configuration file.
 * If it exists launch wcnparse to process the configuration.
 */
static int
get_wcn_config()
{
	int ret = ENOENT;
	struct stat tmp_stat;

	if (stat("/mnt/SMRTNTKY/WSETTING.WFC", &tmp_stat) == 0) {
		eval("/usr/sbin/wcnparse", "-C", "/mnt", "SMRTNTKY/WSETTING.WFC");
		ret = 0;
	}
	return ret;
}


#if defined(__CONFIG_DLNA__)
static void
start_dlna()
{
	char *dlna_enable = nvram_safe_get("dlna_enable");

	if (strcmp(dlna_enable, "1") == 0) {
		/* Check mount device */
		if (strlen(mntpath) == 0 || strlen(devpath) == 0)
			return;

		cprintf("Start bcmmserver.\n");
		eval("sh", "-c", "bcmmserver&");
	}
}

static void
stop_dlna()
{
	cprintf("Stop bcmmserver.\n");
	eval("killall", "bcmmserver");
}
#endif	/* __CONFIG_DLNA_DMS__ */

/* Handle hotplugging of UFD */
static int
usb_start_services(void)
{
	/* Read mount path and dump to file */
	get_mntpath();

	dump_disk_type(devpath);

	/* Check WCN config */
	if (get_wcn_config() == 0)
		return 0;


#if defined(__CONFIG_DLNA__)
	start_dlna();
#endif

	return 0;
}

static int
usb_stop_services(void)
{

#if defined(__CONFIG_DLNA__)
	stop_dlna();
#endif

	return 0;
}


#if (defined INCLUDE_USB_LED)
#include "wps_led.h"
#include <sys/ioctl.h>

int hotplug_NetUSB(void)
{
    char *devpath;
    char *action;
    char usb_port[8];
    int usb_port_num;
    char command[128];
    
    int has_usb1 = 0;
    int has_usb2 = 0;
        
    FILE *fp;
    int  fd;
    char line[128];
    
    devpath = getenv("PHYSDEVPATH");
    if (!devpath)
        return -1;

    sprintf(command, "cat /proc/%d/environ > /dev/console", getpid());
    system(command);

    action = getenv("ACTION");
    sprintf(command, "echo 'action = %s' > dev/console", action);
    system(command);

    memset(usb_port, 0, sizeof(usb_port));
    get_usb_port(devpath, usb_port);
    usb_port_num = atoi(usb_port);
                    
    if (usb_port_num == 1)
    {
        fp = fopen("/proc/NetUSB/0/device", "r");
        if (fp)
        {
            fgets(line, sizeof(line), fp);
            if (strlen(line) > 2)
            {
                has_usb1 = 1;
            }
            fclose(fp); /* close /proc/NetUSB/0/device */
        }
    }
    else
    if (usb_port_num == 2)
    {
        fp = fopen("/proc/NetUSB/1/device", "r");
        if (fp)
        {
            fgets(line, sizeof(line), fp);
            if (strlen(line) > 2)
            {
                has_usb2 = 1;
            }
            fclose(fp); /* close /proc/NetUSB/1/device */
        }
    }

#if (defined GPIO_EXT_CTRL) /* WNDR4000 */
    if (has_usb1 || has_usb2)
        system("gpio usbled 1");
    else
        system("gpio usbled 0");
#else
    fd = open("/dev/wps_led", O_RDWR);
    if (fd >= 0) {
        if (has_usb1)
        {
            ioctl(fd, USB_LED_STATE_ON, 1);
        }
        else
        {
            ioctl(fd, USB_LED_STATE_OFF, 1);
        }

        if (has_usb2)
        {
            ioctl(fd, USB2_LED_STATE_ON, 1);
        }
        else
        {
            ioctl(fd, USB2_LED_STATE_OFF, 1);
        }

        close(fd);
    }
#endif /* GPIO_EXT_CTRL */

    system("killall -SIGHUP httpd"); /*foxconn Han edited, 08/18/2015 SIGHUP httpd trigger PRINTER LED  turn off*/
    return 0;
}
#endif /*INCLUDE_USB_LED*/

