/*
 * Router rc control script
 *
 * Copyright (C) 2015, Broadcom Corporation. All Rights Reserved.
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
 * $Id: rc.c 551076 2015-04-22 09:02:54Z $
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h> /* for open */
#include <string.h>
#include <sys/klog.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/utsname.h> /* for uname */
#include <net/if_arp.h>
#include <dirent.h>

#include <epivers.h>
#include <router_version.h>
#include <mtd.h>
#include <shutils.h>
#include <rc.h>
#include <netconf.h>
#include <nvparse.h>
#include <bcmdevs.h>
#include <bcmparams.h>
#include <bcmnvram.h>
#include <wlutils.h>
#include <ezc.h>
#include <pmon.h>
#include <bcmconfig.h>
#include <etioctl.h>
#if defined(__CONFIG_WAPI__) || defined(__CONFIG_WAPI_IAS__)
#include <wapi_utils.h>
#endif /* __CONFIG_WAPI__ || __CONFIG_WAPI_IAS__ */
#ifdef __BRCM_GENERIC_IQOS__
#include "bcmIqosDef.h"
#endif

/* foxconn added start, zacker, 09/17/2009, @wps_led */
#include <fcntl.h>
#include <wps_led.h>
/* foxconn added end, zacker, 09/17/2009, @wps_led */

/*fxcn added by dennis start,05/03/2012, fixed guest network can't reconnect issue*/
#define MAX_BSSID_NUM       4
#define MIN_BSSID_NUM       2
/*fxcn added by dennis end,05/03/2012, fixed guest network can't reconnect issue*/

#ifdef __CONFIG_NAT__
static void auto_bridge(void);
#endif	/* __CONFIG_NAT__ */

#include <sys/sysinfo.h> /* foxconn wklin added */
#ifdef __CONFIG_EMF__
extern void load_emf(void);
#endif /* __CONFIG_EMF__ */

#ifdef __CONFIG_DHDAP__
#define MAX_FW_PATH	512
#endif /* __CONFIG_DHDAP__ */

static void restore_defaults(void);
static void sysinit(void);
static void rc_signal(int sig);
/* Foxconn added start, Wins, 05/16/2011, @RU_IPTV */
#if defined(CONFIG_RUSSIA_IPTV)
static int is_russia_specific_support (void);
static int is_china_specific_support (void); /* Foxconn add, Edward zhang, 09/05/2012, @add IPTV support for PR SKU*/
#endif /* CONFIG_RUSSIA_IPTV */
/* Foxconn added end, Wins, 05/16/2011, @RU_IPTV */
/*Foxconn add start, edward zhang, 2013/07/03*/
#ifdef VLAN_SUPPORT
static int getVlanname(char vlanname[C_MAX_VLAN_RULE][C_MAX_TOKEN_SIZE]);
static int getVlanRule(vlan_rule vlan[C_MAX_VLAN_RULE]);
static int getTokens(char *str, char *delimiter, char token[][C_MAX_TOKEN_SIZE], int maxNumToken);
#endif
/*Foxconn add end, edward zhang, 2013/07/03*/
extern struct nvram_tuple router_defaults[];

#define RESTORE_DEFAULTS() \
    (!nvram_match("restore_defaults", "0") || nvram_invmatch("os_name", "linux"))
#ifdef LINUX_2_6_36
    static int
coma_uevent(void)
{
    char *modalias = NULL;
    char lan_ifname[32], *lan_ifnames, *next;

    modalias = getenv("MODALIAS");
    if (!strcmp(modalias, "platform:coma_dev")) {

        /* down WiFi adapter */
        lan_ifnames = nvram_safe_get("lan_ifnames");
        foreach(lan_ifname, lan_ifnames, next) {
            if (!strncmp(lan_ifname, "eth", 3)) {
                eval("wl", "-i", lan_ifname, "down");
            }
        }

        system("echo \"2\" > /proc/bcm947xx/coma");
    }
    return 0;
}
#endif /* LINUX_2_6_36 */

#define RESTORE_DEFAULTS() (!nvram_match("restore_defaults", "0") || nvram_invmatch("os_name", "linux"))

/* for WCN support, Foxconn added start by EricHuang, 12/13/2006 */
static void convert_wlan_params(void)
{
    int config_flag = 0; /* Foxconn add, Tony W.Y. Wang, 01/06/2010 */

    /* Foxconn added start pling 03/05/2010 */
    /* Added for dual band WPS req. for WNDR3400 */
#define MAX_SSID_LEN    32
    char wl0_ssid[64], wl1_ssid[64];
#if defined(R8000)
    char wl2_ssid[64];
#endif
    /* first check how we arrived here?
     * 1. or by "add WPS client" in unconfigured state.
     * 2. by external register configure us, 
     */
    strcpy(wl0_ssid, nvram_safe_get("wl0_ssid"));
    strcpy(wl1_ssid, nvram_safe_get("wl1_ssid"));
#if defined(R8000)
    strcpy(wl2_ssid, nvram_safe_get("wl2_ssid"));
#endif
    /* Foxconn modified, Tony W.Y. Wang, 03/24/2010 @WPS random ssid setting */
    if (!nvram_match("wps_start", "none") || nvram_match("wps_pbc_conn_success", "1"))
    {
        /* case 1 above, either via pbc, or gui */
        /* In this case, the WPS set both SSID to be
         *  either "NTRG-2.4G_xxx" or "NTRG-5G_xxx".
         * We need to set proper SSID for each radio.
         */
#define RANDOM_SSID_2G  "NTGR-2.4G_"
#define RANDOM_SSID_5G  "NTGR-5G_"

#if defined(R8000)
#define RANDOM_SSID_5G_1  "NTGR-5G-1_"
#define RANDOM_SSID_5G_2  "NTGR-5G-2_"
#endif

        /* Foxconn modified start pling 05/23/2012 */
        /* Fix a issue where 2.4G radio is disabled, 
         * router uses incorrect random ssid */
        /* if (strncmp(wl0_ssid, RANDOM_SSID_2G, strlen(RANDOM_SSID_2G)) == 0) */
        if (strncmp(wl0_ssid, RANDOM_SSID_2G, strlen(RANDOM_SSID_2G)) == 0 && !nvram_match("wl0_radio","0") && nvram_match("wps_currentRFband", "1"))
        {
            printf("Random ssid 2.4G\n");
            /* Set correct ssid for 5G */
            sprintf(wl1_ssid, "%s%s", RANDOM_SSID_5G_1, 
                    &wl0_ssid[strlen(RANDOM_SSID_2G)]);
            nvram_set("wl1_ssid", wl1_ssid);
#if defined(R8000)
            sprintf(wl2_ssid, "%s%s", RANDOM_SSID_5G_2, 
                    &wl0_ssid[strlen(RANDOM_SSID_2G)]);
            nvram_set("wl2_ssid", wl2_ssid);
#endif
            if (nvram_match("wl_5g_bandsteering", "1"))
                nvram_set("wl2_ssid", wl1_ssid);

            nvram_set("wl1_wpa_psk", nvram_safe_get("wl0_wpa_psk"));
            nvram_set("wl1_akm", nvram_safe_get("wl0_akm"));
            nvram_set("wl1_crypto", nvram_safe_get("wl0_crypto"));
            nvram_set("wl2_wpa_psk", nvram_safe_get("wl0_wpa_psk"));
            nvram_set("wl2_akm", nvram_safe_get("wl0_akm"));
            nvram_set("wl2_crypto", nvram_safe_get("wl0_crypto"));
        }
        else
            if (strncmp(wl1_ssid, RANDOM_SSID_5G, strlen(RANDOM_SSID_5G)) == 0 && !nvram_match("wl1_radio","0") 
                    && nvram_match("wps_currentRFband", "2") && nvram_match("wl2_radio", "0") )
            {
                printf("Random ssid 5G\n");
                /* Set correct ssid for 2.4G */
                sprintf(wl0_ssid, "%s%s", RANDOM_SSID_2G, 
                        &wl1_ssid[strlen(RANDOM_SSID_5G)]);
                nvram_set("wl0_ssid", wl0_ssid);
#if defined(R8000)
                sprintf(wl2_ssid, "%s%s", RANDOM_SSID_5G_2, 
                        &wl1_ssid[strlen(RANDOM_SSID_5G)]);
                nvram_set("wl2_ssid", wl2_ssid);
#endif
                if (nvram_match("wl_5g_bandsteering", "1"))
                    nvram_set("wl2_ssid", wl1_ssid);

                nvram_set("wl0_wpa_psk", nvram_safe_get("wl1_wpa_psk"));
                nvram_set("wl0_akm", nvram_safe_get("wl1_akm"));
                nvram_set("wl0_crypto", nvram_safe_get("wl1_crypto"));
                nvram_set("wl2_wpa_psk", nvram_safe_get("wl1_wpa_psk"));
                nvram_set("wl2_akm", nvram_safe_get("wl1_akm"));
                nvram_set("wl2_crypto", nvram_safe_get("wl1_crypto"));
            }
#if defined(R8000)
            else
                if (strncmp(wl2_ssid, RANDOM_SSID_5G, strlen(RANDOM_SSID_5G)) == 0 && !nvram_match("wl2_radio","0")
                        && nvram_match("wps_currentRFband", "2"))
                {
                    printf("Random ssid 5G_2\n");
                    /* Set correct ssid for 2.4G */
                    sprintf(wl0_ssid, "%s%s", RANDOM_SSID_2G, 
                            &wl2_ssid[strlen(RANDOM_SSID_5G)]);
                    nvram_set("wl0_ssid", wl0_ssid);
                    sprintf(wl1_ssid, "%s%s", RANDOM_SSID_5G_1, 
                            &wl2_ssid[strlen(RANDOM_SSID_5G)]);
                    nvram_set("wl1_ssid", wl1_ssid);

                    if (nvram_match("wl_5g_bandsteering", "1"))
                        nvram_set("wl1_ssid", wl2_ssid);

                    nvram_set("wl0_wpa_psk", nvram_safe_get("wl2_wpa_psk"));
                    nvram_set("wl0_akm", nvram_safe_get("wl2_akm"));
                    nvram_set("wl0_crypto", nvram_safe_get("wl2_crypto"));
                    nvram_set("wl1_wpa_psk", nvram_safe_get("wl2_wpa_psk"));
                    nvram_set("wl1_akm", nvram_safe_get("wl2_akm"));
                    nvram_set("wl1_crypto", nvram_safe_get("wl2_crypto"));

                }
#endif
        nvram_unset("wps_pbc_conn_success");
    }
    else
    {
        /* case 2 */
        /* now check whether external register is from:
         * 1. UPnP,
         * 2. 2.4GHz radio
         * 3. 5GHz radio
         * 4. 5GHz radio 2
         */
        if (nvram_match("wps_is_upnp", "1"))
        {
            /* Case 1: UPnP: wired registrar */
            /* SSID for both interface should be same already.
             * So nothing to do.
             */
            printf("Wired External registrar!\n");
        }
        else
            if (nvram_match("wps_currentRFband", "1"))
            {
                /* Case 2: 2.4GHz radio */
                /* Need to add "-5G" to the SSID of the 5GHz band */
                char ssid_suffix[] = "-5G";
#if (defined R8000)
                char ssid_suffix_2[] = "-5G-2";
#endif
                if (MAX_SSID_LEN - strlen(wl0_ssid) >= strlen(ssid_suffix))
                {
                    printf("2.4G Wireless External registrar 1!\n");
                    /* SSID is not long, so append suffix to wl1_ssid */
                    sprintf(wl1_ssid, "%s%s", wl0_ssid, ssid_suffix);
                }
                else
                {
                    printf("2.4G Wireless External registrar 2!\n");
                    /* SSID is too long, so replace last few chars of ssid
                     * with suffix
                     */
                    strcpy(wl1_ssid, wl0_ssid);
                    strcpy(&wl1_ssid[MAX_SSID_LEN - strlen(ssid_suffix)], ssid_suffix);
                }
#if defined(R8000)
                if (MAX_SSID_LEN - strlen(wl0_ssid) >= strlen(ssid_suffix_2))
                {
                    printf("2.4G Wireless External registrar 1!\n");
                    /* SSID is not long, so append suffix to wl1_ssid */
                    sprintf(wl2_ssid, "%s%s", wl0_ssid, ssid_suffix_2);
                }
                else
                {
                    printf("2.4G Wireless External registrar 2!\n");
                    /* SSID is too long, so replace last few chars of ssid
                     * with suffix
                     */
                    strcpy(wl2_ssid, wl0_ssid);
                    strcpy(&wl2_ssid[MAX_SSID_LEN - strlen(ssid_suffix_2)], ssid_suffix_2);
                }
#endif
                if (strlen(wl1_ssid) > MAX_SSID_LEN)
                    printf("Error wl1_ssid too long (%d)!\n", strlen(wl1_ssid));

#if defined(R8000)
                if (strlen(wl2_ssid) > MAX_SSID_LEN)
                    printf("Error wl2_ssid too long (%d)!\n", strlen(wl2_ssid));
#endif
                nvram_set("wl1_ssid", wl1_ssid);
#if defined(R8000)
                if (nvram_match("wl_5g_bandsteering", "1"))
                    nvram_set("wl2_ssid", wl1_ssid);
                else
                    nvram_set("wl2_ssid", wl2_ssid);
#endif

#if defined(R8000)  /* Foxconn Bob added start 09/29/2014, must sync wifi security as well since WPS of 5G radio 1 is disabled. */
                nvram_set("wl1_wpa_psk", nvram_safe_get("wl0_wpa_psk"));
                nvram_set("wl1_akm", nvram_safe_get("wl0_akm"));
                nvram_set("wl1_crypto", nvram_safe_get("wl0_crypto"));
                nvram_set("wl2_wpa_psk", nvram_safe_get("wl0_wpa_psk"));
                nvram_set("wl2_akm", nvram_safe_get("wl0_akm"));
                nvram_set("wl2_crypto", nvram_safe_get("wl0_crypto"));
#endif
            }
            else
                if (nvram_match("wps_currentRFband", "2"))
                {
                    /* Case 2: 5GHz radio */
                    /* Need to add "-2.4G" to the SSID of the 2.4GHz band */

                    if (nvram_match("wl2_radio", "1"))
                    {
                        /*wps is done with 5G radio 2 */
                        char ssid_suffix[] = "-2.4G";
                        char ssid_suffix_2[] = "-5G-1";

                        if (MAX_SSID_LEN - strlen(wl2_ssid) >= strlen(ssid_suffix))
                        {
                            printf("5G Wireless External registrar 1!\n");
                            /* SSID is not long, so append suffix to wl1_ssid */
                            sprintf(wl0_ssid, "%s%s", wl2_ssid, ssid_suffix);
                        }
                        else
                        {
                            printf("5G Wireless External registrar 2!\n");
                            /* Replace last few chars ssid with suffix */
                            /* SSID is too long, so replace last few chars of ssid
                             * with suffix
                             */
                            strcpy(wl0_ssid, wl2_ssid);
                            strcpy(&wl0_ssid[MAX_SSID_LEN - strlen(ssid_suffix)], ssid_suffix);
                        }
#if (defined R8000)
                        if (MAX_SSID_LEN - strlen(wl2_ssid) >= strlen(ssid_suffix_2))
                        {
                            printf("5G Wireless External registrar 1!\n");
                            /* SSID is not long, so append suffix to wl1_ssid */
                            sprintf(wl1_ssid, "%s%s", wl2_ssid, ssid_suffix_2);
                        }
                        else
                        {
                            printf("5G Wireless External registrar 2!\n");
                            /* Replace last few chars ssid with suffix */
                            /* SSID is too long, so replace last few chars of ssid
                             * with suffix
                             */
                            strcpy(wl1_ssid, wl2_ssid);
                            strcpy(&wl1_ssid[MAX_SSID_LEN - strlen(ssid_suffix_2)], ssid_suffix_2);
                        }
#endif
                        nvram_set("wl0_ssid", wl0_ssid);
#if (defined R8000)
                        if (nvram_match("wl_5g_bandsteering", "1"))
                            nvram_set("wl1_ssid", wl2_ssid);
                        else
                            nvram_set("wl1_ssid", wl1_ssid);
#endif            
#if defined(R8000)  /* Foxconn Bob added start 09/29/2014, must sync wifi security as well since WPS of 5G radio 1 is disabled. */
                        nvram_set("wl1_wpa_psk", nvram_safe_get("wl2_wpa_psk"));
                        nvram_set("wl1_akm", nvram_safe_get("wl2_akm"));
                        nvram_set("wl1_crypto", nvram_safe_get("wl2_crypto"));
                        nvram_set("wl0_wpa_psk", nvram_safe_get("wl2_wpa_psk"));
                        nvram_set("wl0_akm", nvram_safe_get("wl2_akm"));
                        nvram_set("wl0_crypto", nvram_safe_get("wl2_crypto"));
#endif
                    }
                    else if (nvram_match("wl2_radio", "0"))
                    {
                        /*wps is done with 5G radio 1 */
                        char ssid_suffix[] = "-2.4G";
                        char ssid_suffix_2[] = "-5G-2";

                        if (MAX_SSID_LEN - strlen(wl1_ssid) >= strlen(ssid_suffix))
                        {
                            printf("5G Wireless External registrar 1!\n");
                            /* SSID is not long, so append suffix to wl1_ssid */
                            sprintf(wl0_ssid, "%s%s", wl1_ssid, ssid_suffix);
                        }
                        else
                        {
                            printf("5G Wireless External registrar 2!\n");
                            /* Replace last few chars ssid with suffix */
                            /* SSID is too long, so replace last few chars of ssid
                             * with suffix
                             */
                            strcpy(wl0_ssid, wl1_ssid);
                            strcpy(&wl0_ssid[MAX_SSID_LEN - strlen(ssid_suffix)], ssid_suffix);
                        }
#if (defined R8000)
                        if (MAX_SSID_LEN - strlen(wl1_ssid) >= strlen(ssid_suffix_2))
                        {
                            printf("5G Wireless External registrar 1!\n");
                            /* SSID is not long, so append suffix to wl1_ssid */
                            sprintf(wl2_ssid, "%s%s", wl1_ssid, ssid_suffix_2);
                        }
                        else
                        {
                            printf("5G Wireless External registrar 2!\n");
                            /* Replace last few chars ssid with suffix */
                            /* SSID is too long, so replace last few chars of ssid
                             * with suffix
                             */
                            strcpy(wl2_ssid, wl1_ssid);
                            strcpy(&wl2_ssid[MAX_SSID_LEN - strlen(ssid_suffix_2)], ssid_suffix_2);
                        }
#endif
                        nvram_set("wl0_ssid", wl0_ssid);
                        if (nvram_match("wl_5g_bandsteering", "1"))
                            nvram_set("wl2_ssid", wl1_ssid);
                        else
                            nvram_set("wl2_ssid", wl2_ssid);

#if defined(R8000)  /* Foxconn Bob added start 09/29/2014, must sync wifi security as well since WPS of 5G radio 1 is disabled. */
                        nvram_set("wl2_wpa_psk", nvram_safe_get("wl1_wpa_psk"));
                        nvram_set("wl2_akm", nvram_safe_get("wl1_akm"));
                        nvram_set("wl2_crypto", nvram_safe_get("wl1_crypto"));
                        nvram_set("wl0_wpa_psk", nvram_safe_get("wl1_wpa_psk"));
                        nvram_set("wl0_akm", nvram_safe_get("wl1_akm"));
                        nvram_set("wl0_crypto", nvram_safe_get("wl1_crypto"));
#endif
                    }
                }
                else
                    printf("Error! unknown external register!\n");
    }
    /* Foxconn added end pling 03/05/2010 */

    nvram_set("wla_ssid", nvram_safe_get("wl0_ssid"));
    nvram_set("wla_temp_ssid", nvram_safe_get("wl0_ssid"));

    if ( strncmp(nvram_safe_get("wl0_akm"), "psk psk2", 7) == 0 )
    {
        nvram_set("wla_secu_type", "WPA-AUTO-PSK");
        nvram_set("wla_temp_secu_type", "WPA-AUTO-PSK");
        nvram_set("wla_passphrase", nvram_safe_get("wl0_wpa_psk"));

        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */

        if (!nvram_match("wps_randomssid", "") ||
                !nvram_match("wps_randomkey", ""))
        {
            nvram_set("wla_secu_type", "WPA-AUTO-PSK");
            nvram_set("wla_temp_secu_type", "WPA-AUTO-PSK");

            nvram_set("wl0_akm", "psk psk2 ");
            nvram_set("wl0_crypto", "tkip+aes");

            nvram_set("wps_mixedmode", "2");
            //nvram_set("wps_randomssid", "");
            //nvram_set("wps_randomkey", "");
            config_flag = 1;
            /* Since we changed to mixed mode, 
             * so we need to disable WDS if it is already enabled
             */
            if (nvram_match("wla_wds_enable", "1"))
            {
                nvram_set("wla_wds_enable",  "0");
                nvram_set("wl0_wds", "");
                nvram_set("wl0_mode", "ap");
            }
        }
        else
        {
            /* Foxconn added start pling 02/25/2007 */
            /* Disable WDS if it is already enabled */
            if (nvram_match("wla_wds_enable", "1"))
            {
                nvram_set("wla_wds_enable",  "0");
                nvram_set("wl0_wds", "");
                nvram_set("wl0_mode", "ap");
            }
            /* Foxconn added end pling 02/25/2007 */
        }
    }
    else if ( strncmp(nvram_safe_get("wl0_akm"), "psk2", 4) == 0 )
    {
        nvram_set("wla_secu_type", "WPA2-PSK");
        nvram_set("wla_temp_secu_type", "WPA2-PSK");
        nvram_set("wla_passphrase", nvram_safe_get("wl0_wpa_psk"));


        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */
        /* Foxconn added start pling 06/15/2010 */
        if (nvram_match("wl0_crypto", "tkip"))
        {
            /* DTM fix: 
             * Registrar may set to WPA2-PSK TKIP mode.
             * In this case, don't try to modify the
             * security type.
             */
            nvram_unset("wps_mixedmode");
        }
        else
            /* Foxconn added end pling 06/15/2010 */
            if (!nvram_match("wps_randomssid", "") ||
                    !nvram_match("wps_randomkey", ""))
            {
                nvram_set("wla_secu_type", "WPA2-PSK");
                nvram_set("wla_temp_secu_type", "WPA2-PSK");

                nvram_set("wl0_akm", "psk2");
                nvram_set("wl0_crypto", "aes");

                nvram_set("wps_mixedmode", "2");
                //nvram_set("wps_randomssid", "");
                //nvram_set("wps_randomkey", "");
                config_flag = 1;
                /* Since we changed to mixed mode, 
                 * so we need to disable WDS if it is already enabled
                 */
                if (nvram_match("wla_wds_enable", "1"))
                {
                    nvram_set("wla_wds_enable",  "0");
                    nvram_set("wl0_wds", "");
                    nvram_set("wl0_mode", "ap");
                }
            }
            else
            {
                /* Foxconn added start pling 02/25/2007 */
                /* Disable WDS if it is already enabled */
                if (nvram_match("wla_wds_enable", "1"))
                {
                    nvram_set("wla_wds_enable",  "0");
                    nvram_set("wl0_wds", "");
                    nvram_set("wl0_mode", "ap");
                }
                /* Foxconn added end pling 02/25/2007 */
            }
    }
    else if ( strncmp(nvram_safe_get("wl0_akm"), "psk", 3) == 0 )
    {
        nvram_set("wla_secu_type", "WPA-PSK");
        nvram_set("wla_temp_secu_type", "WPA-PSK");
        nvram_set("wla_passphrase", nvram_safe_get("wl0_wpa_psk"));

        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */
        /* Foxconn added start pling 06/15/2010 */
        if (nvram_match("wl0_crypto", "aes"))
        {
            /* DTM fix: 
             * Registrar may set to WPA-PSK AES mode.
             * In this case, don't try to modify the
             * security type.
             */
            nvram_unset("wps_mixedmode");
        }
        else
            /* Foxconn added end pling 06/15/2010 */
            if (!nvram_match("wps_randomssid", "") ||
                    !nvram_match("wps_randomkey", ""))
            {
                /* Foxconn add start, Tony W.Y. Wang, 11/30/2009 */
                /* WiFi TKIP changes for WNDR3400*/
                /*
                   When external registrar configures our router as WPA-PSK [TKIP], security, 
                   we auto change the wireless mode to Up to 54Mbps. This should only apply to
                   router when router is in "WPS Unconfigured" state.
                   */
                nvram_set("wla_mode",  "g and b");

                /* Disable 11n support, copied from bcm_wlan_util.c */
                acosNvramConfig_set("wl_nmode", "0");
                acosNvramConfig_set("wl0_nmode", "0");

                acosNvramConfig_set("wl_gmode", "1");
                acosNvramConfig_set("wl0_gmode", "1");

                /* Set bandwidth to 20MHz */
#if ( !(defined BCM4718) && !(defined BCM4716) && !(defined R6300v2) && !defined(R6250) && !defined(R6200v2) && !defined(R7000) && !defined(R8000))
                acosNvramConfig_set("wl_nbw", "20");
                acosNvramConfig_set("wl0_nbw", "20");
#endif

                acosNvramConfig_set("wl_nbw_cap", "0");
                acosNvramConfig_set("wl0_nbw_cap", "0");

                /* Disable extension channel */
                acosNvramConfig_set("wl_nctrlsb", "none");
                acosNvramConfig_set("wl0_nctrlsb", "none");

                /* Now set the security */
                nvram_set("wla_secu_type", "WPA-PSK");
                nvram_set("wla_temp_secu_type", "WPA-PSK");

                nvram_set("wl0_akm", "psk ");
                nvram_set("wl0_crypto", "tkip");

                /*
                   nvram_set("wla_secu_type", "WPA-AUTO-PSK");
                   nvram_set("wla_temp_secu_type", "WPA-AUTO-PSK");

                   nvram_set("wl0_akm", "psk psk2 ");
                   nvram_set("wl0_crypto", "tkip+aes");
                   */
                /* Foxconn add end, Tony W.Y. Wang, 11/30/2009 */
                nvram_set("wps_mixedmode", "1");
                //nvram_set("wps_randomssid", "");
                //nvram_set("wps_randomkey", "");
                config_flag = 1;
                /* Since we changed to mixed mode, 
                 * so we need to disable WDS if it is already enabled
                 */
                if (nvram_match("wla_wds_enable", "1"))
                {
                    nvram_set("wla_wds_enable",  "0");
                    nvram_set("wl0_wds", "");
                    nvram_set("wl0_mode", "ap");
                }
            }
    }
    else if ( strncmp(nvram_safe_get("wl0_wep"), "enabled", 7) == 0 )
    {
        int key_len=0;
        if ( strncmp(nvram_safe_get("wl0_auth"), "1", 1) == 0 ) /*shared mode*/
        {
            nvram_set("wla_auth_type", "sharedkey");
            nvram_set("wla_temp_auth_type", "sharedkey");
        }
        else
        {
            nvram_set("wla_auth_type", "opensystem");
            nvram_set("wla_temp_auth_type", "opensystem");
        }

        nvram_set("wla_secu_type", "WEP");
        nvram_set("wla_temp_secu_type", "WEP");
        nvram_set("wla_defaKey", "0");
        nvram_set("wla_temp_defaKey", "0");
        /* Foxconn add start by aspen Bai, 02/24/2009 */
        /*
           nvram_set("wla_key1", nvram_safe_get("wl_key1"));
           nvram_set("wla_temp_key1", nvram_safe_get("wl_key1"));

           printf("wla_wep_length: %d\n", strlen(nvram_safe_get("wl_key1")));

           key_len = atoi(nvram_safe_get("wl_key1"));
           */
        nvram_set("wla_key1", nvram_safe_get("wl0_key1"));
        nvram_set("wla_temp_key1", nvram_safe_get("wl0_key1"));

        printf("wla_wep_length: %d\n", strlen(nvram_safe_get("wl0_key1")));

        key_len = strlen(nvram_safe_get("wl0_key1"));
        /* Foxconn add end by aspen Bai, 02/24/2009 */
        if (key_len==5 || key_len==10)
        {
            nvram_set("wla_wep_length", "1");
        }
        else
        {
            nvram_set("wla_wep_length", "2");
        }
        /* Foxconn add start by aspen Bai, 02/24/2009 */
        if (key_len==5 || key_len==13)
        {
            char HexKeyArray[32];
            char key[32], tmp[32];
            int i;

            strcpy(key, nvram_safe_get("wl0_key1"));
            memset(HexKeyArray, 0, sizeof(HexKeyArray));
            for (i=0; i<key_len; i++)
            {
                sprintf(tmp, "%02X", (unsigned char)key[i]);
                strcat(HexKeyArray, tmp);
            }
            printf("ASCII WEP key (%s) convert -> HEX WEP key (%s)\n", key, HexKeyArray);

            nvram_set("wla_key1", HexKeyArray);
            nvram_set("wla_temp_key1", HexKeyArray);
        }
        /* Foxconn add end by aspen Bai, 02/24/2009 */
    }
    else
    {
        nvram_set("wla_secu_type", "None");
        nvram_set("wla_temp_secu_type", "None");
        nvram_set("wla_passphrase", "");
    }
    /* Foxconn add start, Tony W.Y. Wang, 11/23/2009 */
    nvram_set("wlg_ssid", nvram_safe_get("wl1_ssid"));
    nvram_set("wlg_temp_ssid", nvram_safe_get("wl1_ssid"));

    if ( strncmp(nvram_safe_get("wl1_akm"), "psk psk2", 7) == 0 )
    {
        nvram_set("wlg_secu_type", "WPA-AUTO-PSK");
        nvram_set("wlg_temp_secu_type", "WPA-AUTO-PSK");
        nvram_set("wlg_passphrase", nvram_safe_get("wl1_wpa_psk"));

        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */

        if (!nvram_match("wps_randomssid", "") ||
                !nvram_match("wps_randomkey", ""))
        {
            nvram_set("wlg_secu_type", "WPA-AUTO-PSK");
            nvram_set("wlg_temp_secu_type", "WPA-AUTO-PSK");

            nvram_set("wl1_akm", "psk psk2 ");
            nvram_set("wl1_crypto", "tkip+aes");

            nvram_set("wps_mixedmode", "2");
            //nvram_set("wps_randomssid", "");
            //nvram_set("wps_randomkey", "");
            config_flag = 1;
            /* Since we changed to mixed mode, 
             * so we need to disable WDS if it is already enabled
             */
            if (nvram_match("wlg_wds_enable", "1"))
            {
                nvram_set("wlg_wds_enable",  "0");
                nvram_set("wl1_wds", "");
                nvram_set("wl1_mode", "ap");
            }
        }
        else
        {
            /* Foxconn added start pling 02/25/2007 */
            /* Disable WDS if it is already enabled */
            if (nvram_match("wlg_wds_enable", "1"))
            {
                nvram_set("wlg_wds_enable",  "0");
                nvram_set("wl1_wds", "");
                nvram_set("wl1_mode", "ap");
            }
            /* Foxconn added end pling 02/25/2007 */
        }
    }
    else if ( strncmp(nvram_safe_get("wl1_akm"), "psk2", 4) == 0 )
    {
        nvram_set("wlg_secu_type", "WPA2-PSK");
        nvram_set("wlg_temp_secu_type", "WPA2-PSK");
        nvram_set("wlg_passphrase", nvram_safe_get("wl1_wpa_psk"));


        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */

        /* Foxconn added start pling 06/15/2010 */
        if (nvram_match("wl1_crypto", "tkip"))
        {
            /* DTM fix: 
             * Registrar may set to WPA2-PSK TKIP mode.
             * In this case, don't try to modify the
             * security type.
             */
            nvram_unset("wps_mixedmode");
        }
        else
            /* Foxconn added end pling 06/15/2010 */
            if (!nvram_match("wps_randomssid", "") ||
                    !nvram_match("wps_randomkey", ""))
            {
                nvram_set("wlg_secu_type", "WPA2-PSK");
                nvram_set("wlg_temp_secu_type", "WPA2-PSK");

                nvram_set("wl1_akm", "psk2");
                nvram_set("wl1_crypto", "aes");

                nvram_set("wps_mixedmode", "2");
                //nvram_set("wps_randomssid", "");
                //nvram_set("wps_randomkey", "");
                config_flag = 1;
                /* Since we changed to mixed mode, 
                 * so we need to disable WDS if it is already enabled
                 */
                if (nvram_match("wlg_wds_enable", "1"))
                {
                    nvram_set("wlg_wds_enable",  "0");
                    nvram_set("wl1_wds", "");
                    nvram_set("wl1_mode", "ap");
                }
            }
            else
            {
                /* Foxconn added start pling 02/25/2007 */
                /* Disable WDS if it is already enabled */
                if (nvram_match("wlg_wds_enable", "1"))
                {
                    nvram_set("wlg_wds_enable",  "0");
                    nvram_set("wl1_wds", "");
                    nvram_set("wl1_mode", "ap");
                }
                /* Foxconn added end pling 02/25/2007 */
            }
    }
    else if ( strncmp(nvram_safe_get("wl1_akm"), "psk", 3) == 0 )
    {
        nvram_set("wlg_secu_type", "WPA-PSK");
        nvram_set("wlg_temp_secu_type", "WPA-PSK");
        nvram_set("wlg_passphrase", nvram_safe_get("wl1_wpa_psk"));

        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */

        /* Foxconn added start pling 06/15/2010 */
        if (nvram_match("wl1_crypto", "aes"))
        {
            /* DTM fix: 
             * Registrar may set to WPA-PSK AES mode.
             * In this case, don't try to modify the
             * security type.
             */
            nvram_unset("wps_mixedmode");
        }
        else
            /* Foxconn added end pling 06/15/2010 */
            if (!nvram_match("wps_randomssid", "") ||
                    !nvram_match("wps_randomkey", ""))
            {
                /* Foxconn add start, Tony W.Y. Wang, 11/30/2009 */
                /* WiFi TKIP changes for WNDR3400*/
                /*
                   When external registrar configures our router as WPA-PSK [TKIP], security, 
                   we auto change the wireless mode to Up to 54Mbps. This should only apply to
                   router when router is in "WPS Unconfigured" state.
                   */
                nvram_set("wlg_mode",  "g and b");

                /* Disable 11n support, copied from bcm_wlan_util.c */
                acosNvramConfig_set("wl1_nmode", "0");

                acosNvramConfig_set("wl1_gmode", "1");

                /* Set bandwidth to 20MHz */
#if ( !(defined BCM4718) && !(defined BCM4716) && !(defined R6300v2) && !defined(R6250) && !defined(R6200v2) && !defined(R7000) && !defined(R8000)) 
                acosNvramConfig_set("wl1_nbw", "20");
#endif

                acosNvramConfig_set("wl1_nbw_cap", "0");

                /* Disable extension channel */
                acosNvramConfig_set("wl1_nctrlsb", "none");

                /* Now set the security */
                nvram_set("wlg_secu_type", "WPA-PSK");
                nvram_set("wlg_temp_secu_type", "WPA-PSK");

                nvram_set("wl1_akm", "psk ");
                nvram_set("wl1_crypto", "tkip");
                /*
                   nvram_set("wlg_secu_type", "WPA-AUTO-PSK");
                   nvram_set("wlg_temp_secu_type", "WPA-AUTO-PSK");

                   nvram_set("wl1_akm", "psk psk2 ");
                   nvram_set("wl1_crypto", "tkip+aes");
                   */
                /* Foxconn add end, Tony W.Y. Wang, 11/30/2009 */
                nvram_set("wps_mixedmode", "1");
                //nvram_set("wps_randomssid", "");
                //nvram_set("wps_randomkey", "");
                config_flag = 1;
                /* Since we changed to mixed mode, 
                 * so we need to disable WDS if it is already enabled
                 */
                if (nvram_match("wlg_wds_enable", "1"))
                {
                    nvram_set("wlg_wds_enable",  "0");
                    nvram_set("wl1_wds", "");
                    nvram_set("wl1_mode", "ap");
                }
            }
    }
    else if ( strncmp(nvram_safe_get("wl1_wep"), "enabled", 7) == 0 )
    {
        int key_len=0;
        if ( strncmp(nvram_safe_get("wl1_auth"), "1", 1) == 0 ) /*shared mode*/
        {
            nvram_set("wlg_auth_type", "sharedkey");
            nvram_set("wlg_temp_auth_type", "sharedkey");
        }
        else
        {
            nvram_set("wlg_auth_type", "opensystem");
            nvram_set("wlg_temp_auth_type", "opensystem");
        }

        nvram_set("wlg_secu_type", "WEP");
        nvram_set("wlg_temp_secu_type", "WEP");
        nvram_set("wlg_defaKey", "0");
        nvram_set("wlg_temp_defaKey", "0");
        /* Foxconn add start by aspen Bai, 02/24/2009 */
        /*
           nvram_set("wla_key1", nvram_safe_get("wl_key1"));
           nvram_set("wla_temp_key1", nvram_safe_get("wl_key1"));

           printf("wla_wep_length: %d\n", strlen(nvram_safe_get("wl_key1")));

           key_len = atoi(nvram_safe_get("wl_key1"));
           */
        nvram_set("wlg_key1", nvram_safe_get("wl1_key1"));
        nvram_set("wlg_temp_key1", nvram_safe_get("wl1_key1"));

        printf("wlg_wep_length: %d\n", strlen(nvram_safe_get("wl1_key1")));

        key_len = strlen(nvram_safe_get("wl1_key1"));
        /* Foxconn add end by aspen Bai, 02/24/2009 */
        if (key_len==5 || key_len==10)
        {
            nvram_set("wlg_wep_length", "1");
        }
        else
        {
            nvram_set("wlg_wep_length", "2");
        }
        /* Foxconn add start by aspen Bai, 02/24/2009 */
        if (key_len==5 || key_len==13)
        {
            char HexKeyArray[32];
            char key[32], tmp[32];
            int i;

            strcpy(key, nvram_safe_get("wl1_key1"));
            memset(HexKeyArray, 0, sizeof(HexKeyArray));
            for (i=0; i<key_len; i++)
            {
                sprintf(tmp, "%02X", (unsigned char)key[i]);
                strcat(HexKeyArray, tmp);
            }
            printf("ASCII WEP key (%s) convert -> HEX WEP key (%s)\n", key, HexKeyArray);

            nvram_set("wlg_key1", HexKeyArray);
            nvram_set("wlg_temp_key1", HexKeyArray);
        }
        /* Foxconn add end by aspen Bai, 02/24/2009 */
    }
    else
    {
        nvram_set("wlg_secu_type", "None");
        nvram_set("wlg_temp_secu_type", "None");
        nvram_set("wlg_passphrase", "");
    }

#if (defined R8000)

    nvram_set("wlh_ssid", nvram_safe_get("wl2_ssid"));
    nvram_set("wlh_temp_ssid", nvram_safe_get("wl2_ssid"));

    if ( strncmp(nvram_safe_get("wl2_akm"), "psk psk2", 7) == 0 )
    {
        nvram_set("wlh_secu_type", "WPA-AUTO-PSK");
        nvram_set("wlh_temp_secu_type", "WPA-AUTO-PSK");
        nvram_set("wlh_passphrase", nvram_safe_get("wl2_wpa_psk"));

        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */

        if (!nvram_match("wps_randomssid", "") ||
                !nvram_match("wps_randomkey", ""))
        {
            nvram_set("wlh_secu_type", "WPA-AUTO-PSK");
            nvram_set("wlh_temp_secu_type", "WPA-AUTO-PSK");

            nvram_set("wl2_akm", "psk psk2 ");
            nvram_set("wl2_crypto", "tkip+aes");

            nvram_set("wps_mixedmode", "2");
            //nvram_set("wps_randomssid", "");
            //nvram_set("wps_randomkey", "");
            config_flag = 1;
            /* Since we changed to mixed mode, 
             * so we need to disable WDS if it is already enabled
             */
            if (nvram_match("wlh_wds_enable", "1"))
            {
                nvram_set("wlh_wds_enable",  "0");
                nvram_set("wl2_wds", "");
                nvram_set("wl2_mode", "ap");
            }
        }
        else
        {
            /* Foxconn added start pling 02/25/2007 */
            /* Disable WDS if it is already enabled */
            if (nvram_match("wlh_wds_enable", "1"))
            {
                nvram_set("wlh_wds_enable",  "0");
                nvram_set("wl2_wds", "");
                nvram_set("wl2_mode", "ap");
            }
            /* Foxconn added end pling 02/25/2007 */
        }
    }
    else if ( strncmp(nvram_safe_get("wl2_akm"), "psk2", 4) == 0 )
    {
        nvram_set("wlh_secu_type", "WPA2-PSK");
        nvram_set("wlh_temp_secu_type", "WPA2-PSK");
        nvram_set("wlh_passphrase", nvram_safe_get("wl2_wpa_psk"));


        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */

        /* Foxconn added start pling 06/15/2010 */
        if (nvram_match("wl2_crypto", "tkip"))
        {
            /* DTM fix: 
             * Registrar may set to WPA2-PSK TKIP mode.
             * In this case, don't try to modify the
             * security type.
             */
            nvram_unset("wps_mixedmode");
        }
        else
            /* Foxconn added end pling 06/15/2010 */
            if (!nvram_match("wps_randomssid", "") ||
                    !nvram_match("wps_randomkey", ""))
            {
                nvram_set("wlh_secu_type", "WPA2-PSK");
                nvram_set("wlh_temp_secu_type", "WPA2-PSK");

                nvram_set("wl2_akm", "psk2");
                nvram_set("wl2_crypto", "aes");

                nvram_set("wps_mixedmode", "2");
                //nvram_set("wps_randomssid", "");
                //nvram_set("wps_randomkey", "");
                config_flag = 1;
                /* Since we changed to mixed mode, 
                 * so we need to disable WDS if it is already enabled
                 */
                if (nvram_match("wlh_wds_enable", "1"))
                {
                    nvram_set("wlh_wds_enable",  "0");
                    nvram_set("wl2_wds", "");
                    nvram_set("wl2_mode", "ap");
                }
            }
            else
            {
                /* Foxconn added start pling 02/25/2007 */
                /* Disable WDS if it is already enabled */
                if (nvram_match("wlh_wds_enable", "1"))
                {
                    nvram_set("wlh_wds_enable",  "0");
                    nvram_set("wl2_wds", "");
                    nvram_set("wl2_mode", "ap");
                }
                /* Foxconn added end pling 02/25/2007 */
            }
    }
    else if ( strncmp(nvram_safe_get("wl2_akm"), "psk", 3) == 0 )
    {
        nvram_set("wlh_secu_type", "WPA-PSK");
        nvram_set("wlh_temp_secu_type", "WPA-PSK");
        nvram_set("wlh_passphrase", nvram_safe_get("wl2_wpa_psk"));

        /* If router changes from 'unconfigured' to 'configured' state by
         * adding a WPS client, the wsc_randomssid and wsc_randomkey will
         * be set. In this case, router should use mixedmode security.
         */

        /* Foxconn added start pling 06/15/2010 */
        if (nvram_match("wl2_crypto", "aes"))
        {
            /* DTM fix: 
             * Registrar may set to WPA-PSK AES mode.
             * In this case, don't try to modify the
             * security type.
             */
            nvram_unset("wps_mixedmode");
        }
        else
            /* Foxconn added end pling 06/15/2010 */
            if (!nvram_match("wps_randomssid", "") ||
                    !nvram_match("wps_randomkey", ""))
            {
                /* Foxconn add start, Tony W.Y. Wang, 11/30/2009 */
                /* WiFi TKIP changes for WNDR3400*/
                /*
                   When external registrar configures our router as WPA-PSK [TKIP], security, 
                   we auto change the wireless mode to Up to 54Mbps. This should only apply to
                   router when router is in "WPS Unconfigured" state.
                   */
                nvram_set("wlh_mode",  "g and b");

                /* Disable 11n support, copied from bcm_wlan_util.c */
                acosNvramConfig_set("wl2_nmode", "0");

                acosNvramConfig_set("wl2_gmode", "1");

                /* Set bandwidth to 20MHz */
                acosNvramConfig_set("wl2_nbw", "20");

                acosNvramConfig_set("wl2_nbw_cap", "0");

                /* Disable extension channel */
                acosNvramConfig_set("wl2_nctrlsb", "none");

                /* Now set the security */
                nvram_set("wlh_secu_type", "WPA-PSK");
                nvram_set("wlh_temp_secu_type", "WPA-PSK");

                nvram_set("wlh_akm", "psk ");
                nvram_set("wlh_crypto", "tkip");

                nvram_set("wps_mixedmode", "1");
                //nvram_set("wps_randomssid", "");
                //nvram_set("wps_randomkey", "");
                config_flag = 1;
                /* Since we changed to mixed mode, 
                 * so we need to disable WDS if it is already enabled
                 */
                if (nvram_match("wlg_wds_enable", "1"))
                {
                    nvram_set("wlg_wds_enable",  "0");
                    nvram_set("wl2_wds", "");
                    nvram_set("wl2_mode", "ap");
                }
            }
    }
    else if ( strncmp(nvram_safe_get("wl2_wep"), "enabled", 7) == 0 )
    {
        int key_len=0;
        if ( strncmp(nvram_safe_get("wl2_auth"), "1", 1) == 0 ) /*shared mode*/
        {
            nvram_set("wlh_auth_type", "sharedkey");
            nvram_set("wlh_temp_auth_type", "sharedkey");
        }
        else
        {
            nvram_set("wlh_auth_type", "opensystem");
            nvram_set("wlh_temp_auth_type", "opensystem");
        }

        nvram_set("wlh_secu_type", "WEP");
        nvram_set("wlh_temp_secu_type", "WEP");
        nvram_set("wlh_defaKey", "0");
        nvram_set("wlh_temp_defaKey", "0");
        /* Foxconn add start by aspen Bai, 02/24/2009 */
        /*
           nvram_set("wla_key1", nvram_safe_get("wl_key1"));
           nvram_set("wla_temp_key1", nvram_safe_get("wl_key1"));

           printf("wla_wep_length: %d\n", strlen(nvram_safe_get("wl_key1")));

           key_len = atoi(nvram_safe_get("wl_key1"));
           */
        nvram_set("wlh_key1", nvram_safe_get("wl2_key1"));
        nvram_set("wlh_temp_key1", nvram_safe_get("wl2_key1"));

        printf("wlh_wep_length: %d\n", strlen(nvram_safe_get("wl2_key1")));

        key_len = strlen(nvram_safe_get("wl2_key1"));
        /* Foxconn add end by aspen Bai, 02/24/2009 */
        if (key_len==5 || key_len==10)
        {
            nvram_set("wlh_wep_length", "1");
        }
        else
        {
            nvram_set("wlh_wep_length", "2");
        }
        /* Foxconn add start by aspen Bai, 02/24/2009 */
        if (key_len==5 || key_len==13)
        {
            char HexKeyArray[32];
            char key[32], tmp[32];
            int i;

            strcpy(key, nvram_safe_get("wl2_key1"));
            memset(HexKeyArray, 0, sizeof(HexKeyArray));
            for (i=0; i<key_len; i++)
            {
                sprintf(tmp, "%02X", (unsigned char)key[i]);
                strcat(HexKeyArray, tmp);
            }
            printf("ASCII WEP key (%s) convert -> HEX WEP key (%s)\n", key, HexKeyArray);

            nvram_set("wlh_key1", HexKeyArray);
            nvram_set("wlh_temp_key1", HexKeyArray);
        }
        /* Foxconn add end by aspen Bai, 02/24/2009 */
    }
    else
    {
        nvram_set("wlh_secu_type", "None");
        nvram_set("wlh_temp_secu_type", "None");
        nvram_set("wlh_passphrase", "");
    }
#endif

    if (config_flag == 1)
    {
        //nvram_set("wps_randomssid", "");
        //nvram_set("wps_randomkey", "");
        nvram_set("wl0_wps_config_state", "1");
        nvram_set("wl1_wps_config_state", "1");
#if defined(R8000)
        nvram_set("wl2_wps_config_state", "1");
#endif        
    }
    /* Foxconn add end, Tony W.Y. Wang, 11/23/2009 */
    nvram_set("allow_registrar_config", "0");  /* Foxconn added pling, 05/16/2007 */

    /* Foxconn added start pling 02/25/2008 */
    /* 'wl_unit' is changed to "0.-1" after Vista configure router (using Borg DTM1.3 patch).
     * This will make WPS fail to work on the correct interface.
     * Set it back to "0" if it is not.
     */
    if (!nvram_match("wl_unit", "0"))
        nvram_set("wl_unit", "0");
    /* Foxconn added end pling 02/25/2008 */
}
/* Foxconn added end by EricHuang, 12/13/2006 */

/* foxconn added start wklin, 11/02/2006 */
static void save_wlan_time(void)
{
    struct sysinfo info;
    char command[128];
    sysinfo(&info);
    sprintf(command, "echo %lu > /tmp/wlan_time", info.uptime);
    system(command);
    return;
}
/* foxconn added end, wklin, 11/02/2006 */

#ifdef MFP
int disable_mfp()
{
    /*foxconn Han edited for GUI pmf enable/disable support once enable_pmf==1 then we should not overwrite mfp value*/
    if(nvram_match("enable_pmf","1"))
        return 0;

    /* Foxconn Bob added start 07/24/2015, force disable PMF to fix IOT issue with Nexus 5 */
    nvram_set("wl0_mfp", "0");
    nvram_set("wl1_mfp", "0");
    nvram_set("wl2_mfp", "0");
    /* Foxconn Bob added end 07/24/2015, force disable PMF to fix IOT issue with Nexus 5 */
}
#endif /*MFP*/

/*foxconn Han edited start, 02/23/2016*/
#ifdef PORT_TRUNKING_SUPPORT
extern int check_lacp_vlan_conflict(unsigned int intf, int gui);
/* move to ap/acos/share/lan_util.c
int check_lacp_vlan_conflict(unsigned int intf)
{
    unsigned int flag;
    printf("%s(%d) intf=0x%X\n",__func__,__LINE__,intf);

    flag = intf & (IPTV_LAN1|IPTV_LAN2);

    if(flag != 0 && flag != (IPTV_LAN1|IPTV_LAN2))
    {
        nvram_set("lacp_vlan_conflict","1");
        return 1;
    }
    
    return 0;
}*/
#endif /*PORT_TRUNKING_SUPPORT*/
/*foxconn Han edited end, 02/23/2016*/

/* foxconn added start, zacker, 01/13/2012, @iptv_igmp */
#ifdef CONFIG_RUSSIA_IPTV
static int config_iptv_params(void)
{
#ifdef VLAN_SUPPORT
    unsigned int enabled_vlan_ports = 0x00;
    unsigned int iptv_bridge_intf = 0x00;
#endif
    char vlan1_ports[16] = "";
    char vlan_iptv_ports[16] = "";
    /*added by dennis start,05/04/2012,for guest network reconnect issue*/
    char br0_ifnames[64]="";
    char if_name[16]="";
    char wl_param[16]="";
    char command[128]="";
    int i = 0;
    /*added by dennis end,05/04/2012,for guest network reconnect issue*/

/*Foxconn add start, edward zhang, 2013/07/03*/
#ifdef VLAN_SUPPORT
    char br_ifname[16] = "";
    char br_ifnames[64] = "";
    char clean_vlan[16] = "";
    char clean_vlan_hw[16] = "";


	/*clean up the nvram ,to let the new config work*/
	
    if (nvram_match ("enable_vlan", "enable"))
    {
        for(i=1; i<7; i++)
        {
            sprintf(br_ifname,"lan%d_ifname",i);
            sprintf(br_ifnames,"lan%d_ifnames",i);
            nvram_set(br_ifnames, "");
            nvram_set(br_ifname, "");
        }
        for(i=1; i < 4094; i++)
        {
            sprintf(clean_vlan,"vlan%dports",i);
            sprintf(clean_vlan_hw,"vlan%dhwname",i);
            if( i == 1 || i == 2)
            {
              nvram_set(clean_vlan,"");
              nvram_set(clean_vlan_hw,"");
            }
            else
            {
                nvram_unset(clean_vlan);
                nvram_unset(clean_vlan_hw);
            }
            #ifdef CONFIG_2ND_SWITCH
            /*foxconn Han edited 05/27/2015, for external switch nvram cleanup*/
            sprintf(clean_vlan,"evlan%dports",i);
            if( i == 1 || i == 2)
                nvram_set(clean_vlan,"");
            else
                nvram_unset(clean_vlan);
            #endif /*CONFIG_2ND_SWITCH*/
        }
    }
    else
    {
        for(i=3; i < 4094; i++)
        {
            sprintf(clean_vlan,"vlan%dports",i);
            sprintf(clean_vlan_hw,"vlan%dhwname",i);
            nvram_unset(clean_vlan);
            nvram_unset(clean_vlan_hw);
            #ifdef CONFIG_2ND_SWITCH
            /*foxconn Han edited 05/27/2015, for external switch nvram cleanup*/
            sprintf(clean_vlan,"evlan%dports",i);
            nvram_unset(clean_vlan);
            #endif /*CONFIG_2ND_SWITCH*/
        }
    }
    /*foxconn Han edited start, 02/23/2016*/
#ifdef  PORT_TRUNKING_SUPPORT
    nvram_set("lacp_vlan_conflict","0");
#endif  /*PORT_TRUNKING_SUPPORT*/
    /*foxconn Han edited end, 02/23/2016*/

    if (!nvram_match("enable_vlan", "enable") && !nvram_match(NVRAM_IPTV_ENABLED, "1") )
        return 0;
#endif
/*Foxconn add end, edward zhang, 2013/07/03*/

    if (nvram_match(NVRAM_IPTV_ENABLED, "1"))
    {
        char iptv_intf[32];

        strcpy(iptv_intf, nvram_safe_get(NVRAM_IPTV_INTF));
        sscanf(iptv_intf, "0x%04X", &iptv_bridge_intf);
    }


    /*foxconn Han edited start, 02/23/2016*/
#ifdef PORT_TRUNKING_SUPPORT
    check_lacp_vlan_conflict(iptv_bridge_intf ,0);
    check_lacp_vlan_conflict(~iptv_bridge_intf ,0);
#endif /*PORT_TRUNKING_SUPPORT*/
    /*foxconn Han edited end, 02/23/2016*/


    /* Foxconn modified start pling 04/03/2012 */
    /* Swap LAN1 ~ LAN4 due to reverse labeling */

    if (iptv_bridge_intf & IPTV_LAN1)
        strcat(vlan_iptv_ports, "1 ");
    else
        strcat(vlan1_ports, "1 ");

    if (iptv_bridge_intf & IPTV_LAN2)   /* Foxconn modified pling 02/09/2012, fix a typo */
        strcat(vlan_iptv_ports, "2 ");
    else
        strcat(vlan1_ports, "2 ");

    if (iptv_bridge_intf & IPTV_LAN3)
        strcat(vlan_iptv_ports, "3 ");
    else
        strcat(vlan1_ports, "3 ");

/*foxconn Han edited start, 2015/05/18*/
/*
Switch layout for R8500:
             ________________________________
Housing     | W | 1 | 2 | 3 |    | 4 | 5 | 6 |
             --------------------------------
             ___________________
Switch 1    | 0 | 1 | 2 | 3 | 4 |
             -------------------
                              ||
                              \/
                          ___________________
             switch 2    | x | 1 | 2 | 3 | 4 |
                          -------------------

Port 4 5 6 is on external switch
*/
#ifdef CONFIG_2ND_SWITCH
    char evlan1_ports[16] = "";
    char evlan_iptv_ports[16] = "";
    
    if(isTriBand())
    {
	    //printf("%s %d iptv_bridge_intf=0x%X\n",__func__,__LINE__,iptv_bridge_intf);
        if(iptv_bridge_intf & (IPTV_LAN4 | IPTV_LAN5 | IPTV_LAN6)) /*0x304*/
        {
            strcat(vlan_iptv_ports, "4t ");
            strcat(evlan_iptv_ports, "1t ");
            if((iptv_bridge_intf & (IPTV_LAN4 | IPTV_LAN5 | IPTV_LAN6)) 
                != (IPTV_LAN4 | IPTV_LAN5 | IPTV_LAN6) ) /*0x304*/
            {
                strcat(vlan1_ports, "4t ");
                strcat(evlan1_ports, "1t ");
            }
        }
        else
        {
            strcat(vlan1_ports, "4 ");
            //strcat(evlan_iptv_ports, "1 ");
            strcat(evlan1_ports, "1 ");
        }

        if (iptv_bridge_intf & IPTV_LAN4)
            strcat(evlan_iptv_ports, "2 ");
        else
            strcat(evlan1_ports, "2 ");

        if (iptv_bridge_intf & IPTV_LAN5)
            strcat(evlan_iptv_ports, "3 ");
        else
            strcat(evlan1_ports, "3 ");

        if (iptv_bridge_intf & IPTV_LAN6)
            strcat(evlan_iptv_ports, "4 ");
        else
            strcat(evlan1_ports, "4 ");
        
        //strcat(evlan1_ports, "5u ");
		//strcat(evlan1_ports, "5 ");
    }
    else
#endif /*CONFIG_2ND_SWITCH*/
/*foxconn Han edited end, 2015/05/18*/
    {   /* !isTriBand() */
        if (iptv_bridge_intf & IPTV_LAN4)
            strcat(vlan_iptv_ports, "4 ");
        else
            strcat(vlan1_ports, "4 ");

    } /*end of !isTriBand()*/


    #ifdef __CONFIG_GMAC3__
    if(nvram_match("gmac3_enable", "1"))
        strcat(vlan1_ports, "5 7 8*");
    else
        strcat(vlan1_ports, "5*");
    #else
    strcat(vlan1_ports, "5*");
    #endif    
    /*Foxconn add start, edward zhang, 2013/07/03*/
    #ifdef VLAN_SUPPORT
    char lan_interface[16]="";
    char lan_hwname[16]="";
	#ifdef CONFIG_2ND_SWITCH
	char ext_lan_interface[16]="";
	#endif /*CONFIG_2ND_SWITCH*/
    if (nvram_match ("enable_vlan", "enable"))
    {
        sprintf(lan_interface,"vlan%sports",nvram_safe_get("vlan_lan_id"));
        nvram_set(lan_interface,vlan1_ports);
        sprintf(lan_hwname,"vlan%shwname",nvram_safe_get("vlan_lan_id"));
        nvram_set(lan_hwname,"et0");
        /*foxconn Han edited 05/27/2015, for external switch*/
		#ifdef CONFIG_2ND_SWITCH
		if(isTriBand())
		{
			sprintf(ext_lan_interface,"evlan%sports",nvram_safe_get("vlan_lan_id"));
            //printf("%s %d ext_lan_interface=%s, evlan1_ports=%s\n",__func__,__LINE__,ext_lan_interface,evlan1_ports);
			nvram_set(ext_lan_interface, evlan1_ports);
		}
		#endif /*CONFIG_2ND_SWITCH*/
    }
    else
    #endif
	{
		/*Foxconn add end, edward zhang, 2013/07/03*/
		nvram_set("vlan1ports", vlan1_ports);
		#ifdef CONFIG_2ND_SWITCH
		if(isTriBand())
			nvram_set("evlan1ports", evlan1_ports);
        //printf("%s %d evlan1_ports=%s\n",__func__,__LINE__,evlan1_ports);
		#endif /*CONFIG_2ND_SWITCH*/
	}

    /* build vlan3 for IGMP snooping on IPTV ports */
    /*Foxconn add start, edward zhang, 2013/07/03*/
#ifdef VLAN_SUPPORT
    if (nvram_match ("enable_vlan", "enable"))
        ;/*do nothing*/
    else
#endif
    /*Foxconn add end, edward zhang, 2013/07/03*/
    {
        if (strlen(vlan_iptv_ports))
        {
            strcat(vlan_iptv_ports, "5");
            nvram_set("vlan3ports", vlan_iptv_ports);
            nvram_set("vlan3hwname", nvram_safe_get("vlan2hwname"));

        }
        else
        {
            nvram_unset("vlan3ports");
            nvram_unset("vlan3hwname");
        }
		#ifdef CONFIG_2ND_SWITCH
        /*foxconn Han edited 05/27/2015, for external switch*/
		if(strlen(evlan_iptv_ports))
		{
			if(isTriBand())
			{
				printf("%s %d evlan3_ports=%s\n",__func__,__LINE__,evlan_iptv_ports);
				nvram_set("evlan3ports",evlan_iptv_ports);
			}
		}
		else
		{
			nvram_unset("evlan3ports");
		}
		#endif /*CONFIG_2ND_SWITCH*/		
    }

/*Foxconn add start, edward zhang, 2013/07/03*/
#ifdef VLAN_SUPPORT
    
    if (nvram_match ("enable_vlan", "enable"))
    {
        char vlan_ifname[16] = "";
        char vlan_ifname_ports[16] = "";
        char vlan_ports[16]  = "";
        char vlan_prio[16] = "";
        char vlan_hwname[16] = "";
        char wan_vlan_ifname[16] = "";
        char lan_vlan_hwname[16] = "";
        vlan_rule vlan[C_MAX_VLAN_RULE];
        int numVlanRule = getVlanRule(vlan);
		unsigned int vlan_bridge_intf = 0x00;
        char lan_vlan_ports[16] = "";
        char lan_ports[16] = "";
        int lan_vlan_port = 4;
        int lan_vlan_br = 1;
        char lan_vlan_ifname[16] = "";
        char lan_vlan_ifnames[128] = "";
        char lan_ifnames[128] = "";
        char lan_ifname[16] = "";
        int internet_vlan_id;
		#ifdef CONFIG_2ND_SWITCH
		char evlan_ifname_ports[16] = "";
		char evlan_ports[16]="";
		char elan_ports[16]="";
		#endif /*CONFIG_2ND_SWITCH*/
		
		
        /* always set emf_enable to 0 when vlan is enable*/
        nvram_set("emf_enable", "0");

        cprintf("rule_num:%d \n",numVlanRule);
        sprintf(lan_ifnames,"%s ",nvram_safe_get("lan_interface"));
        for(i=0;i<numVlanRule;i++)
        {
            memset(lan_vlan_ifnames,0,sizeof(lan_vlan_ifnames));
            memset(vlan_ports,0,sizeof(vlan_ports));
            if(!strcmp(vlan[i].enable_rule,"0"))
                continue;
            sprintf(vlan_ifname,"vlan%s ",vlan[i].vlan_id);
            sprintf(wan_vlan_ifname,"vlan%s",vlan[i].vlan_id);
            sprintf(vlan_ifname_ports,"vlan%sports",vlan[i].vlan_id);
            sprintf(vlan_hwname,"vlan%shwname",vlan[i].vlan_id);
            nvram_set(vlan_hwname,"et0");
            sprintf(vlan_prio,"vlan%s_prio",vlan[i].vlan_id);
            nvram_set(vlan_prio,vlan[i].vlan_prio);
			#ifdef CONFIG_2ND_SWITCH
			sprintf(evlan_ifname_ports,"evlan%sports",vlan[i].vlan_id);
			#endif /*CONFIG_2ND_SWITCH*/
            
            if(!strcmp(vlan[i].vlan_name, "Internet"))
            {
         	    nvram_set(vlan_ifname_ports,"0t 5");
                nvram_set("internet_prio",vlan[i].vlan_prio);
                nvram_set("internet_vlan",vlan[i].vlan_id);
                nvram_set("wan_ifnames", vlan_ifname);
                nvram_set("wan_ifname", wan_vlan_ifname);
                internet_vlan_id=atoi(vlan[i].vlan_id);
                continue;
            }
            
            if(internet_vlan_id==atoi(vlan[i].vlan_id))
            {
                nvram_set("wan_ifnames", "br1");
                nvram_set("wan_ifname", "br1");
            }
            
            sscanf(vlan[i].vlan_ports, "0x%04X", &vlan_bridge_intf);

            strcat(lan_vlan_ifnames, vlan_ifname);
            enabled_vlan_ports |= vlan_bridge_intf ;

            //printf("%s %d %d vlan_bridge_intf=0x%X enabled_vlan_ports=0x%X\n",__func__,__LINE__,i,vlan_bridge_intf,enabled_vlan_ports);

            /*foxconn Han edited start, 02/23/2016*/
#ifdef      PORT_TRUNKING_SUPPORT
            check_lacp_vlan_conflict(vlan_bridge_intf ,0);
#endif      /*PORT_TRUNKING_SUPPORT*/
            /*foxconn Han edited end, 02/23/2016*/

            if (vlan_bridge_intf & IPTV_LAN1)
                strcat(vlan_ports, "1 ");

            if (vlan_bridge_intf & IPTV_LAN2)  
                strcat(vlan_ports, "2 ");

            if (vlan_bridge_intf & IPTV_LAN3)
                strcat(vlan_ports, "3 ");

			#ifdef CONFIG_2ND_SWITCH
			if(isTriBand())
			{
				if (vlan_bridge_intf & (IPTV_LAN4 | IPTV_LAN5 | IPTV_LAN6)) /*0x304*/
				{
					strcat(vlan_ports, "4t ");
					strcat(evlan_ports, "1t ");
				}
				else
				{
					;
				}
				
				if (vlan_bridge_intf & IPTV_LAN4)
				{
					strcat(evlan_ports, "2 ");
				}
				if (vlan_bridge_intf & IPTV_LAN5)
				{
					strcat(evlan_ports, "3 ");
				}
				if (vlan_bridge_intf & IPTV_LAN6)
				{
					strcat(evlan_ports, "4 ");
				}
				if(strlen(evlan_ports)>0)
				{
					//strcat(evlan_ports, "5 ");
				}
				printf("%s %d evlan_ifname_ports=%s evlan_ports=%s\n",__func__,__LINE__,evlan_ifname_ports,evlan_ports);
				nvram_set(evlan_ifname_ports,evlan_ports);
			}
			else
			#endif /*CONFIG_2ND_SWITCH*/
			{
				if (vlan_bridge_intf & IPTV_LAN4)
					strcat(vlan_ports, "4 ");
            }
            strcat(vlan_ports, "0t 5");

            nvram_set(vlan_ifname_ports,vlan_ports);    /*Foxconn add, edward zhang ,set the bridge ports*/

            if (vlan_bridge_intf & IPTV_WLAN1)
                strcat(lan_vlan_ifnames, "eth1 ");

            if (vlan_bridge_intf & IPTV_WLAN2)
                strcat(lan_vlan_ifnames, "eth2 ");

			if(isTriBand())
			{
				if (vlan_bridge_intf & IPTV_WLAN3)
					strcat(lan_vlan_ifnames, "eth3 ");
			}

            if (vlan_bridge_intf & IPTV_WLAN_GUEST1)
                strcat(lan_vlan_ifnames, "wl0.1 ");

            if (vlan_bridge_intf & IPTV_WLAN_GUEST2)
                strcat(lan_vlan_ifnames, "wl1.1 ");

			if(isTriBand())
			{
				if (vlan_bridge_intf & IPTV_WLAN_GUEST3)
					strcat(lan_vlan_ifnames, "wl2.1 ");
			}

            
            sprintf(br_ifname,"lan%d_ifname",lan_vlan_br);
            sprintf(br_ifnames,"lan%d_ifnames",lan_vlan_br);
            sprintf(lan_vlan_ifname,"br%d",lan_vlan_br);
            nvram_set(br_ifname,lan_vlan_ifname);
            nvram_set(br_ifnames,lan_vlan_ifnames);
            lan_vlan_br++;
        }
        
        /*foxconn Han edited start, 02/23/2016*/
#ifdef  PORT_TRUNKING_SUPPORT
        check_lacp_vlan_conflict(~enabled_vlan_ports ,0);
#endif  /*PORT_TRUNKING_SUPPORT*/
        /*foxconn Han edited end, 02/23/2016*/

        if (!(enabled_vlan_ports & IPTV_LAN1))
            strcat(lan_ports, "1 ");

        if (!(enabled_vlan_ports & IPTV_LAN2))  
            strcat(lan_ports, "2 ");

        if (!(enabled_vlan_ports & IPTV_LAN3))
            strcat(lan_ports, "3 ");

		#ifdef CONFIG_2ND_SWITCH
		if(isTriBand())
		{
			if(enabled_vlan_ports & (IPTV_LAN4 | IPTV_LAN5 | IPTV_LAN6))
			{
				strcat(lan_ports, "4t ");
				strcat(elan_ports, "0 1t ");
				//strcat(lan_ports, "4 ");
				//strcat(elan_ports, "0 1 ");
				//strcat(elan_ports, "0 ");
				if (!(enabled_vlan_ports & IPTV_LAN4))
					strcat(elan_ports, "2 ");
				if (!(enabled_vlan_ports & IPTV_LAN5))
					strcat(elan_ports, "3 ");
				if (!(enabled_vlan_ports & IPTV_LAN6))
					strcat(elan_ports, "4 ");
			}
			else
			{
				strcat(lan_ports, "4 ");
				strcat(elan_ports, "0 1 2 3 4 ");
			}
			
			strcat(elan_ports, "5u ");
			printf("%s %d ext_lan_interface=%s,elan_ports=%s\n",__func__,__LINE__,ext_lan_interface,elan_ports);
			nvram_set(ext_lan_interface,elan_ports);
		}
		else
		#endif /*CONFIG_2ND_SWITCH*/ 
        {    
			if (!(enabled_vlan_ports & IPTV_LAN4))
				strcat(lan_ports, "4 ");
        }
		
		strcat(lan_ports, "5* ");
		nvram_set(lan_interface,lan_ports);
		
        
        if (!(enabled_vlan_ports & IPTV_WLAN1))
            strcat(lan_ifnames, "eth1 ");

        if (!(enabled_vlan_ports & IPTV_WLAN2))
            strcat(lan_ifnames, "eth2 ");

		if(isTriBand())
		{
			if (!(enabled_vlan_ports & IPTV_WLAN3))
				strcat(lan_ifnames, "eth3 ");
		}
        
        strcpy(br0_ifnames,lan_ifnames);
		#ifdef __CONFIG_IGMP_SNOOPING__
        /* always enable snooping for VLAN IPTV */
        //nvram_set("emf_enable", "1");
		#endif
		#ifdef VLAN_SUPPORT
		{
            nvram_set("vlan2hwname", "et0");
            nvram_set("vlan1hwname", "et0");
		}
		#endif
    }
	else
#endif /*VLAN_SUPPORT*/
/*Foxconn add end, edward zhang, 2013/07/03*/
	#ifdef CONFIG_2ND_SWITCH
	if (((!isTriBand()) && (iptv_bridge_intf & IPTV_MASK)) 
		|| (isTriBand() && (iptv_bridge_intf & IPTV_EXT_MASK)))
	#else
    if (iptv_bridge_intf & IPTV_MASK)
	#endif /*CONFIG_2ND_SWITCH*/
    {
        char lan_ifnames[128] = "vlan1 ";
        char wan_ifnames[128] = "vlan2 ";
    
		#ifdef __CONFIG_IGMP_SNOOPING__
        /* always enable snooping for IPTV */
        nvram_set("emf_enable", "1");
		#endif

        /* always build vlan2 and br1 and enable vlan tag output for all vlan */
		#ifdef __CONFIG_GMAC3__
        if(nvram_match("gmac3_enable", "1"))
            nvram_set("vlan2ports", "0 8");
        else
        {
			/*Foxconn add , edward zhang, 2013/07/03*/
            nvram_set("vlan2ports", "0 5");
        }
		#else

        nvram_set("vlan2ports", "4 5");
		#endif

        /* build vlan3 for IGMP snooping on IPTV ports */
        if (strlen(vlan_iptv_ports))
            strcat(wan_ifnames, "vlan3 ");

        if (iptv_bridge_intf & IPTV_WLAN1)
            strcat(wan_ifnames, "eth1 ");
        else
            strcat(lan_ifnames, "eth1 ");

        if (iptv_bridge_intf & IPTV_WLAN2)
            strcat(wan_ifnames, "eth2 ");
        else
            strcat(lan_ifnames, "eth2 ");

		if(isTriBand())
		{
			if (iptv_bridge_intf & IPTV_WLAN3)
				strcat(wan_ifnames, "eth3 ");
			else
				strcat(lan_ifnames, "eth3 ");
		}

        if (iptv_bridge_intf & IPTV_WLAN_GUEST1)
            strcat(wan_ifnames, "wl0.1 ");
        else
            strcat(lan_ifnames, "wl0.1 ");

        if (iptv_bridge_intf & IPTV_WLAN_GUEST2)
            strcat(wan_ifnames, "wl1.1 ");
        else
            strcat(lan_ifnames, "wl1.1 ");

		if(isTriBand())
		{
			if (iptv_bridge_intf & IPTV_WLAN_GUEST3)
				strcat(wan_ifnames, "wl2.1 ");
			else
				strcat(lan_ifnames, "wl2.1 ");
		}

        //nvram_set("lan_ifnames", lan_ifnames);
#ifdef __CONFIG_GMAC3__
        strcpy(br0_ifnames,lan_ifnames);
#else
        strcpy(br0_ifnames,lan_ifnames);
#endif        
        nvram_set("wan_ifnames", wan_ifnames);
        nvram_set("lan1_ifnames", wan_ifnames);

        nvram_set("wan_ifname", "br1");
        nvram_set("lan1_ifname", "br1");
    }
    else
    {
        
        //nvram_set("lan_ifnames", "vlan1 eth1 eth2 wl0.1");
        /*modified by dennis start, 05/03/2012,fixed guest network cannot reconnect issue*/
#ifdef __CONFIG_GMAC3__
        if(nvram_match("gmac3_enable", "1"))
            strcpy(br0_ifnames,"vlan1");       
        else
        {
/*Foxconn add start, edward zhang, 2013/07/03*/
#ifdef VLAN_SUPPORT
            nvram_set("vlan2hwname", "et0");
            nvram_set("vlan1hwname", "et0");
#endif
            if(!nvram_match("enable_vlan", "enable"))
                strcpy(br0_ifnames,"vlan1 eth1 eth2 eth3");       
        }
#else
        strcpy(br0_ifnames,"vlan1 eth1 eth2");       
#endif
        /*modified by dennis end, 05/03/2012,fixed guest network cannot reconnect issue*/
        nvram_set("lan1_ifnames", "");
        nvram_set("lan1_ifname", "");

#ifdef __CONFIG_IGMP_SNOOPING__
        /* foxconn Bob modified start 07/18/2014, not to bridge eth0 and vlan1 in the same bridge, or may cause broadcast radiation */
        if (nvram_match("emf_enable", "1") || nvram_match("enable_ap_mode", "1") ) {
        /* foxconn Bob modified end 07/18/2014 */
#ifdef __CONFIG_GMAC3__
            if(nvram_match("gmac3_enable", "1"))
                nvram_set("vlan2ports", "0 8");
            else
                nvram_set("vlan2ports", "0 5");
#else
            nvram_set("vlan2ports", "0 5");
#endif
            /* foxconn Bob modified start 07/18/2014, not to bridge eth0 and vlan1 in the same bridge, or may cause broadcast radiation */
            nvram_set("wan_ifnames", "vlan2");
            nvram_set("wan_ifname", "vlan2");
            /* foxconn Bob modified end 07/18/2014 */
        }
        else
#endif
        {
#ifdef __CONFIG_GMAC3__

            if(nvram_match("gmac3_enable", "1"))
            {
                if (nvram_match("enable_ap_mode", "1")) {
                    nvram_set("vlan2ports", "0 8");
                    nvram_set("wan_ifnames", "vlan2 ");
                    nvram_set("wan_ifname", "vlan2");
                }
                else {
                    nvram_set("vlan2ports", "0 8u");
                    nvram_set("wan_ifnames", "eth0 ");
                    nvram_set("wan_ifname", "eth0");
                }
            }
            else
            {
                if (nvram_match("enable_ap_mode", "1")) {
                    nvram_set("vlan2ports", "0 5");
                    nvram_set("wan_ifnames", "vlan2 ");
                    nvram_set("wan_ifname", "vlan2");
                }
                else {
                    nvram_set("vlan2ports", "0 5u");
                    nvram_set("wan_ifnames", "eth0 ");
                    nvram_set("wan_ifname", "eth0");
                }
            }
#else
            nvram_set("vlan2ports", "0 5");
            nvram_set("wan_ifnames", "eth0 ");
            nvram_set("wan_ifname", "eth0");
#endif
/* foxconn revise end ken chen @ 08/23/2013, to fix IGMP report duplicated in AP mode*/
        }
    }

     /*added by dennis start, 05/03/2012,fixed guest network cannot reconnect issue*/
     for(i = MIN_BSSID_NUM; i <= MAX_BSSID_NUM; i++){
        sprintf(wl_param, "%s_%d", "wla_sec_profile_enable", i);     
        if(nvram_match(wl_param, "1")){
            sprintf(if_name, "wl0.%d", i-1);
            if(nvram_match("enable_vlan", "enable"))
            {
                if(!(enabled_vlan_ports & IPTV_WLAN_GUEST1))
                {
                    strcat(br0_ifnames, " ");
                    strcat(br0_ifnames, if_name);
                }
            }
            else if (nvram_match(NVRAM_IPTV_ENABLED, "1"))
            {
            	// Do nothing here
            }
            else
            {
                strcat(br0_ifnames, " ");
                strcat(br0_ifnames, if_name);
            }
            	
        }
     }

     for(i = MIN_BSSID_NUM; i <= MAX_BSSID_NUM; i++){
         sprintf(wl_param, "%s_%d", "wlg_sec_profile_enable", i);        
         if(nvram_match(wl_param, "1")){
             sprintf(if_name, "wl1.%d", i-1);
            if(nvram_match("enable_vlan", "enable"))
            {
                if(!(enabled_vlan_ports & IPTV_WLAN_GUEST2))
                {
                    strcat(br0_ifnames, " ");
                    strcat(br0_ifnames, if_name);
                }
            }
            else if (nvram_match(NVRAM_IPTV_ENABLED, "1"))
            {
            	// Do nothing here
            }
            else
            {
                strcat(br0_ifnames, " ");
                strcat(br0_ifnames, if_name);
            }
         }
     }

#if defined(R8000)
     for(i = MIN_BSSID_NUM; i <= MAX_BSSID_NUM; i++){
         sprintf(wl_param, "%s_%d", "wlh_sec_profile_enable", i);        
         if(nvram_match(wl_param, "1")){
             sprintf(if_name, "wl2.%d", i-1);
            if(nvram_match("enable_vlan", "enable"))
            {
                if(!(enabled_vlan_ports & IPTV_WLAN_GUEST3))
                {
                    strcat(br0_ifnames, " ");
                    strcat(br0_ifnames, if_name);
                }
            }
            else if (nvram_match(NVRAM_IPTV_ENABLED, "1"))
            {
            	// Do nothing here
            }
	          else
            {
                strcat(br0_ifnames, " ");
                strcat(br0_ifnames, if_name);
            }
         }
     }
#endif
#ifdef __CONFIG_GMAC3__
     if(nvram_match("iptv_enabled", "1"))
         nvram_set("lan_ifnames", br0_ifnames);
     else if(nvram_match("enable_vlan", "enable"))
         nvram_set("lan_ifnames", br0_ifnames);
     else
         nvram_set("lan_ifnames", "vlan1 eth1 eth2 eth3 wl0.1 wl1.1 wl2.1");
#else
     nvram_set("lan_ifnames", br0_ifnames);
#endif     
    /*added by dennis start, 05/03/2012,fixed guest network cannot reconnect issue*/
	/* Foxconn added start pling 08/17/2012 */
    /* Fix: When IPTV is enabled, WAN interface is "br1".
     * This can cause CTF/pktc to work abnormally.
     * So bypass CTF/pktc altogether */
    if (nvram_match(NVRAM_IPTV_ENABLED, "1"))
        eval("et", "robowr", "0xFFFF", "0xFB", "1");
    else
        eval("et", "robowr", "0xFFFF", "0xFB", "0");
    /* Foxconn added end pling 08/17/2012 */
    return 0;
}
#endif
#ifdef VLAN_SUPPORT

static int active_vlan(void)
{
    char buf[128];
    unsigned char mac[ETHER_ADDR_LEN];
    char eth0_mac[32];

    /* foxconn Han edited, 05/28/2015 for external switch, 
     * don't change switch configuration by our own.*/
    //return 0;

    strcpy(eth0_mac, nvram_safe_get("et0macaddr"));
    ether_atoe(eth0_mac, mac);

    /* Set MAC address byte 0 */
    sprintf(buf, "et robowr 0x%04X 0x%02X 0x%02X%02X", VCFG_PAGE, VCFG_REG, MAC_BYTE0, mac[0]);
    system(buf);
    /* Set MAC address byte 1 */
    sprintf(buf, "et robowr 0x%04X 0x%02X 0x%02X%02X", VCFG_PAGE, VCFG_REG, MAC_BYTE1, mac[1]);
    system(buf);
    /* Set MAC address byte 2 */
    sprintf(buf, "et robowr 0x%04X 0x%02X 0x%02X%02X", VCFG_PAGE, VCFG_REG, MAC_BYTE2, mac[2]);
    system(buf);
    /* Set MAC address byte 3 */
    sprintf(buf, "et robowr 0x%04X 0x%02X 0x%02X%02X", VCFG_PAGE, VCFG_REG, MAC_BYTE3, mac[3]);
    system(buf);
    /* Set MAC address byte 4 */
    sprintf(buf, "et robowr 0x%04X 0x%02X 0x%02X%02X", VCFG_PAGE, VCFG_REG, MAC_BYTE4, mac[4]);
    system(buf);
    /* Set MAC address byte 5 */
    sprintf(buf, "et robowr 0x%04X 0x%02X 0x%02X%02X", VCFG_PAGE, VCFG_REG, MAC_BYTE5, mac[5]);
    system(buf);
    /* Issue command to activate new vlan configuration. */
    sprintf(buf, "et robowr 0x%04X 0x%02X 0x%02X00", VCFG_PAGE, VCFG_REG, SET_VLAN);
    system(buf);

    return 0;
}
#endif

#if (defined INCLUDE_QOS) || (defined __CONFIG_IGMP_SNOOPING__)
/* these settings are for BCM53115S switch */
static int config_switch_reg(void)
{

    /* foxconn Han edited, 05/28/2015 for external switch, 
     * don't change switch configuration by our own.*/
    return 0;

#ifdef VLAN_SUPPORT
    if(nvram_match("enable_vlan", "enable"))
    {

        system("et robowr 0x00 0x08 0x1C");
        system("et robowr 0x00 0x0B 0x07");
        system("et robowr 0x02 0x00 0x80");
#ifdef BCM5301X           
        /*Enable BRCM header for port 5*/
        system("et robowr 0x02 0x03 0x02");  /* Foxconn Bob added for 4708 */
#endif        
        system("et robowr 0xFFFF 0xFA 1");    	
    }
#endif

    if (
#if (defined __CONFIG_IGMP_SNOOPING__)
        nvram_match("emf_enable", "1") ||
#endif
#if defined(CONFIG_RUSSIA_IPTV)
		nvram_match("iptv_enabled", "1") ||
#endif          
        (nvram_match("qos_enable", "1")  
        && !nvram_match("wla_repeater", "1")
#if (defined INCLUDE_DUAL_BAND)
        && !nvram_match("wlg_repeater", "1")
#endif
        && !nvram_match("qos_port", "")))
    {
        /* Enables the receipt of unicast, multicast and broadcast on IMP port */
        system("et robowr 0x00 0x08 0x1C");
        /* Enable Frame-managment mode */
        system("et robowr 0x00 0x0B 0x07");
        /* Enable management port */
        system("et robowr 0x02 0x00 0x80");
#ifdef BCM5301X           
        /*Enable BRCM header for port 5*/
        system("et robowr 0x02 0x03 0x02");  /* Foxconn Bob added for 4708 */
#endif        
        /* CRC bypass and auto generation */
//        system("et robowr 0x34 0x06 0x11");
#if (defined __CONFIG_IGMP_SNOOPING__)
        if (nvram_match("emf_enable", "1"))
        {
#if 0
            /* Set IMP port default tag id */
            system("et robowr 0x34 0x20 0x02");
            /* Enable IPMC bypass V fwdmap */
            system("et robowr 0x34 0x01 0x2E");
            /* Set Multiport address enable */
            system("et robowr 0x04 0x0E 0x0AAA");
#endif
        }
#endif
        /* Turn on the flags for kernel space (et/emf/igs) handling */
        system("et robowr 0xFFFF 0xFE 0x03");
    }
    else
    {
#if 0
        system("et robowr 0x00 0x08 0x00");
        system("et robowr 0x00 0x0B 0x06");
        system("et robowr 0x02 0x00 0x00");
#ifdef BCM5301X          
        /*Enable BRCM header for port 8*/
        system("et robowr 0x02 0x03 0x01");  /* Foxconn Bob added for 4708 */
#endif        
        system("et robowr 0x34 0x06 0x10");
#if (defined __CONFIG_IGMP_SNOOPING__)
        system("et robowr 0x34 0x20 0x02");
        system("et robowr 0x34 0x01 0x0E");
        system("et robowr 0x04 0x0E 0x0000");
#endif
        if (nvram_match("qos_enable", "1") )
            system("et robowr 0xFFFF 0xFE 0x01");
        else if (!nvram_match("qos_port", ""))
            system("et robowr 0xFFFF 0xFE 0x02");
        else
            system("et robowr 0xFFFF 0xFE 0x00");
#endif
    }

    return 0;
}
/* foxconn added end, zacker, 01/13/2012, @iptv_igmp */

/* foxconn modified start, zacker, 01/13/2012, @iptv_igmp */
static void config_switch(void)
{
    /* BCM5325 & BCM53115 switch request to change these vars
     * to output ethernet port tag/id in packets.
     */
    struct nvram_tuple generic_gmac3[] = {
        { "wan_ifname", "eth0", 0 },
        { "wan_ifnames", "eth0 ", 0 },

        { "vlan1ports", "1 2 3 4 5 7 8*", 0 },
        { "vlan2ports", "0 8u", 0 },
        { 0, 0, 0 }
    };

    struct nvram_tuple generic[] = {
        { "wan_ifname", "eth0", 0 },
        { "wan_ifnames", "eth0 ", 0 },

        { "vlan1ports", "1 2 3 4 5*", 0 },
        { "vlan2ports", "0 5u", 0 },
        { 0, 0, 0 }
    };

    struct nvram_tuple vlan_gmac3[] = {
        { "wan_ifname", "vlan2", 0 },
        { "wan_ifnames", "vlan2 ", 0 },

        { "vlan1ports", "1 2 3 4 5 7 8*", 0 },
        { "vlan2ports", "0 8", 0 },
        { 0, 0, 0 }
    };

    struct nvram_tuple vlan[] = {
        { "wan_ifname", "vlan2", 0 },
        { "wan_ifnames", "vlan2 ", 0 },

        { "vlan1ports", "1 2 3 4 5*", 0 },
        { "vlan2ports", "0 5", 0 },
        { 0, 0, 0 }
    };

    struct nvram_tuple *u;
    int commit = 0;

    if(nvram_match("gmac3_enable", "1"))
        u = generic_gmac3;
    else
        u = generic;
    	
    /* foxconn Bob modified start 08/26/2013, not to bridge eth0 and vlan1 in the same bridge */
    if (nvram_match("emf_enable", "1") || nvram_match("enable_ap_mode", "1") ) {
        if(nvram_match("gmac3_enable", "1"))
            u = vlan_gmac3;
        else
            u = vlan;
    }
    /* foxconn Bob modified end 08/26/2013, not to bridge eth0 and vlan1 in the same bridge */

    /* don't need vlan in repeater mode */
    if (nvram_match("wla_repeater", "1")
#if (defined INCLUDE_DUAL_BAND)
        || nvram_match("wlg_repeater", "1")
#endif
        ) {
    if(nvram_match("gmac3_enable", "1"))
        u = generic_gmac3;
    else
        u = generic;
    }

    for ( ; u && u->name; u++) {
        if (strcmp(nvram_safe_get(u->name), u->value)) {
            commit = 1;
            nvram_set(u->name, u->value);
        }
    }

    /*foxconn Han edited, 05/11/2015
    * From CSP 915149
    * details:
    * =========================================
    * Cathy Yeh 08-May-2015 12:40:27 AM 
    *      
    * Hi Han,
    *
    * Please find the 2nd switch's patch, 150506_erobo_patch.tgz, in attachment.
    * 1. Set nvram "erobo=1" to attach the 2nd switch
    * 2. Support et command for read/write the 2nd switch's registers:
    *   et -i eth0 erobord <page> <reg> [length] (read the reg of external switch, the usage is same as robord)
    *   et -i eth0 erobowr <page> <reg> <val> [length] (write the reg of external switch, the usage is same as robowr)
    * 3. Set nvram "evlanXXXXports" to configure the 2nd switch's vlan table:
    *   Ex: nvram set evlan1ports"0 1 2 3 4 5u"
    *
    *       Regards,
    *       Cathy  
    * ==========================================*/
    #ifdef CONFIG_2ND_SWITCH
    if(acosNvramConfig_match("hwver",acosNvramConfig_get("tri_band_hw_ver")))
    {
        nvram_set("erobo","1");
        nvram_set("evlan1ports", "0 1 2 3 4 5u");
    }
    #endif /*CONFIG_2ND_SWITCH*/

    if (commit) {
        cprintf("Commit new ethernet config...\n");
        nvram_commit();
        commit = 0;
    }
}
#endif
/* foxconn modified end, zacker, 01/13/2012, @iptv_igmp */

/* foxconn modified start, zacker, 01/04/2011 */
static int should_stop_wps(void)
{
    /* WPS LED OFF */
    if ((nvram_match("wla_wlanstate","Disable") || acosNvramConfig_match("wifi_on_off", "0"))
#if (defined INCLUDE_DUAL_BAND)
        && (nvram_match("wlg_wlanstate","Disable") || acosNvramConfig_match("wifi_on_off", "0"))
#endif
       )
        return WPS_LED_STOP_RADIO_OFF;

    /* WPS LED quick blink for 5sec */
    if (nvram_match("wps_mode", "disabled")
        || nvram_match("wla_repeater", "1")
#if (defined INCLUDE_DUAL_BAND)
        || nvram_match("wlg_repeater", "1")
#endif
       )
        return WPS_LED_STOP_DISABLED;

    /* WPS LED original action */
    return WPS_LED_STOP_NO;
}

static int is_secure_wl(void)
{
    /* for ACR5500 , there is only on WiFi LED for WPS */
#if defined(R6300v2) || defined(R6250) || defined(R6200v2) || defined(R7000) || defined(R8000)

    if ((acosNvramConfig_match("wla_wlanstate","Disable") || acosNvramConfig_match("wifi_on_off", "0"))
        && (acosNvramConfig_match("wlg_wlanstate","Disable") || acosNvramConfig_match("wifi_on_off", "0")) )
        return 0;

    return 1;
#else    
    
    if (   (!acosNvramConfig_match("wla_secu_type", "None")
            && (acosNvramConfig_match("wla_wlanstate","Enable") && acosNvramConfig_match("wifi_on_off", "1")))
#if (defined INCLUDE_DUAL_BAND)
        || (!acosNvramConfig_match("wlg_secu_type", "None")
            && (acosNvramConfig_match("wlg_wlanstate","Enable") && acosNvramConfig_match("wifi_on_off", "1")))
#endif
        )
        return 1;

    return 0;
#endif /* defined(R6300v2) */    
}

/* Foxconn added start, Wins, 04/20/2011 @RU_IPTV */
#ifdef CONFIG_RUSSIA_IPTV
static int is_russia_specific_support (void)
{
    int result = 0;
    char sku_name[8];

    /* Router Spec v2.0:                                                        *
     *   Case 1: RU specific firmware.                                          *
     *   Case 2: single firmware & region code is RU.                           *
     *   Case 3: WW firmware & GUI language is Russian.                         *
     *   Case 4: single firmware & region code is WW & GUI language is Russian. *
     * Currently, new built firmware will be single firmware.                   */
    strcpy(sku_name, nvram_get("sku_name"));
    if (!strcmp(sku_name, "RU"))
    {
        /* Case 2: single firmware & region code is RU. */
        /* Region is RU (0x0005) */
        result = 1;
    }
    else if (!strcmp(sku_name, "WW"))
    {
        /* Region is WW (0x0002) */
        char gui_region[16];
        strcpy(gui_region, nvram_get("gui_region"));
        if (!strcmp(gui_region, "Russian"))
        {
            /* Case 4: single firmware & region code is WW & GUI language is Russian */
            /* GUI language is Russian */
            result = 1;
        }
    }

    return result;
}
/* Foxconn add start, Edward zhang, 09/05/2012, @add IPTV support for PR SKU*/
static int is_china_specific_support (void)
{
    int result = 0;
    char sku_name[8];

    /* Router Spec v2.0:                                                        *
     *   Case 1: WW specific firmware.                                          *
     *   Case 2: single firmware & region code is PR.                           *
     *   Case 3: WW firmware & GUI language is Chinise.                         *
     *   Case 4: single firmware & region code is WW & GUI language is Chinise. *
     * Currently, new built firmware will be single firmware.                   */
    strcpy(sku_name, nvram_get("sku_name"));
    if (!strcmp(sku_name, "PR"))
    {
        /* Case 2: single firmware & region code is PR. */
        /* Region is PR (0x0004) */
        result = 1;
    }
    else if (!strcmp(sku_name, "WW"))
    {
        /* Region is WW (0x0002) */
        char gui_region[16];
        strcpy(gui_region, nvram_get("gui_region"));
        if (!strcmp(gui_region, "Chinese"))
        {
            /* Case 4: single firmware & region code is WW & GUI language is Chinise */
            /* GUI language is Chinise */
            result = 1;
        }
    }

    return result;
}
/* Foxconn add end, Edward zhang, 09/05/2012, @add IPTV support for PR SKU*/
#endif /* CONFIG_RUSSIA_IPTV */
/* Foxconn added end, Wins, 04/20/2011 @RU_IPTV */

/*Foxconn add start, edward zhang, 2013/07/03*/
#ifdef VLAN_SUPPORT
static int getVlanname(char vlanname[C_MAX_VLAN_RULE][C_MAX_TOKEN_SIZE])
{
    char *var;

    if ((var = acosNvramConfig_get("vlan_name")) != NULL)
    {
        int num, i;
        num = getTokens(var, " ", vlanname, C_MAX_VLAN_RULE);
        for (i = 0; i< num; i++)
            restore_devname(vlanname[i]);
        return num;
    }

    return 0;
}



static int getVlanRule(vlan_rule vlan[C_MAX_VLAN_RULE])
{
    int numVlanRule = 0 , i;
    char *var;
    char VlanName[C_MAX_VLAN_RULE][C_MAX_TOKEN_SIZE];
    char VlanId[C_MAX_VLAN_RULE][C_MAX_TOKEN_SIZE];
    char VlanPrio[C_MAX_VLAN_RULE][C_MAX_TOKEN_SIZE];
    char VlanPorts[C_MAX_VLAN_RULE][C_MAX_TOKEN_SIZE];
    char VlanRuleEnable[C_MAX_VLAN_RULE][C_MAX_TOKEN_SIZE];
    if ( (var = acosNvramConfig_get("vlan_id")) != NULL )
    {
        getTokens(var, " ", VlanId, C_MAX_VLAN_RULE);
    }
    
    if ( (var=acosNvramConfig_get("vlan_prio")) != NULL )
    {
        getTokens(var, " ", VlanPrio, C_MAX_VLAN_RULE);
    }
    
    if ( (var=acosNvramConfig_get("vlan_ports")) != NULL )
    {
        getTokens(var, " ", VlanPorts, C_MAX_VLAN_RULE);
    }
 
    if ( (var=acosNvramConfig_get("vlan_rule_enable")) != NULL )
    {
        getTokens(var, " ", VlanRuleEnable, C_MAX_VLAN_RULE);
    }
    
    numVlanRule = getVlanname(VlanName);
    
    for(i=0;i<numVlanRule;i++)
    {
        strcpy( vlan[i].vlan_name , VlanName[i]);
        strcpy( vlan[i].vlan_id , VlanId[i]);
        strcpy( vlan[i].vlan_prio , VlanPrio[i]);
        //strcpy( vlan[i].vlan_ports , VlanPorts[i]);
        sprintf( vlan[i].vlan_ports,"%s",VlanPorts[i]);
        strcpy( vlan[i].enable_rule , VlanRuleEnable[i]);
    }
    
    return numVlanRule;
}
#endif

static int send_wps_led_cmd(int cmd, int arg)
{
    int ret_val=0;
    int fd;

    fd = open(DEV_WPS_LED, O_RDWR);
    if (fd < 0) 
        return -1;

    if (is_secure_wl())
        arg = 1;
    else
        arg = 0;

    switch (should_stop_wps())
    {
        case WPS_LED_STOP_RADIO_OFF:
            cmd = WPS_LED_BLINK_OFF;
            break;
            
        case WPS_LED_STOP_DISABLED:
            if (cmd == WPS_LED_BLINK_NORMAL)
                cmd = WPS_LED_BLINK_QUICK;
            break;
            
        case WPS_LED_STOP_NO:
        default:
            break;
    }

    ret_val = ioctl(fd, cmd, arg);
    close(fd);

    return ret_val;
}
/* foxconn modified end, zacker, 01/04/2011 */

static int
build_ifnames(char *type, char *names, int *size)
{
	char name[32], *next;
	int len = 0;
	int s;

	/* open a raw scoket for ioctl */
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
		return -1;

	/*
	 * go thru all device names (wl<N> il<N> et<N> vlan<N>) and interfaces to
	 * build an interface name list in which each i/f name coresponds to a device
	 * name in device name list. Interface/device name matching rule is device
	 * type dependant:
	 *
	 *	wl:	by unit # provided by the driver, for example, if eth1 is wireless
	 *		i/f and its unit # is 0, then it will be in the i/f name list if
	 *		wl0 is in the device name list.
	 *	il/et:	by mac address, for example, if et0's mac address is identical to
	 *		that of eth2's, then eth2 will be in the i/f name list if et0 is
	 *		in the device name list.
	 *	vlan:	by name, for example, vlan0 will be in the i/f name list if vlan0
	 *		is in the device name list.
	 */
	foreach(name, type, next) {
		struct ifreq ifr;
		int i, unit;
		char var[32], *mac;
		unsigned char ea[ETHER_ADDR_LEN];

		/* vlan: add it to interface name list */
		if (!strncmp(name, "vlan", 4)) {
			/* append interface name to list */
			len += snprintf(&names[len], *size - len, "%s ", name);
			continue;
		}

		/* others: proceed only when rules are met */
		for (i = 1; i <= DEV_NUMIFS; i ++) {
			/* ignore i/f that is not ethernet */
			ifr.ifr_ifindex = i;
			if (ioctl(s, SIOCGIFNAME, &ifr))
				continue;
			if (ioctl(s, SIOCGIFHWADDR, &ifr))
				continue;
			if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER)
				continue;
			if (!strncmp(ifr.ifr_name, "vlan", 4))
				continue;

			/* wl: use unit # to identify wl */
			if (!strncmp(name, "wl", 2)) {
				if (wl_probe(ifr.ifr_name) ||
				    wl_ioctl(ifr.ifr_name, WLC_GET_INSTANCE, &unit, sizeof(unit)) ||
				    unit != atoi(&name[2]))
					continue;
			}
			/* et/il: use mac addr to identify et/il */
			else if (!strncmp(name, "et", 2) || !strncmp(name, "il", 2)) {
				snprintf(var, sizeof(var), "%smacaddr", name);
				if (!(mac = nvram_get(var)) || !ether_atoe(mac, ea) ||
				    bcmp(ea, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN))
					continue;
			}
			/* mac address: compare value */
			else if (ether_atoe(name, ea) &&
				!bcmp(ea, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN))
				;
			/* others: ignore */
			else
				continue;

			/* append interface name to list */
			len += snprintf(&names[len], *size - len, "%s ", ifr.ifr_name);
		}
	}

	close(s);

	*size = len;
	return 0;
}

#ifdef __CONFIG_WPS__
static void
wps_restore_defaults(void)
{
	/* cleanly up nvram for WPS */
	nvram_unset("wps_config_state");
	nvram_unset("wps_device_pin");
	nvram_unset("wps_proc_status");
	nvram_unset("wps_sta_pin");
	nvram_unset("wps_restart");
	nvram_unset("wps_config_method");
}
#endif /* __CONFIG_WPS__ */

#if defined(LINUX_2_6_36) && defined(__CONFIG_TREND_IQOS__)
static void
iqos_restore_defaults(void)
{
	nvram_set("broadstream_iqos_default_conf", "1");
}
#endif /* LINUX_2_6_36 & __CONFIG_TREND_IQOS__ */

static void
virtual_radio_restore_defaults(void)
{
	char tmp[100], prefix[] = "wlXXXXXXXXXXXXXXXXXXXXXXXXXXXX_mssid_";
	int i, j;

	nvram_unset("unbridged_ifnames");
	nvram_unset("ure_disable");

	/* Delete dynamically generated variables */
	for (i = 0; i < MAX_NVPARSE; i++) {
		sprintf(prefix, "wl%d_", i);
		nvram_unset(strcat_r(prefix, "vifs", tmp));
		nvram_unset(strcat_r(prefix, "ssid", tmp));
		nvram_unset(strcat_r(prefix, "guest", tmp));
		nvram_unset(strcat_r(prefix, "ure", tmp));
		nvram_unset(strcat_r(prefix, "ipconfig_index", tmp));
		nvram_unset(strcat_r(prefix, "nas_dbg", tmp));
		sprintf(prefix, "lan%d_", i);
		nvram_unset(strcat_r(prefix, "ifname", tmp));
		nvram_unset(strcat_r(prefix, "ifnames", tmp));
		nvram_unset(strcat_r(prefix, "gateway", tmp));
		nvram_unset(strcat_r(prefix, "proto", tmp));
		nvram_unset(strcat_r(prefix, "ipaddr", tmp));
		nvram_unset(strcat_r(prefix, "netmask", tmp));
		nvram_unset(strcat_r(prefix, "lease", tmp));
		nvram_unset(strcat_r(prefix, "stp", tmp));
		nvram_unset(strcat_r(prefix, "hwaddr", tmp));
		sprintf(prefix, "dhcp%d_", i);
		nvram_unset(strcat_r(prefix, "start", tmp));
		nvram_unset(strcat_r(prefix, "end", tmp));

		/* clear virtual versions */
		for (j = 0; j < 16; j++) {
			sprintf(prefix, "wl%d.%d_", i, j);
			nvram_unset(strcat_r(prefix, "ssid", tmp));
			nvram_unset(strcat_r(prefix, "ipconfig_index", tmp));
			nvram_unset(strcat_r(prefix, "guest", tmp));
			nvram_unset(strcat_r(prefix, "closed", tmp));
			nvram_unset(strcat_r(prefix, "wpa_psk", tmp));
			nvram_unset(strcat_r(prefix, "auth", tmp));
			nvram_unset(strcat_r(prefix, "wep", tmp));
			nvram_unset(strcat_r(prefix, "auth_mode", tmp));
			nvram_unset(strcat_r(prefix, "crypto", tmp));
			nvram_unset(strcat_r(prefix, "akm", tmp));
			nvram_unset(strcat_r(prefix, "hwaddr", tmp));
			nvram_unset(strcat_r(prefix, "bss_enabled", tmp));
			nvram_unset(strcat_r(prefix, "bss_maxassoc", tmp));
			nvram_unset(strcat_r(prefix, "wme_bss_disable", tmp));
			nvram_unset(strcat_r(prefix, "ifname", tmp));
			nvram_unset(strcat_r(prefix, "unit", tmp));
			nvram_unset(strcat_r(prefix, "ap_isolate", tmp));
			nvram_unset(strcat_r(prefix, "macmode", tmp));
			nvram_unset(strcat_r(prefix, "maclist", tmp));
			nvram_unset(strcat_r(prefix, "maxassoc", tmp));
			nvram_unset(strcat_r(prefix, "mode", tmp));
			nvram_unset(strcat_r(prefix, "radio", tmp));
			nvram_unset(strcat_r(prefix, "radius_ipaddr", tmp));
			nvram_unset(strcat_r(prefix, "radius_port", tmp));
			nvram_unset(strcat_r(prefix, "radius_key", tmp));
			nvram_unset(strcat_r(prefix, "key", tmp));
			nvram_unset(strcat_r(prefix, "key1", tmp));
			nvram_unset(strcat_r(prefix, "key2", tmp));
			nvram_unset(strcat_r(prefix, "key3", tmp));
			nvram_unset(strcat_r(prefix, "key4", tmp));
			nvram_unset(strcat_r(prefix, "wpa_gtk_rekey", tmp));
			nvram_unset(strcat_r(prefix, "nas_dbg", tmp));
			nvram_unset(strcat_r(prefix, "probresp_mf", tmp));

			nvram_unset(strcat_r(prefix, "bss_opmode_cap_reqd", tmp));
			nvram_unset(strcat_r(prefix, "mcast_regen_bss_enable", tmp));
			nvram_unset(strcat_r(prefix, "wmf_bss_enable", tmp));
			nvram_unset(strcat_r(prefix, "preauth", tmp));
			nvram_unset(strcat_r(prefix, "dwds", tmp));
			nvram_unset(strcat_r(prefix, "acs_dfsr_deferred", tmp));
			nvram_unset(strcat_r(prefix, "wet_tunnel", tmp));
			nvram_unset(strcat_r(prefix, "bridge", tmp));
			nvram_unset(strcat_r(prefix, "mfp", tmp));
			nvram_unset(strcat_r(prefix, "acs_dfsr_activity", tmp));
			nvram_unset(strcat_r(prefix, "acs_dfsr_immediate", tmp));
			nvram_unset(strcat_r(prefix, "wme", tmp));
			nvram_unset(strcat_r(prefix, "net_reauth", tmp));
			nvram_unset(strcat_r(prefix, "sta_retry_time", tmp));
			nvram_unset(strcat_r(prefix, "infra", tmp));

#ifdef __CONFIG_HSPOT__
			nvram_unset(strcat_r(prefix, "hsflag", tmp));
			nvram_unset(strcat_r(prefix, "hs2cap", tmp));
			nvram_unset(strcat_r(prefix, "opercls", tmp));
			nvram_unset(strcat_r(prefix, "anonai", tmp));
			nvram_unset(strcat_r(prefix, "wanmetrics", tmp));
			nvram_unset(strcat_r(prefix, "oplist", tmp));
			nvram_unset(strcat_r(prefix, "homeqlist", tmp));
			nvram_unset(strcat_r(prefix, "osu_ssid", tmp));
			nvram_unset(strcat_r(prefix, "osu_frndname", tmp));
			nvram_unset(strcat_r(prefix, "osu_uri", tmp));
			nvram_unset(strcat_r(prefix, "osu_nai", tmp));
			nvram_unset(strcat_r(prefix, "osu_method", tmp));
			nvram_unset(strcat_r(prefix, "osu_icons", tmp));
			nvram_unset(strcat_r(prefix, "osu_servdesc", tmp));
			nvram_unset(strcat_r(prefix, "concaplist", tmp));
			nvram_unset(strcat_r(prefix, "qosmapie", tmp));
			nvram_unset(strcat_r(prefix, "gascbdel", tmp));
			nvram_unset(strcat_r(prefix, "iwnettype", tmp));
			nvram_unset(strcat_r(prefix, "hessid", tmp));
			nvram_unset(strcat_r(prefix, "ipv4addr", tmp));
			nvram_unset(strcat_r(prefix, "ipv6addr", tmp));
			nvram_unset(strcat_r(prefix, "netauthlist", tmp));
			nvram_unset(strcat_r(prefix, "venuegrp", tmp));
			nvram_unset(strcat_r(prefix, "venuetype", tmp));
			nvram_unset(strcat_r(prefix, "venuelist", tmp));
			nvram_unset(strcat_r(prefix, "ouilist", tmp));
			nvram_unset(strcat_r(prefix, "3gpplist", tmp));
			nvram_unset(strcat_r(prefix, "domainlist", tmp));
			nvram_unset(strcat_r(prefix, "realmlist", tmp));
#endif /* __CONFIG_HSPOT__ */
		}
	}
}

#ifdef __CONFIG_URE__
static void
ure_restore_defaults(int unit)
{
	char tmp[100], prefix[] = "wlXXXXXXXXXX_";
	struct nvram_tuple *t;

	sprintf(prefix, "wl%d.1_", unit);

	for (t = router_defaults; t->name; t++) {
		if (!strncmp(t->name, "wl_", 3)) {
			strcat_r(prefix, &t->name[3], tmp);
			nvram_unset(tmp);
		}
	}

	sprintf(prefix, "wl%d_ure", unit);
	nvram_unset(prefix);
}

static void
set_ure_vars(int unit)
{
	int wl_unit = unit;
	int travel_router;
	char prefix[] = "wlXXXXXXXXXX_";
	struct nvram_tuple *t;
	char nv_param[NVRAM_MAX_PARAM_LEN];
	char nv_value[NVRAM_MAX_VALUE_LEN];
	char nv_interface[NVRAM_MAX_PARAM_LEN];
	char os_interface[NVRAM_MAX_PARAM_LEN];
	int os_interface_size = sizeof(os_interface);
	char *temp = NULL;
	char interface_list[NVRAM_MAX_VALUE_LEN];
	int interface_list_size = sizeof(interface_list);
	char *wan0_ifname = "wan0_ifname";
	char *lan_ifnames = "lan_ifnames";
	char *wan_ifnames = "wan_ifnames";

	sprintf(prefix, "wl%d.1_", wl_unit);

	/* Clone default wl nvram settings to wl0.1 */
	for (t = router_defaults; t->name; t++) {
		if (!strncmp(t->name, "wl_", 3)) {
			strcat_r(prefix, &t->name[3], nv_param);
			nvram_set(nv_param, t->value);
		}
	}

	/* Overwrite some specific nvram settings */
	sprintf(nv_param, "wl%d_ure", wl_unit);
	nvram_set(nv_param, "1");

	sprintf(nv_param, "wl%d_vifs", wl_unit);
	sprintf(nv_value, "wl%d.1", wl_unit);
	nvram_set(nv_param, nv_value);

	sprintf(nv_param, "wl%d.1_unit", wl_unit);
	sprintf(nv_value, "%d.1", wl_unit);
	nvram_set(nv_param, nv_value);

	nvram_set("wl_ampdu_rr_rtylimit_tid", "3 3 3 3 3 3 3 3");
	nvram_set("wl_ampdu_rtylimit_tid", "7 7 7 7 7 7 7 7");
	sprintf(nv_param, "wl%d_ampdu_rr_rtylimit_tid", wl_unit);
	nvram_set(nv_param, "3 3 3 3 3 3 3 3");
	sprintf(nv_param, "wl%d_ampdu_rtylimit_tid", wl_unit);
	nvram_set(nv_param, "7 7 7 7 7 7 7 7");

	if (nvram_match("router_disable", "1"))
		travel_router = 0;
	else
		travel_router = 1;

	/* Set the wl modes for the primary wireless adapter
	 * and it's virtual interface
	 */
	sprintf(nv_param, "wl%d_mode", wl_unit);
	if (travel_router == 1) {
		nvram_set(nv_param, "sta");
		nvram_set("wl_mode", "sta");
	}
	else {
		nvram_set(nv_param, "wet");
		nvram_set("wl_mode", "wet");
	}

	sprintf(nv_param, "wl%d.1_mode", wl_unit);
	nvram_set(nv_param, "ap");

	if (travel_router == 1) {
		/* For URE with routing (Travel Router) we're using the STA part
		 * of our URE enabled radio as our WAN connection. So, we need to
		 * remove this interface from the list of bridged lan interfaces
		 * and set it up as the WAN device.
		 */
		temp = nvram_safe_get(lan_ifnames);
		strncpy(interface_list, temp, interface_list_size);

		/* Leverage build_ifnames() to get OS-dependent interface name.
		 * One white space is appended after build_ifnames(); need to
		 * remove it.
		 */
		sprintf(nv_interface, "wl%d", wl_unit);
		memset(os_interface, 0, os_interface_size);
		build_ifnames(nv_interface, os_interface, &os_interface_size);
		if (strlen(os_interface) > 1) {
			os_interface[strlen(os_interface) - 1] = '\0';
		}
		remove_from_list(os_interface, interface_list, interface_list_size);
		nvram_set(lan_ifnames, interface_list);

		/* Now remove the existing WAN interface from "wan_ifnames" */
		temp = nvram_safe_get(wan_ifnames);
		strncpy(interface_list, temp, interface_list_size);

		temp = nvram_safe_get(wan0_ifname);
		if (strlen(temp) != 0) {
			/* Stash this interface name in an nvram variable in case
			 * we need to restore this interface as the WAN interface
			 * when URE is disabled.
			 */
			nvram_set("old_wan0_ifname", temp);
			remove_from_list(temp, interface_list, interface_list_size);
		}

		/* Set the new WAN interface as the pimary WAN interface and add to
		 * the list wan_ifnames.
		 */
		nvram_set(wan0_ifname, os_interface);
		add_to_list(os_interface, interface_list, interface_list_size);
		nvram_set(wan_ifnames, interface_list);

		/* Now add the AP to the list of bridged lan interfaces */
		temp = nvram_safe_get(lan_ifnames);
		strncpy(interface_list, temp, interface_list_size);
		sprintf(nv_interface, "wl%d.1", wl_unit);

		/* Virtual interfaces that appear in NVRAM lists are ALWAYS stored
		 * as the NVRAM_FORM so we can add to list without translating.
		 */
		add_to_list(nv_interface, interface_list, interface_list_size);
		nvram_set(lan_ifnames, interface_list);
	}
	else {
		/* For URE without routing (Range Extender) we're using the STA
		 * as a WET device to connect to the "upstream" AP. We need to
		 * add our virtual interface(AP) to the bridged lan.
		 */
		temp = nvram_safe_get(lan_ifnames);
		strncpy(interface_list, temp, interface_list_size);

		sprintf(nv_interface, "wl%d.1", wl_unit);
		add_to_list(nv_interface, interface_list, interface_list_size);
		nvram_set(lan_ifnames, interface_list);
	}

	/* Make lan1_ifname, lan1_ifnames empty so that br1 is not created in URE mode. */
	nvram_set("lan1_ifname", "");
	nvram_set("lan1_ifnames", "");

	if (nvram_match("wl0_ure_mode", "wre")) {
		/* By default, wl0 bss is disabled in WRE mode */
		nvram_set("wl_bss_enabled", "0");
		nvram_set("wl0_bss_enabled", "0");
	}
}
#endif /* __CONFIG_URE__ */


#ifdef __CONFIG_NAT__
static void
auto_bridge(void)
{

	struct nvram_tuple generic[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "eth0 eth2 eth3 eth4", 0 },
		{ "wan_ifname", "eth1", 0 },
		{ "wan_ifnames", "eth1", 0 },
		{ 0, 0, 0 }
	};
#ifdef __CONFIG_VLAN__
	struct nvram_tuple vlan[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "vlan0 eth1 eth2 eth3", 0 },
		{ "wan_ifname", "vlan1", 0 },
		{ "wan_ifnames", "vlan1", 0 },
		{ 0, 0, 0 }
	};
#endif	/* __CONFIG_VLAN__ */
	struct nvram_tuple dyna[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "", 0 },
		{ "wan_ifname", "", 0 },
		{ "wan_ifnames", "", 0 },
		{ 0, 0, 0 }
	};
	struct nvram_tuple generic_auto_bridge[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "eth0 eth1 eth2 eth3 eth4", 0 },
		{ "wan_ifname", "", 0 },
		{ "wan_ifnames", "", 0 },
		{ 0, 0, 0 }
	};
#ifdef __CONFIG_VLAN__
	struct nvram_tuple vlan_auto_bridge[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "vlan0 vlan1 eth1 eth2 eth3", 0 },
		{ "wan_ifname", "", 0 },
		{ "wan_ifnames", "", 0 },
		{ 0, 0, 0 }
	};
#endif	/* __CONFIG_VLAN__ */

	struct nvram_tuple dyna_auto_bridge[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "", 0 },
		{ "wan_ifname", "", 0 },
		{ "wan_ifnames", "", 0 },
		{ 0, 0, 0 }
	};

	struct nvram_tuple *linux_overrides;
	struct nvram_tuple *t, *u;
	int auto_bridge = 0, i;
#ifdef __CONFIG_VLAN__
	uint boardflags;
#endif	/* __CONFIG_VLAN_ */
	char *landevs, *wandevs;
	char lan_ifnames[128], wan_ifnames[128];
	char dyna_auto_ifnames[128];
	char wan_ifname[32], *next;
	int len;
	int ap = 0;

	printf(" INFO : enter function auto_bridge()\n");

	if (!strcmp(nvram_safe_get("auto_bridge_action"), "1")) {
		auto_bridge = 1;
		cprintf("INFO: Start auto bridge...\n");
	} else {
		nvram_set("router_disable_auto", "0");
		cprintf("INFO: Start non auto_bridge...\n");
	}

	/* Delete dynamically generated variables */
	if (auto_bridge) {
		char tmp[100], prefix[] = "wlXXXXXXXXXX_";
		for (i = 0; i < MAX_NVPARSE; i++) {

			del_filter_client(i);
			del_forward_port(i);
#if !defined(AUTOFW_PORT_DEPRECATED)
			del_autofw_port(i);
#endif

			snprintf(prefix, sizeof(prefix), "wan%d_", i);
			for (t = router_defaults; t->name; t ++) {
				if (!strncmp(t->name, "wan_", 4))
					nvram_unset(strcat_r(prefix, &t->name[4], tmp));
			}
		}
	}

	/*
	 * Build bridged i/f name list and wan i/f name list from lan device name list
	 * and wan device name list. Both lan device list "landevs" and wan device list
	 * "wandevs" must exist in order to preceed.
	 */
	if ((landevs = nvram_get("landevs")) && (wandevs = nvram_get("wandevs"))) {
		/* build bridged i/f list based on nvram variable "landevs" */
		len = sizeof(lan_ifnames);
		if (!build_ifnames(landevs, lan_ifnames, &len) && len)
			dyna[1].value = lan_ifnames;
		else
			goto canned_config;
		/* build wan i/f list based on nvram variable "wandevs" */
		len = sizeof(wan_ifnames);
		if (!build_ifnames(wandevs, wan_ifnames, &len) && len) {
			dyna[3].value = wan_ifnames;
			foreach(wan_ifname, wan_ifnames, next) {
				dyna[2].value = wan_ifname;
				break;
			}
		}
		else
			ap = 1;

		if (auto_bridge)
		{
			printf("INFO: lan_ifnames=%s\n", lan_ifnames);
			printf("INFO: wan_ifnames=%s\n", wan_ifnames);
			sprintf(dyna_auto_ifnames, "%s %s", lan_ifnames, wan_ifnames);
			printf("INFO: dyna_auto_ifnames=%s\n", dyna_auto_ifnames);
			dyna_auto_bridge[1].value = dyna_auto_ifnames;
			linux_overrides = dyna_auto_bridge;
			printf("INFO: linux_overrides=dyna_auto_bridge \n");
		}
		else
		{
			linux_overrides = dyna;
			printf("INFO: linux_overrides=dyna \n");
		}

	}
	/* override lan i/f name list and wan i/f name list with default values */
	else {
canned_config:
#ifdef __CONFIG_VLAN__
		boardflags = strtoul(nvram_safe_get("boardflags"), NULL, 0);
		if (boardflags & BFL_ENETVLAN) {
			if (auto_bridge)
			{
				linux_overrides = vlan_auto_bridge;
				printf("INFO: linux_overrides=vlan_auto_bridge \n");
			}
			else
			{
				linux_overrides = vlan;
				printf("INFO: linux_overrides=vlan \n");
			}
		} else {
#endif	/* __CONFIG_VLAN__ */
			if (auto_bridge)
			{
				linux_overrides = generic_auto_bridge;
				printf("INFO: linux_overrides=generic_auto_bridge \n");
			}
			else
			{
				linux_overrides = generic;
				printf("INFO: linux_overrides=generic \n");
			}
#ifdef __CONFIG_VLAN__
		}
#endif	/* __CONFIG_VLAN__ */
	}

		for (u = linux_overrides; u && u->name; u++) {
			nvram_set(u->name, u->value);
			printf("INFO: action nvram_set %s, %s\n", u->name, u->value);
			}

	/* Force to AP */
	if (ap)
		nvram_set("router_disable", "1");

	if (auto_bridge) {
		printf("INFO: reset auto_bridge flag.\n");
		nvram_set("auto_bridge_action", "0");
	}

	nvram_commit();
	cprintf("auto_bridge done\n");
}

#endif	/* __CONFIG_NAT__ */


static void
upgrade_defaults(void)
{
	char temp[100];
	int i;
	bool bss_enabled = TRUE;
	char *val;

	/* Check whether upgrade is required or not
	 * If lan1_ifnames is not found in NVRAM , upgrade is required.
	 */
	if (!nvram_get("lan1_ifnames") && !RESTORE_DEFAULTS()) {
		cprintf("NVRAM upgrade required.  Starting.\n");

		if (nvram_match("ure_disable", "1")) {
			nvram_set("lan1_ifname", "br1");
			nvram_set("lan1_ifnames", "wl0.1 wl0.2 wl0.3 wl1.1 wl1.2 wl1.3");
		}
		else {
			nvram_set("lan1_ifname", "");
			nvram_set("lan1_ifnames", "");
			for (i = 0; i < 2; i++) {
				snprintf(temp, sizeof(temp), "wl%d_ure", i);
				if (nvram_match(temp, "1")) {
					snprintf(temp, sizeof(temp), "wl%d.1_bss_enabled", i);
					nvram_set(temp, "1");
				}
				else {
					bss_enabled = FALSE;
					snprintf(temp, sizeof(temp), "wl%d.1_bss_enabled", i);
					nvram_set(temp, "0");
				}
			}
		}
		if (nvram_get("lan1_ipaddr")) {
			nvram_set("lan1_gateway", nvram_get("lan1_ipaddr"));
		}

		for (i = 0; i < 2; i++) {
			snprintf(temp, sizeof(temp), "wl%d_bss_enabled", i);
			nvram_set(temp, "1");
			snprintf(temp, sizeof(temp), "wl%d.1_guest", i);
			if (nvram_match(temp, "1")) {
				nvram_unset(temp);
				if (bss_enabled) {
					snprintf(temp, sizeof(temp), "wl%d.1_bss_enabled", i);
					nvram_set(temp, "1");
				}
			}

			snprintf(temp, sizeof(temp), "wl%d.1_net_reauth", i);
			val = nvram_get(temp);
			if (!val || (*val == 0))
				nvram_set(temp, nvram_default_get(temp));

			snprintf(temp, sizeof(temp), "wl%d.1_wpa_gtk_rekey", i);
			val = nvram_get(temp);
			if (!val || (*val == 0))
				nvram_set(temp, nvram_default_get(temp));
		}

		nvram_commit();

		cprintf("NVRAM upgrade complete.\n");
	}
}

#ifdef LINUX_2_6_36
/* Override the "0 5u" to "0 5" to backward compatible with old image */
static void
fa_override_vlan2ports()
{
	char port[] = "XXXX", *nvalue;
	char *next, *cur, *ports, *u;
	int len;

	ports = nvram_get("vlan2ports");
	nvalue = malloc(strlen(ports) + 2);
	if (!nvalue) {
		cprintf("Memory allocate failed!\n");
		return;
	}
	memset(nvalue, 0, strlen(ports) + 2);

	/* search last port include 'u' */
	for (cur = ports; cur; cur = next) {
		/* tokenize the port list */
		while (*cur == ' ')
			cur ++;
		next = strstr(cur, " ");
		len = next ? next - cur : strlen(cur);
		if (!len)
			break;
		if (len > sizeof(port) - 1)
			len = sizeof(port) - 1;
		strncpy(port, cur, len);
		port[len] = 0;

		/* prepare new value */
		if ((u = strchr(port, 'u')))
			*u = '\0';
		strcat(nvalue, port);
		strcat(nvalue, " ");
	}

	/* Remove last " " */
	len = strlen(nvalue);
	if (len) {
		nvalue[len-1] = '\0';
		nvram_set("vlan2ports", nvalue);
	}
	free(nvalue);
}

static void
fa_nvram_adjust()
{
	FILE *fp;
	int fa_mode;
	bool reboot = FALSE;

	if (RESTORE_DEFAULTS())
		return;

	fa_mode = atoi(nvram_safe_get("ctf_fa_mode"));
	switch (fa_mode) {
		case CTF_FA_BYPASS:
		case CTF_FA_NORMAL:
			break;
		default:
			fa_mode = CTF_FA_DISABLED;
			break;
	}

	if ((fp = fopen("/proc/fa", "r"))) {
		/* FA is capable */
		fclose(fp);

		if (FA_ON(fa_mode)) {
			char wan_ifnames[128];
			char wan_ifname[32], *next;
			int len, ret;

			cprintf("\nFA on.\n");

			/* Set et2macaddr, et2phyaddr as same as et0macaddr, et0phyaddr */
			if (!nvram_get("vlan2ports") || !nvram_get("wandevs"))  {
				cprintf("Insufficient envram, cannot do FA override\n");
				return;
			}

			/* adjusted */
			if (!strcmp(nvram_get("wandevs"), "vlan2") &&
			    !strchr(nvram_get("vlan2ports"), 'u'))
			    return;

			/* The vlan2ports will be change to "0 8u" dynamically by
			 * robo_fa_imp_port_upd. Keep nvram vlan2ports unchange.
			 */
			fa_override_vlan2ports();

			/* Override wandevs to "vlan2" */
			nvram_set("wandevs", "vlan2");
			/* build wan i/f list based on def nvram variable "wandevs" */
			len = sizeof(wan_ifnames);
			ret = build_ifnames("vlan2", wan_ifnames, &len);
			if (!ret && len) {
				/* automatically configure wan0_ too */
				nvram_set("wan_ifnames", wan_ifnames);
				nvram_set("wan0_ifnames", wan_ifnames);
				foreach(wan_ifname, wan_ifnames, next) {
					nvram_set("wan_ifname", wan_ifname);
					nvram_set("wan0_ifname", wan_ifname);
					break;
				}
			}
			cprintf("Override FA nvram...\n");
			reboot = TRUE;
		}
		else {
			cprintf("\nFA off.\n");
		}
	}
	else {
		/* FA is not capable */
		if (FA_ON(fa_mode)) {
			nvram_unset("ctf_fa_mode");
			cprintf("FA not supported...\n");
			reboot = TRUE;
		}
	}

	if (reboot) {
		nvram_commit();
		cprintf("FA nvram overridden, rebooting...\n");
		kill(1, SIGTERM);
	}
}

#define GMAC3_ENVRAM_BACKUP(name)				\
do {								\
	char *var, bvar[NVRAM_MAX_PARAM_LEN];			\
	if ((var = nvram_get(name)) != NULL) {			\
		snprintf(bvar, sizeof(bvar), "old_%s", name);	\
		nvram_set(bvar, var);				\
	}							\
} while (0)
/* Override GAMC3 nvram */
static void
gmac3_override_nvram()
{
	char var[32], *lists, *next;
	char newlists[NVRAM_MAX_PARAM_LEN];

	/* back up old embedded nvram */
	GMAC3_ENVRAM_BACKUP("et0macaddr");
	GMAC3_ENVRAM_BACKUP("et1macaddr");
	GMAC3_ENVRAM_BACKUP("et2macaddr");
	GMAC3_ENVRAM_BACKUP("et0mdcport");
	GMAC3_ENVRAM_BACKUP("et1mdcport");
	GMAC3_ENVRAM_BACKUP("et2mdcport");
	GMAC3_ENVRAM_BACKUP("et0phyaddr");
	GMAC3_ENVRAM_BACKUP("et1phyaddr");
	GMAC3_ENVRAM_BACKUP("et2phyaddr");
	GMAC3_ENVRAM_BACKUP("vlan1ports");
	GMAC3_ENVRAM_BACKUP("vlan2ports");
	GMAC3_ENVRAM_BACKUP("vlan1hwname");
	GMAC3_ENVRAM_BACKUP("vlan2hwname");
	GMAC3_ENVRAM_BACKUP("wandevs");

	/* change mac, mdcport, phyaddr */
	nvram_set("et2macaddr", nvram_get("et0macaddr"));
	nvram_set("et2mdcport", nvram_get("et0mdcport"));
	nvram_set("et2phyaddr", nvram_get("et0phyaddr"));
	nvram_set("et1mdcport", nvram_get("et0mdcport"));
	nvram_set("et1phyaddr", nvram_get("et0phyaddr"));
	nvram_set("et0macaddr", "00:00:00:00:00:00");
	nvram_set("et1macaddr", "00:00:00:00:00:00");

	/* change vlan ports */
	if (!(lists = nvram_get("vlan1ports"))) {
		cprintf("Default vlan1ports is not specified, override GMAC3 defaults...\n");
		nvram_set("vlan1ports", "1 2 3 4 5 7 8*");
	} else {
		strncpy(newlists, lists, sizeof(newlists));
		newlists[sizeof(newlists)-1] = '\0';

		/* search first port include '*' or 'u' and remove it */
		foreach(var, lists, next) {
			if (strchr(var, '*') || strchr(var, 'u')) {
				remove_from_list(var, newlists, sizeof(newlists));
				break;
			}
		}

		/* add port 5, 7 and 8* */
		add_to_list("5", newlists, sizeof(newlists));
		add_to_list("7", newlists, sizeof(newlists));
		add_to_list("8*", newlists, sizeof(newlists));
		nvram_set("vlan1ports", newlists);
	}

	if (!(lists = nvram_get("vlan2ports"))) {
		cprintf("Default vlan2ports is not specified, override GMAC3 defaults...\n");
		nvram_set("vlan2ports", "0 8u");
	} else {
		strncpy(newlists, lists, sizeof(newlists));
		newlists[sizeof(newlists)-1] = '\0';

		/* search first port include '*' or 'u' and remove it */
		foreach(var, lists, next) {
			if (strchr(var, '*') || strchr(var, 'u')) {
				remove_from_list(var, newlists, sizeof(newlists));
				break;
			}
		}

		/* add port 8u */
		add_to_list("8u", newlists, sizeof(newlists));
		nvram_set("vlan2ports", newlists);
	}

	/* change wandevs vlan hw name */
	nvram_set("wandevs", "et2");
	nvram_set("vlan1hwname", "et2");
	nvram_set("vlan2hwname", "et2");

	/* landevs should be have wl1 wl2 */

	/* set fwd_wlandevs from lan_ifnames */
	if (!(lists = nvram_get("lan_ifnames"))) {
		/* should not be happened */
		cprintf("lan_ifnames is not exist, override GMAC3 defaults...\n");
		nvram_set("fwd_wlandevs", "eth1 eth2 eth3");
	} else {
		strncpy(newlists, lists, sizeof(newlists));
		newlists[sizeof(newlists)-1] = '\0';

		/* remove ifname if it's not a wireless interrface */
		foreach(var, lists, next) {
			if (wl_probe(var)) {
				remove_from_list(var, newlists, sizeof(newlists));
				continue;
			}
		}
		nvram_set("fwd_wlandevs", newlists);
	}

	/* set fwddevs */
	nvram_set("fwddevs", "fwd0 fwd1");
}

#define GMAC3_ENVRAM_RESTORE(name)				\
do {								\
	char *var, bvar[NVRAM_MAX_PARAM_LEN];			\
	snprintf(bvar, sizeof(bvar), "old_%s", name);		\
	if ((var = nvram_get(bvar))) {				\
		nvram_set(name, var);				\
		nvram_unset(bvar);				\
	}							\
	else {							\
		nvram_unset(name);				\
	}							\
} while (0)

static void
gmac3_restore_nvram()
{
	/* back up old embedded nvram */
	GMAC3_ENVRAM_RESTORE("et0macaddr");
	GMAC3_ENVRAM_RESTORE("et1macaddr");
	GMAC3_ENVRAM_RESTORE("et2macaddr");
	GMAC3_ENVRAM_RESTORE("et0mdcport");
	GMAC3_ENVRAM_RESTORE("et1mdcport");
	GMAC3_ENVRAM_RESTORE("et2mdcport");
	GMAC3_ENVRAM_RESTORE("et0phyaddr");
	GMAC3_ENVRAM_RESTORE("et1phyaddr");
	GMAC3_ENVRAM_RESTORE("et2phyaddr");
	GMAC3_ENVRAM_RESTORE("vlan1ports");
	GMAC3_ENVRAM_RESTORE("vlan2ports");
	GMAC3_ENVRAM_RESTORE("vlan1hwname");
	GMAC3_ENVRAM_RESTORE("vlan2hwname");
	GMAC3_ENVRAM_RESTORE("wandevs");

	nvram_unset("fwd_wlandevs");
	nvram_unset("fwddevs");
}

static void
gmac3_restore_defaults()
{
	/* nvram variables will be changed when gmac3_enable */
	if (!strcmp(nvram_safe_get("wandevs"), "et2") &&
	    nvram_get("fwd_wlandevs") && nvram_get("fwddevs")) {
		gmac3_restore_nvram();

		/* Handle the case of user disable GMAC3 and do restore defaults. */
		if (nvram_invmatch("gmac3_enable", "1"))
			nvram_set("gmac3_reboot", "1");
	}
}

static void
gmac3_nvram_adjust()
{
	int fa_mode;
	bool reboot = FALSE;
	bool gmac3_enable, gmac3_configured = FALSE;

	/* gmac3_enable nvram control everything */
	gmac3_enable = nvram_match("gmac3_enable", "1") ? TRUE : FALSE;

	/* nvram variables will be changed when gmac3_enable */
	if (!strcmp(nvram_safe_get("wandevs"), "et2") &&
	    nvram_get("fwd_wlandevs") &&
	    nvram_get("fwddevs"))
		gmac3_configured = TRUE;

	fa_mode = atoi(nvram_safe_get("ctf_fa_mode"));
	if (fa_mode == CTF_FA_NORMAL || fa_mode == CTF_FA_BYPASS) {
		cprintf("GMAC3 based forwarding not compatible with ETFA - AuX port...\n");
		return;
	}

	if (et_capable(NULL, "gmac3")) {
		if (gmac3_enable) {
			cprintf("\nGMAC3 on.\n");

			if (gmac3_configured)
				return;

			/* The vlan2ports will be change to "0 8u" dynamically by
			 * robo_fa_imp_port_upd. Keep nvram vlan2ports unchange.
			 */
			gmac3_override_nvram();

			cprintf("Override GMAC3 nvram...\n");
			reboot = TRUE;
		} else {
			cprintf("\nGMAC3 off.\n");
			if (gmac3_configured) {
				gmac3_restore_nvram();
				reboot = TRUE;
			}
		}
	} else {
		cprintf("GMAC3 not supported...\n");

		if (gmac3_enable) {
			nvram_unset("gmac3_enable");
			reboot = TRUE;
		}

		/* GMAC3 is not capable */
		if (!gmac3_configured) {
			gmac3_restore_nvram();
			reboot = TRUE;
		}
	}

	if (nvram_get("gmac3_reboot")) {
		nvram_unset("gmac3_reboot");
      nvram_commit();
		reboot = TRUE;
	}

	if (reboot) {
		nvram_commit();
		cprintf("GMAC3 nvram overridden, rebooting...\n");
		kill(1, SIGTERM);
	}
}
#endif /* LINUX_2_6_36 */

#ifdef __CONFIG_FAILSAFE_UPGRADE_SUPPORT__
static void
failsafe_nvram_adjust(void)
{
	FILE *fp;
	char dev[PATH_MAX];
	bool found = FALSE, need_commit = FALSE;
	int i, partialboots;

	partialboots = atoi(nvram_safe_get(PARTIALBOOTS));
	if (partialboots != 0) {
		nvram_set(PARTIALBOOTS, "0");
		need_commit = TRUE;
	}

	if (nvram_get(BOOTPARTITION) != NULL) {
		/* get mtdblock device */
		if (!(fp = fopen("/proc/mtd", "r"))) {
			cprintf("Can't open /proc/mtd\n");
		} else {
			while (fgets(dev, sizeof(dev), fp)) {
				if (sscanf(dev, "mtd%d:", &i) && strstr(dev, LINUX_SECOND)) {
					found = TRUE;
					break;
				}
			}
			fclose(fp);

			/* The BOOTPARTITON was set, but linux2 partition can't be created.
			 * So unset BOOTPARTITION.
			 */
			if (found == FALSE) {
				cprintf("Unset bootpartition due to no linux2 partition found.\n");
				nvram_unset(BOOTPARTITION);
				need_commit = TRUE;
			}
		}
	}

	if (need_commit == TRUE)
		nvram_commit();
}
#endif /* __CONFIG_FAILSAFE_UPGRADE_SUPPORT__ */
restore_defaults(void)
{
#if 0 /* foxconn wklin removed start, 10/22/2008 */
	struct nvram_tuple generic[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "eth0 eth2 eth3 eth4", 0 },
		{ "wan_ifname", "eth1", 0 },
		{ "wan_ifnames", "eth1", 0 },
		{ "lan1_ifname", "br1", 0 },
		{ "lan1_ifnames", "wl0.1 wl0.2 wl0.3 wl1.1 wl1.2 wl1.3", 0 },
		{ 0, 0, 0 }
	};
#ifdef __CONFIG_VLAN__
	struct nvram_tuple vlan[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "vlan0 eth1 eth2 eth3", 0 },
		{ "wan_ifname", "vlan1", 0 },
		{ "wan_ifnames", "vlan1", 0 },
		{ "lan1_ifname", "br1", 0 },
		{ "lan1_ifnames", "wl0.1 wl0.2 wl0.3 wl1.1 wl1.2 wl1.3", 0 },
		{ 0, 0, 0 }
	};
#endif	/* __CONFIG_VLAN__ */
	struct nvram_tuple dyna[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "", 0 },
		{ "wan_ifname", "", 0 },
		{ "wan_ifnames", "", 0 },
		{ "lan1_ifname", "br1", 0 },
		{ "lan1_ifnames", "wl0.1 wl0.2 wl0.3 wl1.1 wl1.2 wl1.3", 0 },
		{ 0, 0, 0 }
	};
#endif /* 0 */ /* foxconn wklin removed end, 10/22/2008 */

	/* struct nvram_tuple *linux_overrides; *//* foxconn wklin removed, 10/22/2008 */
	struct nvram_tuple *t, *u;
	int restore_defaults, i;
#ifdef __CONFIG_VLAN__
	uint boardflags;
#endif	/* __CONFIG_VLAN_ */ 
    /* foxconn  wklin removed start, 10/22/2008*/
    /*
	char *landevs, *wandevs;
	char lan_ifnames[128], wan_ifnames[128];
	char wan_ifname[32], *next;
	int len;
	int ap = 0;
    */
    /* foxconn wklin removed end, 10/22/2008 */
#ifdef TRAFFIC_MGMT
	int j;
#endif  /* TRAFFIC_MGMT */

	/* Restore defaults if told to or OS has changed */
	restore_defaults = RESTORE_DEFAULTS();

	if (restore_defaults)
		cprintf("Restoring defaults...");

	/* Delete dynamically generated variables */
	if (restore_defaults) {
		char tmp[100], prefix[] = "wlXXXXXXXXXX_";
		for (i = 0; i < MAX_NVPARSE; i++) {
#ifdef __CONFIG_NAT__
			del_filter_client(i);
			del_forward_port(i);
#if !defined(AUTOFW_PORT_DEPRECATED)
			del_autofw_port(i);
#endif
#endif	/* __CONFIG_NAT__ */
			snprintf(prefix, sizeof(prefix), "wl%d_", i);
			for (t = router_defaults; t->name; t ++) {
				if (!strncmp(t->name, "wl_", 3))
					nvram_unset(strcat_r(prefix, &t->name[3], tmp));
			}
#ifdef __CONFIG_NAT__
			snprintf(prefix, sizeof(prefix), "wan%d_", i);
			for (t = router_defaults; t->name; t ++) {
				if (!strncmp(t->name, "wan_", 4))
					nvram_unset(strcat_r(prefix, &t->name[4], tmp));
			}
#endif	/* __CONFIG_NAT__ */

#ifdef TRAFFIC_MGMT
			/* Delete dynamically generated NVRAMs for Traffic Mgmt & DWM */
			snprintf(prefix, sizeof(prefix), "wl%d_", i);
			for (j = 0; j < MAX_NUM_TRF_MGMT_RULES; j++) {
				del_trf_mgmt_port(prefix, j);
			}
			for (j = 0; j < MAX_NUM_TRF_MGMT_DWM_RULES; j++) {
				del_trf_mgmt_dwm(prefix, j);
			}
#endif  /* TRAFFIC_MGMT */
#ifdef __CONFIG_DHDAP__
			/* Delete dynamically generated NVRAMs for DHDAP */
			snprintf(tmp, sizeof(tmp), "wl%d_cfg_maxassoc", i);
			nvram_unset(tmp);
#endif
		}

#if defined(LINUX_2_6_36) && defined(__CONFIG_TREND_IQOS__)
		iqos_restore_defaults();
#endif /* LINUX_2_6_36 && __CONFIG_TREND_IQOS__ */
#ifdef __CONFIG_WPS__
		wps_restore_defaults();
#endif /* __CONFIG_WSCCMD__ */
#ifdef __CONFIG_WAPI_IAS__
		nvram_unset("as_mode");
#endif /* __CONFIG_WAPI_IAS__ */

		virtual_radio_restore_defaults();
#ifdef __CONFIG_URE__
		if (nvram_match("wl0_ure_mode", "wre") ||
		    nvram_match("wl0_ure_mode", "ure")) {
			ure_restore_defaults(0);
		}
#endif /* __CONFIG_URE__ */
#ifdef LINUX_2_6_36
		/* Delete dynamically generated variables */
		//gmac3_restore_defaults();
#endif
	}

#if 0 /* foxconn removed start, wklin, 10/22/2008, we don't need this */
	/* 
	 * Build bridged i/f name list and wan i/f name list from lan device name list
	 * and wan device name list. Both lan device list "landevs" and wan device list
	 * "wandevs" must exist in order to preceed.
	 */
	if ((landevs = nvram_get("landevs")) && (wandevs = nvram_get("wandevs"))) {
		/* build bridged i/f list based on nvram variable "landevs" */
		len = sizeof(lan_ifnames);
		if (!build_ifnames(landevs, lan_ifnames, &len) && len)
			dyna[1].value = lan_ifnames;
		else
			goto canned_config;
		/* build wan i/f list based on nvram variable "wandevs" */
		len = sizeof(wan_ifnames);
		if (!build_ifnames(wandevs, wan_ifnames, &len) && len) {
			dyna[3].value = wan_ifnames;
			foreach(wan_ifname, wan_ifnames, next) {
				dyna[2].value = wan_ifname;
				break;
			}
		}
		else
			ap = 1;
		linux_overrides = dyna;
	}
	/* override lan i/f name list and wan i/f name list with default values */
	else {
canned_config:
#ifdef __CONFIG_VLAN__
		boardflags = strtoul(nvram_safe_get("boardflags"), NULL, 0);
		if (boardflags & BFL_ENETVLAN)
			linux_overrides = vlan;
		else
#endif	/* __CONFIG_VLAN__ */
			linux_overrides = generic;
	}

	/* Check if nvram version is set, but old */
	if (nvram_get("nvram_version")) {
		int old_ver, new_ver;

		old_ver = atoi(nvram_get("nvram_version"));
		new_ver = atoi(NVRAM_SOFTWARE_VERSION);
		if (old_ver < new_ver) {
			cprintf("NVRAM: Updating from %d to %d\n", old_ver, new_ver);
			nvram_set("nvram_version", NVRAM_SOFTWARE_VERSION);
		}
	}
#endif /* 0 */ /* foxconn removed end, wklin, 10/22/2008 */

	/* Restore defaults */
	for (t = router_defaults; t->name; t++) {
		if (restore_defaults || !nvram_get(t->name)) {
#if 0 /* foxconn removed, wklin, 10/22/2008 , no overrides */
			for (u = linux_overrides; u && u->name; u++) {
				if (!strcmp(t->name, u->name)) {
					nvram_set(u->name, u->value);
					break;
				}
			}
			if (!u || !u->name)
#endif
			nvram_set(t->name, t->value);
			//if(nvram_get(t->name))
			//	cprintf("%s:%s\n", t->name, nvram_get(t->name));
		}
	}


	/* Force to AP */
#if 0 /* foxconn wklin removed, 10/22/2008 */
	if (ap)
		nvram_set("router_disable", "1");
#endif

	/* Always set OS defaults */
	nvram_set("os_name", "linux");
	nvram_set("os_version", ROUTER_VERSION_STR);
	nvram_set("os_date", __DATE__);
	/* Always set WL driver version! */
	nvram_set("wl_version", EPI_VERSION_STR);

	nvram_set("is_modified", "0");
	nvram_set("ezc_version", EZC_VERSION_STR);

	if (restore_defaults) {
	    
	    /* Foxconn removed start, Tony W.Y. Wang, 04/06/2010 */
#if 0
	    /* Foxconn add start by aspen Bai, 02/12/2009 */
		nvram_unset("pa2gw0a0");
		nvram_unset("pa2gw1a0");
		nvram_unset("pa2gw2a0");
		nvram_unset("pa2gw0a1");
		nvram_unset("pa2gw1a1");
		nvram_unset("pa2gw2a1");
#ifdef FW_VERSION_NA
		acosNvramConfig_setPAParam(0);
#else
		acosNvramConfig_setPAParam(1);
#endif
		/* Foxconn add end by aspen Bai, 02/12/2009 */
#endif
		/* Foxconn removed end, Tony W.Y. Wang, 04/06/2010 */
		
		/* foxconn modified start, zacker, 08/06/2010 */
		/* Create a new value to inform loaddefault in "read_bd" */
		nvram_set("load_defaults", "1");
        eval("read_bd"); /* foxconn wklin added, 10/22/2008 */
		/* finished "read_bd", unset load_defaults flag */
		nvram_unset("load_defaults");
		/* foxconn modified end, zacker, 08/06/2010 */
        /* Foxconn add start, Tony W.Y. Wang, 04/06/2010 */
#ifdef SINGLE_FIRMWARE
        if (nvram_match("sku_name", "NA"))
            acosNvramConfig_setPAParam(0);
        else
            acosNvramConfig_setPAParam(1);
#else
		#ifdef FW_VERSION_NA
			acosNvramConfig_setPAParam(0);
		#else
			acosNvramConfig_setPAParam(1);
		#endif
#endif
        /* Foxconn add end, Tony W.Y. Wang, 04/06/2010 */
	}
#ifdef __CONFIG_URE__
	if (restore_defaults) {
		if (nvram_match("wl0_ure_mode", "ure") ||
		    nvram_match("wl0_ure_mode", "wre")) {
			nvram_set("ure_disable", "0");
			nvram_set("router_disable", "1");
			/* Set ure related nvram settings */
			set_ure_vars(0);
		}
	}
#endif /* __CONFIG_URE__ */

#ifdef __CONFIG_PLC__
	/* plc_pconfig_state = 3 will trigger a plc restore_to_defaults on
	 * plcnvm daemon. plcnvm will unset it.
	 */
	if (restore_defaults) {
		nvram_set("plc_pconfig_state", "0");
	}
#endif /* __CONFIG_PLC__ */

	/* Commit values */
	if (restore_defaults) {
		nvram_commit();
		sync();         /* Foxconn added start pling 12/25/2006 */
		cprintf("done\n");
	}
}

#ifdef __CONFIG_NAT__
static void
set_wan0_vars(void)
{
	int unit;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";

	/* check if there are any connections configured */
	for (unit = 0; unit < MAX_NVPARSE; unit ++) {
		snprintf(prefix, sizeof(prefix), "wan%d_", unit);
		if (nvram_get(strcat_r(prefix, "unit", tmp)))
			break;
	}
	/* automatically configure wan0_ if no connections found */
	if (unit >= MAX_NVPARSE) {
		struct nvram_tuple *t;
		char *v;

		/* Write through to wan0_ variable set */
		snprintf(prefix, sizeof(prefix), "wan%d_", 0);
		for (t = router_defaults; t->name; t ++) {
			if (!strncmp(t->name, "wan_", 4)) {
				if (nvram_get(strcat_r(prefix, &t->name[4], tmp)))
					continue;
				v = nvram_get(t->name);
				nvram_set(tmp, v ? v : t->value);
			}
		}
		nvram_set(strcat_r(prefix, "unit", tmp), "0");
		nvram_set(strcat_r(prefix, "desc", tmp), "Default Connection");
		nvram_set(strcat_r(prefix, "primary", tmp), "1");
	}
}
#endif	/* __CONFIG_NAT__ */

#if defined(LINUX26)
#if defined(__CONFIG_NAND_JFFS2__) || defined(__CONFIG_NAND_YAFFS2__)

#ifdef __CONFIG_NAND_JFFS2__
#define JFFS2_MAGIC	0x1985
static uint16
read_jffs2_magic()
{
	uint16 magic = 0;

#if defined(LINUX_2_6_36) && defined(__CONFIG_NAND_YAFFS2__)
	int fd = -1;
	int readbytes;

	/* get oob free areas data */
	if ((fd = open("/proc/brcmnand", O_RDONLY)) < 0)
		goto done;

	readbytes = read(fd, (char*)&magic, sizeof(magic));
	if (readbytes != sizeof(magic)) {
		fprintf(stderr, "Read brcmnand oob failed, "
			"want %d but got %d bytes\n", sizeof(magic), readbytes);
		goto done;
	}

done:
	if (fd >= 0)
		close(fd);
#else
	magic = JFFS2_MAGIC;
#endif /* LINUX_2_6_36  && __CONFIG_NAND_YAFFS2__ */

	return magic;
}
#endif /* __CONFIG_NAND_JFFS2__ */

static int
nand_fs_mtd_mount()
{
	FILE *fp;
	char *dpath = NULL, dev[PATH_MAX];
	int i, ret = -1;
	char *jpath = "/tmp/media/nand";

	/* get mtdblock device */
	if (!(fp = fopen("/proc/mtd", "r")))
		return -1;

	while (fgets(dev, sizeof(dev), fp)) {
		if (sscanf(dev, "mtd%d:", &i) && strstr(dev, "brcmnand")) {
			snprintf(dev, sizeof(dev), "/dev/mtdblock%d", i);
			dpath = dev;
			break;
		}
	}
	fclose(fp);

	/* check if we have brcmnand partition */
	if (dpath == NULL)
		return 0;

	/* create mount directory */
	if (mkdir(jpath, 0777) != 0 && errno != EEXIST)
		return -1;

#ifdef __CONFIG_NAND_JFFS2__
	if (read_jffs2_magic() == JFFS2_MAGIC) {
		if (mount(dpath, jpath, "jffs2", 0, NULL)) {
			fprintf(stderr, "Mount nflash MTD jffs2 partition %s to %s failed\n",
				dpath, jpath);
			goto erase_and_mount;
		}

		return 0;
	}
#endif /* __CONFIG_NAND_JFFS2__ */

#if defined(LINUX_2_6_36) && defined(__CONFIG_NAND_YAFFS2__)
	if (mount(dpath, jpath, "yaffs2", 0, NULL)) {
		fprintf(stderr, "Mount nflash MTD yaffs2 partition %s to %s failed\n",
			dpath, jpath);
		goto erase_and_mount;
	}

	return 0;
#endif

erase_and_mount:
	if ((ret = mtd_erase("brcmnand"))) {
		fprintf(stderr, "Erase nflash MTD partition %s failed %d\n", dpath, ret);
		return ret;
	}

	ret = -1;
#if defined(LINUX_2_6_36) && defined(__CONFIG_NAND_YAFFS2__)
	if ((ret = mount(dpath, jpath, "yaffs2", 0, NULL)) != 0)
		fprintf(stderr, "Mount nflash MTD yaffs2 partition %s to %s failed\n",
			dpath, jpath);
#endif
#if defined(__CONFIG_NAND_JFFS2__)
	if (ret != 0 && (ret = mount(dpath, jpath, "jffs2", 0, NULL)) != 0)
		fprintf(stderr, "Mount nflash MTD jffs2 partition %s to %s failed\n",
			dpath, jpath);
#endif

	return ret;
}
#endif /* __CONFIG_NAND_JFFS2__ || __CONFIG_NAND_YAFFS2__ */
#endif /* LINUX26 */

static int noconsole = 0;

static void
sysinit(void)
{
	char buf[PATH_MAX];
	struct utsname name;
	struct stat tmp_stat;
	time_t tm = 0;
	char *loglevel;

	struct utsname unamebuf;
	char *lx_rel;

	/* Use uname() to get the system's hostname */
	uname(&unamebuf);
	lx_rel = unamebuf.release;

	if (memcmp(lx_rel, "2.6", 3) == 0) {
		int fd;
		if ((fd = open("/dev/console", O_RDWR)) < 0) {
			if (memcmp(lx_rel, "2.6.36", 6) == 0) {
				mount("devfs", "/dev", "devtmpfs", MS_MGC_VAL, NULL);
			}
			else {
				mount("devfs", "/dev", "tmpfs", MS_MGC_VAL, NULL);
				if (mknod("/dev/console", S_IRWXU|S_IFCHR, makedev(5, 1)) < 0 &&
					errno != EEXIST) {
					perror("filesystem node /dev/console not created");
				}
			}
		}
		else {
			close(fd);
		}
	}

	/* /proc */
	mount("proc", "/proc", "proc", MS_MGC_VAL, NULL);
#ifdef LINUX26
	mount("sysfs", "/sys", "sysfs", MS_MGC_VAL, NULL);
#endif /* LINUX26 */

	/* /tmp */
	mount("ramfs", "/tmp", "ramfs", MS_MGC_VAL, NULL);

	/* /var */
	if (mkdir("/tmp/var", 0777) < 0 && errno != EEXIST) perror("/tmp/var not created");
	if (mkdir("/var/lock", 0777) < 0 && errno != EEXIST) perror("/var/lock not created");
	if (mkdir("/var/log", 0777) < 0 && errno != EEXIST) perror("/var/log not created");
	if (mkdir("/var/run", 0777) < 0 && errno != EEXIST) perror("/var/run not created");
	if (mkdir("/var/tmp", 0777) < 0 && errno != EEXIST) perror("/var/tmp not created");
	if (mkdir("/tmp/media", 0777) < 0 && errno != EEXIST) perror("/tmp/media not created");

#if 0
#ifdef __CONFIG_HSPOT__
	if (mkdir(RAMFS_CONFMTD_DIR, 0777) < 0 && errno != EEXIST) {
		perror("/tmp/confmtd not created.");
	}
	if (mkdir(RAMFS_CONFMTD_DIR"/hspot", 0777) < 0 && errno != EEXIST) {
		perror("/tmp/confmtd/hspot not created.");
	}
#endif /* __CONFIG_HSPOT__ */

#ifdef __CONFIG_DHDAP__
	if (mkdir(RAMFS_CONFMTD_DIR, 0777) < 0 && errno != EEXIST) {
		perror("/tmp/confmtd not created.");
	}
	if (mkdir(RAMFS_CONFMTD_DIR"/crash_logs", 0777) < 0 && errno != EEXIST) {
		perror("/tmp/confmtd/crash_logs not created.");
	}
#endif /* __CONFIG_DHDAP__ */


#if defined(LINUX26)
#if defined(__CONFIG_NAND_JFFS2__) || defined(__CONFIG_NAND_YAFFS2__)
	nand_fs_mtd_mount();
#endif /* __CONFIG_NAND_JFFS2__ || __CONFIG_NAND_YAFFS2__ */
#endif /* LINUX26 */
	confmtd_restore();
#endif
	/* Foxconn added start by Kathy, 10/14/2013 @ Facebook WiFi */
    mkdir("/tmp/fbwifi", 0777);
	/* Foxconn added end by Kathy, 10/14/2013 @ Facebook WiFi */

#ifdef __CONFIG_UTELNETD__
	/* If kernel enable unix908 pty then we have to make following things. */
	if (mkdir("/dev/pts", 0777) < 0 && errno != EEXIST) perror("/dev/pts not created");
	if (mount("devpts", "/dev/pts", "devpts", MS_MGC_VAL, NULL) == 0) {
		/* pty master */
		if (mknod("/dev/ptmx", S_IRWXU|S_IFCHR, makedev(5, 2)) < 0 && errno != EEXIST)
			perror("filesystem node /dev/ptmx not created");
	} else {
		rmdir("/dev/pts");
	}
#endif	/* LINUX2636 && __CONFIG_UTELNETD__ */

#ifdef __CONFIG_SAMBA__
	/* Add Samba Stuff */
	if (mkdir("/tmp/samba", 0777) < 0 && errno != EEXIST) {
		perror("/tmp/samba not created");
	}
	if (mkdir("/tmp/samba/lib", 0777) < 0 && errno != EEXIST) {
		perror("/tmp/samba/lib not created");
	}
	if (mkdir("/tmp/samba/private", 0777) < 0 && errno != EEXIST) {
		perror("/tmp/samba/private not created");
	}
	if (mkdir("/tmp/samba/var", 0777) < 0 && errno != EEXIST) {
		perror("/tmp/samba/var not created");
	}
	if (mkdir("/tmp/samba/var/locks", 0777) < 0 && errno != EEXIST) {
		perror("/tmp/samba/var/locks not created");
	}

#if defined(LINUX_2_6_36)
	/* To improve SAMBA upload performance */
	reclaim_mem_earlier();
#endif /* LINUX_2_6_36 */
#endif

#ifdef BCMQOS
	if (mkdir("/tmp/qos", 0777) < 0 && errno != EEXIST) perror("/tmp/qos not created");
#endif
	/* Setup console */
	if (console_init())
		noconsole = 1;

#ifdef LINUX26
	if (mkdir("/dev/shm", 0777) < 0 && errno != EEXIST) perror("/dev/shm not created");
	eval("/sbin/hotplug2", "--coldplug");
#endif /* LINUX26 */

	if ((loglevel = nvram_get("console_loglevel")))
		klogctl(8, NULL, atoi(loglevel));
	else
		klogctl(8, NULL, 1);

	/* Modules */
	uname(&name);
	snprintf(buf, sizeof(buf), "/lib/modules/%s", name.release);
	if (stat("/proc/modules", &tmp_stat) == 0 &&
	    stat(buf, &tmp_stat) == 0) {
		char module[80], *modules, *next;

		/* foxconn modified start, zacker, 08/06/2010 */
		/* Restore defaults if necessary */
  nvram_set ("wireless_restart", "1");
		restore_defaults();
		
		/* Foxconn Bob added start on 11/12/2014, force enable DFS */
		nvram_set("fcc_dfs_ch_enable", "0");
		nvram_set("ce_dfs_ch_enable", "1");
		nvram_set("telec_dfs_ch_enable", "1");
		/* Foxconn Bob added end on 11/12/2014, force enable DFS */


        /* For 4500 IR-159. by MJ. 2011.07.04  */
        /* Foxconn added start pling 02/11/2011 */
        /* WNDR4000 IR20: unset vifs NVRAM and let
         * bcm_wlan_util.c to reconstruct them if
         * necessary. move to here since they should be
         * done before read_bd */
        nvram_unset("wl0_vifs");
        nvram_unset("wl1_vifs");
        nvram_unset("wl2_vifs");
        /* Foxconn added end pling 02/11/2011 */

        /* Read ethernet MAC, RF params, etc */
		eval("read_bd");
        /* foxconn modified end, zacker, 08/06/2010 */

		/* Load ctf */

        /* Foxconn added start Bob 10/30/2014 */
        /* Make sure ctf_disable value is correct after dynamic enable/disable CTF function is introduced */
        if (nvram_match("enable_vlan", "1"))
            nvram_set("ctf_disable", "1");
        else
            nvram_set("ctf_disable", "0");
        /* Foxconn added end Bob 10/30/2014 */

        if (!nvram_match("ctf_disable", "1"))
            eval("insmod", "ctf");
#if defined(__CONFIG_WAPI__) || defined(__CONFIG_WAPI_IAS__)
		if (stat(WAPI_DIR, &tmp_stat) != 0) {
			if (mkdir(WAPI_DIR, 0777) < 0 && errno != EEXIST) {
				perror("WAPI_DIR not created");
			}
			if (mkdir(WAPI_WAI_DIR, 0777) < 0 && errno != EEXIST) {
				perror("WAPI_WAI_DIR not created");
			}
			if (mkdir(WAPI_AS_DIR, 0777) < 0 && errno != EEXIST) {
				perror("WAPI_AS_DIR not created");
			}
		}
#endif /* __CONFIG_WAPI__ || __CONFIG_WAPI_IAS__ */
#if defined(__CONFIG_CIFS__)
		if (stat(CIFS_DIR, &tmp_stat) != 0) {
			if (mkdir(CIFS_DIR, 0777) < 0 && errno != EEXIST) {
				perror("CIFS_DIR not created");
			}
			eval("cp", "/usr/sbin/cs_cfg.txt", CIFS_DIR);
			eval("cp", "/usr/sbin/cm_cfg.txt", CIFS_DIR);
			eval("cp", "/usr/sbin/pwd_list.txt", CIFS_DIR);
		}
#endif /* __CONFIG_CIFS__ */

/* #ifdef BCMVISTAROUTER */
#ifdef __CONFIG_IPV6__
		eval("insmod", "ipv6");
#endif /* __CONFIG_IPV6__ */
/* #endif */

#ifdef __CONFIG_EMF__
		/* Load the EMF & IGMP Snooper modules */
		load_emf();
#endif /*  __CONFIG_EMF__ */
#if 0
#if defined(__CONFIG_HSPOT__) || defined(__CONFIG_NPS__)
		eval("insmod", "proxyarp");
#endif /*  __CONFIG_HSPOT__ || __CONFIG_NPS__ */
#endif
    /* Bob added start to avoid sending unexpected dad, 09/16/2009 */
#ifdef INCLUDE_IPV6
		if (nvram_match("ipv6ready","1"))
		{
			system("echo 0 > /proc/sys/net/ipv6/conf/default/dad_transmits");
		}else{
		/* Foxconn added start pling 12/06/2010 */
		/* By default ipv6_spi is inserted to system to drop all packets. */
		/*Foxconn modify start by Hank for change ipv6_spi path in rootfs 08/27/2012*/
    
		if (nvram_match("enable_ap_mode","1"))
			system("/sbin/insmod /lib/modules/2.6.36.4brcmarm+/kernel/lib/ipv6_spi.ko working_mode=\"ap\"");
		else
			system("/sbin/insmod /lib/modules/2.6.36.4brcmarm+/kernel/lib/ipv6_spi.ko");
		/*Foxconn modify end by Hank for change ipv6_spi path in rootfs 08/27/2012*/
		/* Foxconn added end pling 12/06/2010 */
		}
#endif
    /* Bob added end to avoid sending unexpected dad, 09/16/2009 */
        
        
		/* Foxconn added start pling 09/02/2010 */
		/* Need to initialise switch related NVRAM before 
		 * insert ethernet module.
		 
		/* Load kernel modules. Make sure dpsta is loaded before wl
		 * due to symbol dependency.
		 */
#ifdef __CONFIG_GMAC3__
    

    if(!nvram_get("gmac3_enable"))
        nvram_set("gmac3_enable", "1");
    	    
    /* Foxconn added start pling 08/15/2017 */
    /* TD#545 add a check to turn off gmac3 if certain functions are enabled */
    if (nvram_match("enable_vlan", "enable") || nvram_match("iptv_enabled", "1"))
    {
        if (!nvram_match("gmac3_enable", "0"))
        {
            nvram_set("gmac3_enable", "0");
            //printf("### VLAN/IPTV enable! Force disable gmac3! ###\n");
        }
    }
    /* Foxconn added end pling 08/15/2017 */

    if((strlen(nvram_get("gmac3_enable"))==0) || (strcmp(nvram_get("gmac3_enable"),"1")==0))
    {
        nvram_set("wandevs", "et2");
        nvram_set("et0macaddr", "00:00:00:00:00:00");
        nvram_set("et0mdcport", "0");
        nvram_set("et0phyaddr", "30");


        nvram_set("et1macaddr", "00:00:00:00:00:00");
        nvram_set("et1mdcport", "0");
        nvram_set("et1phyaddr", "30");
        nvram_set("et2macaddr", acosNvramConfig_get("lan_hwaddr"));
        nvram_set("et2mdcport", "0");
        nvram_set("et2phyaddr", "30");

        nvram_set("vlan1hwname", "et2");
        nvram_set("vlan2hwname", "et2");

        nvram_set("landevs", "vlan1 wl0 wl1 wl2 wl0.1 wl1.1 wl2.1");
        nvram_set("fwd_cpumap", "d:x:2:169:1 d:l:5:169:1 d:u:5:163:0");
        nvram_set("fwd_wlandevs", "eth1 eth2 eth3");
        nvram_set("fwddevs", "fwd0 fwd1");
    }
    else
    {
        nvram_set("wandevs", "et0");
        nvram_set("et0macaddr", acosNvramConfig_get("lan_hwaddr"));
        nvram_set("et0mdcport", "0");
        nvram_set("et0phyaddr", "30");
        nvram_unset("et1macaddr");
        nvram_unset("et2macaddr");
      	nvram_unset("fwd_wlandevs");

  	    nvram_unset("fwddevs");
        nvram_set("vlan1hwname", "et0");
        nvram_set("vlan2hwname", "et0");

    }	
    nvram_set("lan_ifname", "br0");


#endif

#ifdef __CONFIG_IGMP_SNOOPING__
		config_switch();
		//if (nvram_match("enable_vlan", "enable")) 
				config_iptv_params();
#endif
		/* Foxconn added end pling 09/02/2010 */

        /* foxconn added start by Bob 12/12/2013, BRCM suggest not to enable rxchain power save */
        nvram_set("wl_rxchain_pwrsave_enable", "0");
        nvram_unset("wl0_rxchain_pwrsave_enable");
        nvram_unset("wl1_rxchain_pwrsave_enable");
        
		nvram_set("0:disband5grp","0x7");
		nvram_set("2:disband5grp","0x18");
        /* foxconn added end by Bob 12/12/2013, BRCM suggest not to enable rxchain power save */

        /* Foxconn Bob added start 01/26/2015, clear all FXCN defined DFS related nvram flag */
        nvram_unset("eth1_dfs_OOC");
        nvram_unset("eth2_dfs_OOC");
        nvram_unset("eth3_dfs_OOC");
        nvram_unset("eth1_dfs_detected");
        nvram_unset("eth2_dfs_detected");
        nvram_unset("eth3_dfs_detected");
        /* Foxconn Bob added end 01/26/2015, clear all FXCN defined DFS related nvram flag */
        
        /* Foxconn Bob added start 07/24/2015, force disable PMF to fix IOT issue with Nexus 5 */
        disable_mfp();
        /* Foxconn Bob added end 07/24/2015, force disable PMF to fix IOT issue with Nexus 5 */
        
        /* Foxconn Bob added start on 08/03/2015, workaround for dongle trap issue */
        nvram_set("0:cpuclk","800");
        nvram_set("1:cpuclk","800");
        nvram_set("2:cpuclk","800");
        /* Foxconn Bob added end on 08/03/2015, workaround for dongle trap issue */

        /* Foxconn Bob added start on 09/15/2015, correct WMM parameters */
        
        nvram_set("wl_wme_sta_be", "15 1023 3 0 0 off off");
        nvram_set("wl_wme_sta_bk", "15 1023 7 0 0 off off");
        nvram_set("wl_wme_sta_vi", "7 15 2 6016 3008 off off");
        nvram_set("wl_wme_sta_vo", "3 7 2 3264 1504 off off");
        nvram_set("wl_wme_ap_be", "15 63 3 0 0 off off");
        nvram_set("wl_wme_ap_bk", "15 1023 7 0 0 off off");
        nvram_set("wl_wme_ap_vi", "7 15 1 6016 3008 off off");
        nvram_set("wl_wme_ap_vo", "3 7 1 3264 1504 off off");
        
        nvram_set("wl0_wme_sta_be", "15 1023 3 0 0 off off");
        nvram_set("wl0_wme_sta_bk", "15 1023 7 0 0 off off");
        nvram_set("wl0_wme_sta_vi", "7 15 2 6016 3008 off off");
        nvram_set("wl0_wme_sta_vo", "3 7 2 3264 1504 off off");
        nvram_set("wl0_wme_ap_be", "15 63 3 0 0 off off");
        nvram_set("wl0_wme_ap_bk", "15 1023 7 0 0 off off");
        nvram_set("wl0_wme_ap_vi", "7 15 1 6016 3008 off off");
        nvram_set("wl0_wme_ap_vo", "3 7 1 3264 1504 off off");
        
        nvram_set("wl1_wme_sta_be", "15 1023 3 0 0 off off");
        nvram_set("wl1_wme_sta_bk", "15 1023 7 0 0 off off");
        nvram_set("wl1_wme_sta_vi", "7 15 2 6016 3008 off off");
        nvram_set("wl1_wme_sta_vo", "3 7 2 3264 1504 off off");
        nvram_set("wl1_wme_ap_be", "15 63 3 0 0 off off");
        nvram_set("wl1_wme_ap_bk", "15 1023 7 0 0 off off");
        nvram_set("wl1_wme_ap_vi", "7 15 1 6016 3008 off off");
        nvram_set("wl1_wme_ap_vo", "3 7 1 3264 1504 off off");
        
        nvram_set("wl2_wme_sta_be", "15 1023 3 0 0 off off");
        nvram_set("wl2_wme_sta_bk", "15 1023 7 0 0 off off");
        nvram_set("wl2_wme_sta_vi", "7 15 2 6016 3008 off off");
        nvram_set("wl2_wme_sta_vo", "3 7 2 3264 1504 off off");
        nvram_set("wl2_wme_ap_be", "15 63 3 0 0 off off");
        nvram_set("wl2_wme_ap_bk", "15 1023 7 0 0 off off");
        nvram_set("wl2_wme_ap_vi", "7 15 1 6016 3008 off off");
        nvram_set("wl2_wme_ap_vo", "3 7 1 3264 1504 off off");
        /* Foxconn Bob added end on 09/15/2015, correct WMM parameters */
        
        nvram_set("gbsd_wait_rssi_intf_idx", "2");  /* Foxconn Bob added on 10/14/2015 to force gbsd rssi interface to wl2 */
        
		//modules = nvram_get("kernel_mods") ? : "et bcm57xx wl";
		/*Foxconn modify start by Hank for insert dpsta 08/27/2012*/
		/*Foxconn modify start by Hank for insert proxyarp 10/05/2012*/
#if defined(__CONFIG_DHDAP__)
		modules = nvram_get("kernel_mods") ? : "et dpsta dhd";
#else
		modules = nvram_get("kernel_mods") ? : "et dpsta wl"; /* foxconn wklin modified, 10/22/2008 */
		/*Foxconn modify end by Hank for insert proxyarp 10/05/2012*/
		/*Foxconn modify end by Hank for insert dpsta 08/27/2012*/
#endif /* __CONFIG_DHDAP__ */
		foreach(module, modules, next){
            /*Foxconn, [MJ] for GPIO debugging. */
#ifdef WIFI_DISABLE
            if(strcmp(module, "wl")){
			    eval("insmod", module);
            }else
                cprintf("we don't insert wl.ko.\n");
#else
#ifdef __CONFIG_DHDAP__
			/* For DHD, additional module params have to be passed. */
			if ((strcmp(module, "dhd") == 0) || (strcmp(module, "wl") == 0)) {
				int	i = 0, maxwl_eth = 0, maxunit = -1;
				int	unit = -1;
				char ifname[16] = {0};
				char instance_base[128];

				/* Search for existing wl devices and the max unit number used */
				for (i = 1; i <= DEV_NUMIFS; i++) {
					snprintf(ifname, sizeof(ifname), "eth%d", i);
					if (!wl_probe(ifname)) {
						if (!wl_ioctl(ifname, WLC_GET_INSTANCE, &unit,
							sizeof(unit))) {
							maxwl_eth = i;
							maxunit = (unit > maxunit) ? unit : maxunit;
						}
					}
				}
				snprintf(instance_base, sizeof(instance_base), "instance_base=%d",
					maxunit + 1);

				eval("insmod", module, instance_base);
			} else
#endif /* __CONFIG_DHDAP__ */

            eval("insmod", module);
#endif
        }
#ifdef __CONFIG_USBAP__
		/* We have to load USB modules after loading PCI wl driver so
		 * USB driver can decide its instance number based on PCI wl
		 * instance numbers (in hotplug_usb())
		 */
		eval("insmod", "usbcore");

        /* Foxconn, [MJ] start, we can't insert usb-storage easiler than
         * automount being started. */
#if 0

		eval("insmod", "usb-storage");
        /* Foxconn, [MJ], for debugging. */
        cprintf("--> insmod usb-storage.\n");
#endif
        /* Foxconn, [MJ] end, we can't insert usb-storage easiler than
         * automount being started. */
		{
			char	insmod_arg[128];
			int	i = 0, maxwl_eth = 0, maxunit = -1;
			char	ifname[16] = {0};
			int	unit = -1;
			char arg1[20] = {0};
			char arg2[20] = {0};
			char arg3[20] = {0};
			char arg4[20] = {0};
			char arg5[20] = {0};
			char arg6[20] = {0};
			char arg7[20] = {0};
			const int wl_wait = 3;	/* max wait time for wl_high to up */

			/* Save QTD cache params in nvram */
			sprintf(arg1, "log2_irq_thresh=%d", atoi(nvram_safe_get("ehciirqt")));
			sprintf(arg2, "qtdc_pid=%d", atoi(nvram_safe_get("qtdc_pid")));
			sprintf(arg3, "qtdc_vid=%d", atoi(nvram_safe_get("qtdc_vid")));
			sprintf(arg4, "qtdc0_ep=%d", atoi(nvram_safe_get("qtdc0_ep")));
			sprintf(arg5, "qtdc0_sz=%d", atoi(nvram_safe_get("qtdc0_sz")));
			sprintf(arg6, "qtdc1_ep=%d", atoi(nvram_safe_get("qtdc1_ep")));
			sprintf(arg7, "qtdc1_sz=%d", atoi(nvram_safe_get("qtdc1_sz")));

			eval("insmod", "ehci-hcd", arg1, arg2, arg3, arg4, arg5,
				arg6, arg7);

			/* Search for existing PCI wl devices and the max unit number used.
			 * Note that PCI driver has to be loaded before USB hotplug event.
			 * This is enforced in rc.c
			 */
			for (i = 1; i <= DEV_NUMIFS; i++) {
				sprintf(ifname, "eth%d", i);
				if (!wl_probe(ifname)) {
					if (!wl_ioctl(ifname, WLC_GET_INSTANCE, &unit,
						sizeof(unit))) {
						maxwl_eth = i;
						maxunit = (unit > maxunit) ? unit : maxunit;
					}
				}
			}

			/* Set instance base (starting unit number) for USB device */
			sprintf(insmod_arg, "instance_base=%d", maxunit + 1);
            /*Foxconn, [MJ] for GPIO debugging. */
#ifndef WIFI_DISABLE
			eval("insmod", "wl_high", insmod_arg);
#endif
			/* Hold until the USB/HSIC interface is up (up to wl_wait sec) */
			sprintf(ifname, "eth%d", maxwl_eth + 1);
			i = wl_wait;
			while (wl_probe(ifname) && i--) {
				sleep(1);
			}
			if (!wl_ioctl(ifname, WLC_GET_INSTANCE, &unit, sizeof(unit)))
				cprintf("wl%d is up in %d sec\n", unit, wl_wait - i);
			else
				cprintf("wl%d not up in %d sec\n", unit, wl_wait);
		}
#ifdef LINUX26
		mount("usbdeffs", "/proc/bus/usb", "usbfs", MS_MGC_VAL, NULL);
#else
		mount("none", "/proc/bus/usb", "usbdevfs", MS_MGC_VAL, NULL);
#endif /* LINUX26 */
#endif /* __CONFIG_USBAP__ */

#ifdef __CONFIG_WCN__
		modules = "scsi_mod sd_mod usbcore usb-ohci usb-storage fat vfat msdos";
		foreach(module, modules, next){
            /* Foxconn, [MJ] for debugging. */
            cprintf("--> insmod %s\n", ,module);
			eval("insmod", module);
#endif

#ifdef __CONFIG_SOUND__
		modules = "soundcore snd snd-timer snd-page-alloc snd-pcm snd-pcm-oss "
		        "snd-soc-core i2c-core i2c-algo-bit i2c-gpio snd-soc-bcm947xx-i2s "
		        "snd-soc-bcm947xx-pcm snd-soc-wm8750 snd-soc-wm8955 snd-soc-bcm947xx";
		foreach(module, modules, next)
			eval("insmod", module);
		mknod("/dev/dsp", S_IRWXU|S_IFCHR, makedev(14, 3));
		if (mkdir("/dev/snd", 0777) < 0 && errno != EEXIST) perror("/dev/snd not created");
		mknod("/dev/snd/controlC0", S_IRWXU|S_IFCHR, makedev(116, 0));
		mknod("/dev/snd/pcmC0D0c", S_IRWXU|S_IFCHR, makedev(116, 24));
		mknod("/dev/snd/pcmC0D0p", S_IRWXU|S_IFCHR, makedev(116, 16));
		mknod("/dev/snd/timer", S_IRWXU|S_IFCHR, makedev(116, 33));
#endif

#ifdef LINUX_2_6_36
		/* To combat hotplug event lost because it could possibly happen before
		 * Rootfs is mounted or rc (preinit) is invoked during kernel boot-up with
		 * USB device attached.
		 */
		modules = "xhci-hcd ehci-hcd ohci-hcd";
		foreach(module, modules, next)
			eval("insmod", module);
#endif /* LINUX_2_6_36 */

#if defined(LINUX_2_6_36) && defined(__CONFIG_TREND_IQOS__)
		if(1){
			struct stat file_stat;
            /* contrack table turning for ACOSNAT start */
			/*
			system("echo 300 > /proc/sys/net/ipv4/netfilter/ip_conntrack_generic_timeout");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_icmp_timeout");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_close");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_close_wait");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_fin_wait");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_last_ack");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_syn_recv");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_syn_sent2");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_syn_sent");
			system("echo 60 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_time_wait");
			*/
			system("echo 300 > /proc/sys/net/ipv4/netfilter/ip_conntrack_udp_timeout");
			system("echo 300 > /proc/sys/net/ipv4/netfilter/ip_conntrack_udp_timeout_stream");
			system("echo 1800 > /proc/sys/net/ipv4/netfilter/ip_conntrack_tcp_timeout_established");
			system("echo 131072 > /proc/sys/net/ipv4/netfilter/ip_conntrack_max");
            /* contrack table turning for ACOSNAT end */
            nvram_set("brcm_speedtest",   "0");
            /* Foxconn modified start, Sinclair, 10/22/2015@ BRCM_GENERIC_IQOS */
#ifdef __BRCM_GENERIC_IQOS__
			if (stat("/usr/sbin/qos.conf", &file_stat) == 0) {
				if (mkdir(IQOS_RUNTIME_FOLDER, 0777) < 0 && errno != EEXIST)
    				perror("IQOS_RUNTIME_FOLDER not created");
				else {
					eval("cp", "/lib/modules/tdts.ko", IQOS_RUNTIME_FOLDER);
					eval("cp", "/lib/modules/tdts_udb.ko", IQOS_RUNTIME_FOLDER);
					eval("cp", "/lib/modules/tdts_udbfw.ko", IQOS_RUNTIME_FOLDER);

					eval("cp", "/usr/sbin/tdts_rule_agent", IQOS_RUNTIME_FOLDER);
					eval("cp", "/usr/sbin/rule.trf", IQOS_RUNTIME_FOLDER);
					eval("cp", "/usr/sbin/setup.sh", IQOS_RUNTIME_FOLDER);
					eval("cp", "/usr/sbin/upgrade.sh", IQOS_RUNTIME_FOLDER);
					eval("cp", "/usr/sbin/qos.sh", IQOS_RUNTIME_FOLDER);
					eval("cp", "/usr/sbin/qos.conf", IQOS_RUNTIME_FOLDER);
					eval("cp", "/usr/sbin/sample.bin", IQOS_RUNTIME_FOLDER);
					eval("cp", "/usr/sbin/TmToNtgr_dev_mapping", IQOS_RUNTIME_FOLDER);
				}
			}
#else  /* BRCM_GENERIC_IQOS */
			if (stat("/usr/sbin/qosd.conf", &file_stat) == 0) {
				if (mkdir("/tmp/trend", 0777) < 0 && errno != EEXIST)
				perror("/tmp/trend not created");
				else {
					eval("cp", "/lib/modules/IDP.ko", "/tmp/trend");
					eval("cp", "/lib/modules/bw_forward.ko", "/tmp/trend");
					eval("cp", "/lib/modules/tc_cmd.ko", "/tmp/trend");
					eval("cp", "/usr/sbin/bwdpi-rule-agent", "/tmp/trend");
					eval("cp", "/usr/sbin/rule.trf", "/tmp/trend");
					eval("cp", "/usr/sbin/setup.sh", "/tmp/trend");
					eval("cp", "/usr/sbin/upgrade.sh", "/tmp/trend");
					eval("cp", "/usr/sbin/qosd.conf", "/tmp/trend");
					eval("cp", "/usr/sbin/idpfw", "/tmp/trend");
					eval("cp", "/usr/sbin/tmdbg", "/tmp/trend");
					eval("cp", "/usr/sbin/TmToNtgr_dev_mapping", "/tmp/trend");
					eval("cp", "/usr/sbin/rule.version", "/tmp/trend");
				}
			}
#endif  /* BRCM_GENERIC_IQOS */
            /* Foxconn modified end, Sinclair, 10/22/2015@ BRCM_GENERIC_IQOS */
		}
#endif /* LINUX_2_6_36 && __CONFIG_TREND_IQOS__ */
	}

    /*foxconn Han edited start, 04/28/2015*/
    if(nvram_match("lacp","1"))
    {
        /*foxconn Han edited 07/24/2015, enable lacp debug when we capture log*/
        if(nvram_match("debug_lacp_enable","1"))
            nvram_set("lacpdebug","6"); /*foxconn Han edited, 12/03/2015 lower the debug level to 6 from 7*/
        else
            nvram_set("lacpdebug","0"); 
        eval("insmod", "lacp");
    }
    /*foxconn Han edited end, 04/28/2015*/
    
	system("/usr/sbin/et robowr 0x0 0x10 0x3000");  /*function 0*/
	system("/usr/sbin/et robowr 0x0 0x12 0x78");    /*function 1*/

	system("/usr/sbin/et robowr 0x0 0x14 0x01");    /* force port 0 to use LED function 1 */

    /*foxconn Han edited start, 05/12/2015 for 2nd switch control*/
    #ifdef CONFIG_2ND_SWITCH
    //if(nvram_match("hwver",AMBIT_PRODUCT_NAME_TRI_BAND))
    if(isTriBand())
    	system("/usr/sbin/et erobowr 0x0 0x14 0x00"); /*all mapping to LED function 0*/
    #endif /*CONFIG_2ND_SWITCH*/
    /*foxconn Han edited end, 05/12/2015 for 2nd switch control*/

	if (memcmp(lx_rel, "2.6.36", 6) == 0) {
		int fd;
		if ((fd = open("/proc/irq/163/smp_affinity", O_RDWR)) >= 0) {
			close(fd);

			if (nvram_match("gmac3_enable", "1")) {
				char *fwd_cpumap;

				/* Place network interface vlan1/eth0 on CPU hosting 5G upper */
				fwd_cpumap = nvram_get("fwd_cpumap");

				if (fwd_cpumap == NULL) {
					/* BCM4709acdcrh: Network interface GMAC on Core#1
					 *    [5G+2G:163 on Core#0] and [5G:169 on Core#1].
					 *    Bind et2:vlan1:eth0:181 to Core#1
					 *    Note, USB3 xhci_hcd's irq#112 binds Core#1
					 *    bind eth0:181 to Core#1 impacts USB3 performance
					 */
					system("echo 1 > /proc/irq/181/smp_affinity");

				} else {

					char cpumap[32], *next;

					foreach(cpumap, fwd_cpumap, next) {
						char mode, chan;
						int band, irq, cpu;

						/* Format: mode:chan:band#:irq#:cpu# */
						if (sscanf(cpumap, "%c:%c:%d:%d:%d",
						           &mode, &chan, &band, &irq, &cpu) != 5) {
							break;
						}
						if (cpu > 1) {
							break;
						}
						/* Find the single 5G upper */
						if ((chan == 'u') || (chan == 'U')) {
							char command[128];
							snprintf(command, sizeof(command),
							    "echo %d > /proc/irq/181/smp_affinity",
							    1 << cpu);
							system(command);
							break;
						}
					}
				}

			} else { /* ! gmac3_enable */

				if (!nvram_match("txworkq", "1")) {
					system("echo 2 > /proc/irq/163/smp_affinity");
					system("echo 2 > /proc/irq/169/smp_affinity");
				}
			}

			system("echo 2 > /proc/irq/112/smp_affinity");
		}
	}
	
	system("echo 20480 > /proc/sys/vm/min_free_kbytes");    /*Bob added on 09/05/2013, Set min free memory to 20Mbytes in case allocate memory failed */
	
	/* Set a sane date */
	stime(&tm);

	dprintf("done\n");
}

/* States */
enum {
	RESTART,
	STOP,
	START,
	TIMER,
	IDLE,
	WSC_RESTART,
	WLANRESTART, /* Foxconn added by EricHuang, 11/24/2006 */
	PPPSTART    /* Foxconn added by EricHuang, 01/09/2008 */
};
static int state = START;
static int signalled = -1;

/* foxconn added start, zacker, 05/20/2010, @spec_1.9 */
static int next_state = IDLE;

static int
next_signal(void)
{
	int tmp_sig = next_state;
	next_state = IDLE;
	return tmp_sig;
}
/* foxconn added end, zacker, 05/20/2010, @spec_1.9 */

/* Signal handling */
static void
rc_signal(int sig)
{
	if (state == IDLE) {	
		if (sig == SIGHUP) {
			dprintf("signalling RESTART\n");
			signalled = RESTART;
		}
		else if (sig == SIGUSR2) {
			dprintf("signalling START\n");
			signalled = START;
		}
		else if (sig == SIGINT) {
			dprintf("signalling STOP\n");
			signalled = STOP;
		}
		else if (sig == SIGALRM) {
			dprintf("signalling TIMER\n");
			signalled = TIMER;
		}
		else if (sig == SIGUSR1) {
			dprintf("signalling WSC RESTART\n");
			signalled = WSC_RESTART;
		}
		/* Foxconn modified start by EricHuang, 01/09/2008 */
		else if (sig == SIGQUIT) {
		    dprintf("signalling WLANRESTART\n");
		    signalled = WLANRESTART;
		}
		else if (sig == SIGILL) {
		    signalled = PPPSTART;
		}
		/* Foxconn modified end by EricHuang, 01/09/2008 */
	}
	/* foxconn added start, zacker, 05/20/2010, @spec_1.9 */
	else if (next_state == IDLE)
	{
		if (sig == SIGHUP) {
			dprintf("signalling RESTART\n");
			next_state = RESTART;
		}
		else if (sig == SIGUSR2) {
			dprintf("signalling START\n");
			next_state = START;
		}
		else if (sig == SIGINT) {
			dprintf("signalling STOP\n");
			next_state = STOP;
		}
		else if (sig == SIGALRM) {
			dprintf("signalling TIMER\n");
			next_state = TIMER;
		}
		else if (sig == SIGUSR1) {
			dprintf("signalling WSC RESTART\n");
			next_state = WSC_RESTART;
		}
		else if (sig == SIGQUIT) {
			printf("signalling WLANRESTART\n");
			next_state = WLANRESTART;
		}
		else if (sig == SIGILL) {
			next_state = PPPSTART;
		}
	}
	/* foxconn added end, zacker, 05/20/2010, @spec_1.9 */
}

/* Get the timezone from NVRAM and set the timezone in the kernel
 * and export the TZ variable
 */
static void
set_timezone(void)
{
	time_t now;
	struct tm gm, local;
	struct timezone tz;
	struct timeval *tvp = NULL;

	/* Export TZ variable for the time libraries to
	 * use.
	 */
	setenv("TZ", nvram_get("time_zone"), 1);

	/* Update kernel timezone */
	time(&now);
	gmtime_r(&now, &gm);
	localtime_r(&now, &local);
	tz.tz_minuteswest = (mktime(&gm) - mktime(&local)) / 60;
	settimeofday(tvp, &tz);

#if defined(__CONFIG_WAPI__) || defined(__CONFIG_WAPI_IAS__)
#ifndef	RC_BUILDTIME
#define	RC_BUILDTIME	1252636574
#endif
	{
		struct timeval tv = {RC_BUILDTIME, 0};

		time(&now);
		if (now < RC_BUILDTIME)
			settimeofday(&tv, &tz);
	}
#endif /* __CONFIG_WAPI__ || __CONFIG_WAPI_IAS__ */
}

/* Timer procedure.Gets time from the NTP servers once every timer interval
 * Interval specified by the NVRAM variable timer_interval
 */
int
do_timer(void)
{
	int interval = atoi(nvram_safe_get("timer_interval"));

	dprintf("%d\n", interval);

	if (interval == 0)
		return 0;

	/* Report stats */
	if (nvram_invmatch("stats_server", "")) {
		char *stats_argv[] = { "stats", nvram_get("stats_server"), NULL };
		_eval(stats_argv, NULL, 5, NULL);
	}

	/* Sync time */
	start_ntpc();

	alarm(interval);

	return 0;
}

/* Foxconn add start, Edward zhang, 09/14/2012, @add ARP PROTECTION support for RU SKU*/
#ifdef ARP_PROTECTION
static int getTokens(char *str, char *delimiter, char token[][C_MAX_TOKEN_SIZE], int maxNumToken)
{
    char temp[16*1024];    
    char *field;
    int numToken=0, i, j;
    char *ppLast = NULL;

    /* Check for empty string */
    if (str == NULL || str[0] == '\0')
        return 0;
   
    /* Now get the tokens */
    strcpy(temp, str);
    
    for (i=0; i<maxNumToken; i++)
    {
        if (i == 0)
            field = strtok_r(temp, delimiter, &ppLast);
        else 
            field = strtok_r(NULL, delimiter, &ppLast);

        /* Foxconn modified start, Wins, 06/27/2010 */
        //if (field == NULL || field[0] == '\0')
        if (field == NULL || (field != NULL && field[0] == '\0'))
        /* Foxconn modified end, Wins, 06/27/2010 */
        {
            for (j=i; j<maxNumToken; j++)
                token[j][0] = '\0';
            break;
        }

        numToken++;
        strcpy(token[i], field);
    }

    return numToken;
}

static int getReservedAddr(char reservedMacAddr[][C_MAX_TOKEN_SIZE], char reservedIpAddr[][C_MAX_TOKEN_SIZE])
/* Foxconn modified end, zacker, 10/31/2008, @lan_setup_change */
{
    int numReservedMac=0, numReservedIp=0;
    char *var;
    
    /* Read MAC and IP address tokens */
    if ( (var = acosNvramConfig_get("dhcp_resrv_mac")) != NULL )
    {
        numReservedMac = getTokens(var, " ", reservedMacAddr, C_MAX_RESERVED_IP);
    }
    
    if ( (var=acosNvramConfig_get("dhcp_resrv_ip")) != NULL )
    {
        numReservedIp = getTokens(var, " ", reservedIpAddr, C_MAX_RESERVED_IP);
    }
    
    if (numReservedMac != numReservedIp)
    {
        printf("getReservedAddr: reserved mac and ip not match\n");
    }
    
    return (numReservedMac<numReservedIp ? numReservedMac:numReservedIp);
}

static void config_arp_table(void)
{
    if(acosNvramConfig_match("arp_enable","enable"))
    {
        int i;
        char resrvMacAddr[C_MAX_RESERVED_IP][C_MAX_TOKEN_SIZE];
        char resrvIpAddr[C_MAX_RESERVED_IP][C_MAX_TOKEN_SIZE];
        int numResrvAddr = getReservedAddr(resrvMacAddr, resrvIpAddr);
        char arp_cmd[64];
        for (i=0; i<numResrvAddr; i++)
        {
            sprintf(arp_cmd,"arp -s %s %s",resrvIpAddr[i],resrvMacAddr[i]);
            printf("%s\n",arp_cmd);
            system(arp_cmd);
        }
    }
    
    return 0;
}
#endif
/* Foxconn add end, Edward zhang, 09/14/2012, @add ARP PROTECTION support for RU SKU*/

/*foxconn Han edited start, 10/01/2015 
 *when R8500 didn't recognize all 3 interface, then do the software reboot*/
int isDhdReady()
{
    char ifname[32] = "eth1";
    int flags;
    unsigned long addr, netmask;
    int i = 1;
    int ret = 0;
    int max = 3;
    int flag = 0;
    char *max_pt = NULL;

    if(acosNvramConfig_match("dhd_check_ignore","1"))
        return 0;

    if((max_pt = nvram_get("dhd_check_max"))!=NULL)
    {
        max = atoi(max_pt);
    }

    printf("\n--------------------isDhdReady()------------------------\n");
    //system("ifconfig -a");
    for(i=1; i<= max; i++)
    {
        sprintf(ifname,"eth%d",i);
        ret=ifconfig_get(ifname, &flags, &addr, &netmask);
        /*ENONET means interface don't have IP address,
         *EACCES means interface don't exist 
         *EADDRNOTAVAIL Cannot assign requested address*/
        if (ret != 0 && ret != EADDRNOTAVAIL)  
        {
            printf("%s %d could not found %s ret=0x%X\n",__func__,__LINE__,ifname,ret);
            flag ++;
        }
        else 
            printf("%s %d found %s ret=0x%X\n",__func__,__LINE__,ifname, ret);
    }
    printf("\n-------------------isDhdReady flag=%d-----------------------------\n",flag);

    if(flag)
    {
        printf("DHD didn't bring up all the interfaces!\n");
        system("reboot");
    }
    return ret;
}
/*foxconn Han edited end, 10/01/2015*/
/* Main loop */
static void
main_loop(void)
{
#ifdef CAPI_AP
	static bool start_aput = TRUE;
#endif
	sigset_t sigset;
	pid_t shell_pid = 0;
#if defined(DUAL_TRI_BAND_HW_SUPPORT)	
   char hwver[32]={"R7800"};
#endif
#ifdef __CONFIG_VLAN__
	uint boardflags;
#endif

    /* foxconn wklin added start, 10/22/2008 */
	sysinit();


    /* Foxconn Bob added start on 03/30/3015, add a nvram hwver to indicate hw verion */
    #if defined(DUAL_TRI_BAND_HW_SUPPORT)
	bd_read_hwver(hwver,sizeof(hwver));

    /*foxconn Han edited, 07/02/2015 add for hwver and hwrev, don't save too many words in hwver*/
    hwver[5] = 0;

	hwver[31]=0;
	nvram_set("hwver", hwver);
	#endif
	/* Foxconn Bob added end on 03/30/3015, add a nvram hwver to indicate hw verion */

	/* Foxconn added start pling 06/26/2014 */
	/* R8000 TD99, Link down/up WAN ethernet for Comcast modem IPv6 compatibility issue*/
    abDisableWanEthernetPort();
	/* Foxconn added end pling 06/26/2014 */

	/* Foxconn added start pling 03/20/2014 */
	/* Router Spec Rev 12: disable/enable ethernet interface when dhcp server start */
	eval("landown");
	/* Foxconn added end pling 03/20/2014 */

	/* Add loopback */
	config_loopback();
	/* Restore defaults if necessary */
	//restore_defaults(); /* foxconn removed, zacker, 08/06/2010, move to sysinit() */

	/* Convert deprecated variables */
	convert_deprecated();

	/* Upgrade NVRAM variables to MBSS mode */
	upgrade_defaults();

    /* Read ethernet MAC, etc */
    //eval("read_bd"); /* foxconn removed, zacker, 08/06/2010, move to sysinit() */
    /* foxconn wklin added end, 10/22/2008 */

    /* Reset some wps-related parameters */
    nvram_set("wps_start",   "none");
    /* foxconn added start, zacker, 05/20/2010, @spec_1.9 */
    nvram_set("wps_status", "0"); /* start_wps() */
    nvram_set("wps_proc_status", "0");
    /* foxconn added end, zacker, 05/20/2010, @spec_1.9 */
    
    /* Foxconn Perry added start, 2011/05/13, for IPv6 router advertisment prefix information */
    /* reset IPv6 obsolete prefix information after reboot */
    nvram_set("radvd_lan_obsolete_ipaddr", "");
    nvram_set("radvd_lan_obsolete_ipaddr_length", "");
    nvram_set("radvd_lan_new_ipaddr", "");
    nvram_set("radvd_lan_new_ipaddr_length", "");
    /* Foxconn Perry added end, 2011/05/13, for IPv6 router advertisment prefix information */

    /* Foxconn added start, zacker, 06/17/2010, @new_tmp_lock */
    /* do this in case "wps_aplockdown_forceon" is set to "1" for tmp_lock
     * purpose but then there are "nvram_commit" and "reboot" action
     */
    if (nvram_match("wsc_pin_disable", "1"))
        nvram_set("wps_aplockdown_forceon", "1");
    else
        nvram_set("wps_aplockdown_forceon", "0");
    /* Foxconn added end, zacker, 06/17/2010, @new_tmp_lock */

    /* Foxconn added start, Wins, 04/20/2011, @RU_IPTV */
#ifdef CONFIG_RUSSIA_IPTV
/* Foxconn modified, Edward zhang, 09/05/2012, @add IPTV support for PR SKU*/
#if 0
    if ((!is_russia_specific_support()) && (!is_china_specific_support()))
    {
        nvram_set(NVRAM_IPTV_ENABLED, "0");
        nvram_set(NVRAM_IPTV_INTF, "0x00");
    }
#endif
#endif /* CONFIG_RUSSIA_IPTV */
    /* Foxconn added end, Wins, 04/20/2011, @RU_IPTV */
/* Foxconn add start, Edward zhang, 09/14/2012, @add ARP PROTECTION support for RU SKU*/
    if ((!is_russia_specific_support()) && (!is_china_specific_support()))
    {
//        nvram_set(NVRAM_ARP_ENABLED, "disable");
    }
/* Foxconn add end, Edward zhang, 09/14/2012, @add ARP PROTECTION support for RU SKU*/
    /* Foxconn add start, Max Ding, 02/26/2010 */
#ifdef RESTART_ALL_PROCESSES
    nvram_unset("restart_all_processes");
#endif
    /* Foxconn add end, Max Ding, 02/26/2010 */
    eval("acos_init_once");
	/* Basic initialization */
	//sysinit();

	/* Setup signal handlers */
	signal_init();
	signal(SIGHUP, rc_signal);
	signal(SIGUSR2, rc_signal);
	signal(SIGINT, rc_signal);
	signal(SIGALRM, rc_signal);
	signal(SIGUSR1, rc_signal);	
	signal(SIGQUIT, rc_signal); /* Foxconn added by EricHuang, 11/24/2006 */
	signal(SIGILL, rc_signal); //ppp restart
	sigemptyset(&sigset);

	/* Give user a chance to run a shell before bringing up the rest of the system */
	if (!noconsole)
		run_shell(1, 0);

	/* Get boardflags to see if VLAN is supported */
#ifdef __CONFIG_VLAN__
	boardflags = strtoul(nvram_safe_get("boardflags"), NULL, 0);
#endif	/* __CONFIG_VLAN__ */


#if 0 /* foxconn modified, wklin 10/22/2008, move the the start of this function */
	/* Add loopback */
	config_loopback();

	/* Convert deprecated variables */
	convert_deprecated();



	/* Upgrade NVRAM variables to MBSS mode */
	upgrade_defaults();

	/* Restore defaults if necessary */
	restore_defaults();

    /* Foxconn added start pling 06/20/2007 */
    /* Read board data again, since the "restore_defaults" action
     * above will overwrite some of our settings */
    eval("read_bd");
    /* Foxconn added end pling 06/20/2006 */
#endif /* 0 */
#ifdef LINUX_2_6_36
	/* Ajuest FA NVRAM variables */
	//fa_nvram_adjust();

	/* Ajuest GMAC3 NVRAM variables */
//	gmac3_nvram_adjust();
#endif
    
#ifdef __CONFIG_NAT__
	/* Auto Bridge if neccessary */
	if (!strcmp(nvram_safe_get("auto_bridge"), "1"))
	{
		auto_bridge();
	}
	/* Setup wan0 variables if necessary */
	set_wan0_vars();
#endif	/* __CONFIG_NAT__ */
#if defined(WLTEST) && defined(RWL_SOCKET)
	/* Shorten TCP timeouts to prevent system from running slow with rwl */
	system("echo \"10 10 10 10 3 3 10 10 10 10\">/proc/sys/net/ipv4/ip_conntrack_tcp_timeouts");
#endif /* WL_TEST && RWL_SOCKET */
#ifdef __CONFIG_FAILSAFE_UPGRADE_SUPPORT__
	failsafe_nvram_adjust();
#endif

    /* Foxconn added start pling 07/13/2009 */
    /* create the USB semaphores */
#ifdef SAMBA_ENABLE
    usb_sem_init(); //[MJ] for 5G crash
#endif
    /* Foxconn added end pling 07/13/2009 */


	/* Loop forever */
	for (;;) {
		switch (state) {
		case RESTART:
			dprintf("RESTART\n");
			/* Fall through */
			/* Foxconn added start pling 06/14/2007 */
            /* When vista finished configuring this router (wl0_wps_config_state: 0->1),
             * then we come here to restart WLAN 
             */
            stop_wps();
			stop_nas();
            stop_eapd();
			stop_bcmupnp();
			stop_wlan();
				stop_bsd();

			/*Foxconn add start by Hank 06/14/2012*/
			/*Enable 2.4G auto channel detect, kill acsd for stop change channel*/
			//if((nvram_match("wla_channel", "0") || nvram_match("wlg_channel", "0")) && nvram_match("enable_sta_mode","0"))
			if(nvram_match("enable_sta_mode","0"))
				stop_acsd();
			/*Foxconn add end by Hank 06/14/2012*/

    	    convert_wlan_params();  /* For WCN , added by EricHuang, 12/21/2006 */
            sleep(2);               /* Wait some time for wsc, etc to terminate */

            /* if "unconfig" to "config" mode, force it to built-in registrar and proxy mode */
            /* added start by EricHuang, 11/04/2008 */
            if ( nvram_match("wps_status", "0") ) //restart wlan for wsc
            {
                nvram_set("lan_wps_reg", "enabled");
                nvram_set("wl_wps_reg", "enabled");
                nvram_set("wl0_wps_reg", "enabled");
#if (defined INCLUDE_DUAL_BAND)
                nvram_set("wl1_wps_reg", "enabled");
#if defined(R8000)
                nvram_set("wl2_wps_reg", "enabled");
#endif
#endif
                /* Foxconn modify start, Max Ding, 08/28/2010 for NEW_BCM_WPS */
                /* New NVRAM to BSP 5.22.83.0, 'wlx_wps_config_state' not used anymore. */
                //printf("restart -- wl0_wps_config_state=%s\n", nvram_get("wl0_wps_config_state"));
                //nvram_set("wl_wps_config_state", nvram_get("wl0_wps_config_state"));
                if ( nvram_match("lan_wps_oob", "enabled") )
                {
                    nvram_set("wl_wps_config_state", "0");
                    nvram_set("wl0_wps_config_state", "0");
#if (defined INCLUDE_DUAL_BAND)
                    nvram_set("wl1_wps_config_state", "0");
#if defined(R8000)
                    nvram_set("wl2_wps_config_state", "0");
#endif
#endif
                }
                else
                {
                    nvram_set("wl_wps_config_state", "1");
                    nvram_set("wl0_wps_config_state", "1");
#if (defined INCLUDE_DUAL_BAND)
                    nvram_set("wl1_wps_config_state", "1");
#if defined(R8000)
                    nvram_set("wl2_wps_config_state", "1");
#endif                    
#endif
                }
                /* Foxconn modify end, Max Ding, 08/28/2010 */
            }
            /* added end by EricHuang, 11/04/2008 */
            
            /* hide unnecessary warnings (Invaid XXX, out of range xxx etc...)*/
            {
                #include <fcntl.h>
                int fd1, fd2;
                fd1 = dup(2);
                fd2 = open("/dev/null", O_WRONLY);
                close(2);
                dup2(fd2, 2);
                close(fd2);
                start_wlan(); //<-- to hide messages generated here
                close(2);
                dup2(fd1, 2);
                close(fd1);
            }
            
            save_wlan_time();          
            start_bcmupnp();

            {
                nvram_unset("wps_randomssid");
                nvram_unset("wps_randomssid_5G");
                nvram_unset("wps_randomkey");
                nvram_unset("wps_randomkey_5G");
                nvram_set("wps_status", "0");
            }

            start_wps();            /* Foxconn modify by aspen Bai, 08/01/2008 */
            start_eapd();           /* Foxconn modify by aspen Bai, 10/08/2008 */
            start_nas();            /* Foxconn modify by aspen Bai, 08/01/2008 */
            if(nvram_match("enable_sta_mode","0"))
				start_acsd();
            sleep(2);               /* Wait for WSC to start */
            /* Foxconn add start by aspen Bai, 09/10/2008 */
            /* Must call it when start wireless */
            start_wl();
            /* Foxconn add end by aspen Bai, 09/10/2008 */
			/*Foxconn add start by Antony 06/16/2013 Start the bandsteering*/

    
      if((strcmp(nvram_safe_get("wla_ssid"),nvram_safe_get("wlg_ssid") )!=0))
          nvram_set("enable_band_steering", "0");      	

      if(strcmp(nvram_safe_get("wla_secu_type"),nvram_safe_get("wlg_secu_type") )!=0)
          nvram_set("enable_band_steering", "0");      	

      if(strcmp(nvram_safe_get("wla_secu_type"),"None") || strcmp(nvram_safe_get("wlg_secu_type"),"None"))
      {
          if(strcmp(nvram_safe_get("wla_passphrase"),nvram_safe_get("wlg_passphrase"))!=0) 
              nvram_set("enable_band_steering", "0");
      }
			if(nvram_match("enable_band_steering", "1") && nvram_match("wla_wlanstate", "Enable")&& nvram_match("wlg_wlanstate", "Enable"))
				start_bsd();
			/*Foxconn add end by Antony 06/16/2013*/
			if(nvram_match("wl_5g_bandsteering", "1") && nvram_match("wlg_wlanstate", "Enable")&& nvram_match("wlh_wlanstate", "Enable"))
				start_bsd();


			/*Foxconn add start by Hank 06/14/2012*/
			/*Enable 2.4G auto channel detect, call acsd to start change channel*/
			//if((nvram_match("wla_channel", "0") || nvram_match("wlg_channel", "0")) && nvram_match("enable_sta_mode","0"))
			
			/*Foxconn add end by Hank 06/14/2012*/
            nvram_commit();         /* Save WCN obtained parameters */

			/* foxconn modified start, zacker, 05/20/2010, @spec_1.9 */
			//state = IDLE;
			state = next_signal();
			/* foxconn modified end, zacker, 05/20/2010, @spec_1.9 */

#if 0       /* foxconn removed start, zacker, 09/17/2009, @wps_led */
#ifdef BCM4716
			if (nvram_match("wla_secu_type", "None"))
			{
				system("/sbin/gpio 7 0");
			}
			else
			{
				system("/sbin/gpio 7 1");
			}
#else
			if (nvram_match("wla_secu_type", "None"))
			{
				system("/sbin/gpio 1 0");
			}
			else
			{
				system("/sbin/gpio 1 1");
			}
#endif
#endif      /* foxconn removed end, zacker, 09/17/2009, @wps_led */
			
			break;
			/* Foxconn added end pling 06/14/2007 */

		case STOP:
			dprintf("STOP\n");
			pmon_init();
      if(nvram_match ("wireless_restart", "1"))
      {
            stop_wps();
            stop_nas();
            stop_eapd(); 
    				stop_bsd();
      }
      
      stop_bcmupnp();
			
			stop_lan();
#ifdef __CONFIG_VLAN__
			if (boardflags & BFL_ENETVLAN)
				stop_vlan();
#endif	/* __CONFIG_VLAN__ */
			if (state == STOP) {
				/* foxconn modified start, zacker, 05/20/2010, @spec_1.9 */
				//state = IDLE;
				state = next_signal();
				/* foxconn modified end, zacker, 05/20/2010, @spec_1.9 */
				break;
			}
			/* Fall through */
		case START:
			dprintf("START\n");
			pmon_init();
			/* foxconn added start, zacker, 01/13/2012, @iptv_igmp */
#ifdef CONFIG_RUSSIA_IPTV
			if (!nvram_match("wla_repeater", "1")
#if (defined INCLUDE_DUAL_BAND)
				&& !nvram_match("wlg_repeater", "1")
#endif
				)
			{
				/* don't do this in cgi since "rc stop" need to do cleanup */
				config_iptv_params();
				/* always do this to active new vlan settings */
				active_vlan();
			}
#endif /* CONFIG_RUSSIA_IPTV */

#if (defined INCLUDE_QOS) || (defined __CONFIG_IGMP_SNOOPING__)
            if (!nvram_match("gmac3_enable", "1"))
			    config_switch_reg();
#endif
			/* foxconn added end, zacker, 01/13/2012, @iptv_igmp */
			
			if ( nvram_match("debug_port_mirror", "1"))
            {
                system("et robowr 0x02 0x10 0x8000");
                system("et robowr 0x02 0x12 0x110");
                system("et robowr 0x02 0x1C 0x110");
            }
            
#if defined(R8000)
        system("et -i eth0 robowr 0x4 0x4 0");   /* Bob added on 07/17/2014, to enable STP forward */
		if ( nvram_match("wla_region", "5"))
		    nvram_set("dual_5g_band","1");
		else
		    nvram_set("dual_5g_band","1");
#if 0		    
		if(nvram_match("sku_name","PR"))
		{
//		    nvram_set("dual_5g_band","0");
		    nvram_set("dual_5g_band","1");
		    nvram_set("wla_region","16");
		}
#endif			
#endif
		/*foxconn added start, water, 12/21/09*/
#ifdef RESTART_ALL_PROCESSES
		if ( nvram_match("restart_all_processes", "1") )
		{
			restore_defaults();
			eval("read_bd");

            /* Foxconn Bob added start 07/24/2015, force disable PMF to fix IOT issue with Nexus 5 */
            disable_mfp();
            /* Foxconn Bob added end 07/24/2015, force disable PMF to fix IOT issue with Nexus 5 */

			convert_deprecated();
			/* Foxconn add start, Max Ding, 03/03/2010 */

#if (defined BCM5325E) || (defined BCM53125)
			system("/usr/sbin/et robowr 0x34 0x00 0x00e0");
#endif
			/* Foxconn add end, Max Ding, 03/03/2010 */
#if !defined(U12H245) && !defined(U12H264) && !defined(U12H268)
			if(acosNvramConfig_match("emf_enable", "1") )
			{
    			system("insmod emf");
    			system("insmod igs");
    			system("insmod wl");
			}
#endif			
		}
#endif

		/*foxconn added end, water, 12/21/09*/
			{ /* Set log level on restart */
				char *loglevel;
				int loglev = 8;

				if ((loglevel = nvram_get("console_loglevel"))) {
					loglev = atoi(loglevel);
				}
				klogctl(8, NULL, loglev);
				if (loglev < 7) {
					printf("WARNING: console log level set to %d\n", loglev);
				}
			}

			set_timezone();
#ifdef __CONFIG_VLAN__
			if (boardflags & BFL_ENETVLAN)
				start_vlan();
#endif	/* __CONFIG_VLAN__ */
            /* wklin modified start, 10/23/2008 */
            /* hide unnecessary warnings (Invaid XXX, out of range xxx etc...)*/
            {
                #include <fcntl.h>
                int fd1, fd2;
                fd1 = dup(2);
                fd2 = open("/dev/null", O_WRONLY);
                close(2);
                dup2(fd2, 2);
                close(fd2);
                start_lan(); //<-- to hide messages generated here
      if(nvram_match ("wireless_restart", "1"))
                start_wlan(); //<-- need it to bring up 5G interface
                close(2);
                dup2(fd1, 2);
                close(fd1);
            }
            if (nvram_match("wla_repeater", "1")
#if (defined INCLUDE_DUAL_BAND)
            || nvram_match("wlg_repeater", "1")
#endif
            )
            {
                /* if repeater mode, del vlan1 from br0 and disable vlan */
#ifdef BCM4716
                system("/usr/sbin/brctl delif br0 vlan0");
                system("/usr/sbin/et robowr 0x34 0x00 0x00");
#else
                /*foxconn modified start, water, 01/07/10, @lan pc ping DUT failed when repeater mode & igmp enabled*/
                //system("/usr/sbin/brctl delif br0 vlan1");
                //system("/usr/sbin/et robowr 0x34 0x00 0x00");
#ifdef IGMP_PROXY
                if (!nvram_match("igmp_proxying_enable", "1"))
#endif
                {
                system("/usr/sbin/brctl delif br0 vlan1");
                system("/usr/sbin/et robowr 0x34 0x00 0x00");
                }
                /*foxconn modified end, water, 01/07/10*/
#endif
            }
            /* wklin modified end, 10/23/2008 */           
            save_wlan_time();
			start_bcmupnp();
      if(nvram_match ("wireless_restart", "1"))
      {
			start_wps();
            start_eapd();
            start_nas();
            if(nvram_match("enable_sta_mode","0") )
				start_acsd();
            sleep(2);
            start_wl();

   
      if((strcmp(nvram_safe_get("wla_ssid"),nvram_safe_get("wlg_ssid") )!=0))
          nvram_set("enable_band_steering", "0");      	

      if(strcmp(nvram_safe_get("wla_secu_type"),nvram_safe_get("wlg_secu_type") )!=0)
          nvram_set("enable_band_steering", "0");      	

      if(strcmp(nvram_safe_get("wla_secu_type"),"None") || strcmp(nvram_safe_get("wlg_secu_type"),"None"))
      {
          if(strcmp(nvram_safe_get("wla_passphrase"),nvram_safe_get("wlg_passphrase"))!=0) 
              nvram_set("enable_band_steering", "0");
      }


			if(nvram_match("enable_band_steering", "1") && nvram_match("wla_wlanstate", "Enable")&& nvram_match("wlg_wlanstate", "Enable"))
				start_bsd();
	  }
			if(nvram_match("wl_5g_bandsteering", "1") && nvram_match("wlh_wlanstate", "Enable")&& nvram_match("wlg_wlanstate", "Enable"))
				start_bsd();
            /* Now start ACOS services */

            /*foxconn Han edited, 10/02/2015*/
            isDhdReady();
            /* Foxconn added start pling 06/26/2014 */
            /* R8000 TD99, Link down/up WAN ethernet for Comcast modem IPv6 compatibility issue*/
            abEnableWanEthernetPort();
            /* Foxconn added end pling 06/26/2014 */


            eval("acos_init");
            eval("acos_service", "start");

            /* Start wsc if it is in 'unconfiged' state, and if PIN is not disabled */
      if(nvram_match ("wireless_restart", "1"))
      {
            if (nvram_match("wl0_wps_config_state", "0") && !nvram_match("wsc_pin_disable", "1"))
            {
                /* if "unconfig" to "config" mode, force it to built-in registrar and proxy mode */
                nvram_set("wl_wps_reg", "enabled");
                nvram_set("wl0_wps_reg", "enabled");
                nvram_set("wps_proc_status", "0");
                nvram_set("wps_method", "1");
                //nvram_set("wps_config_command", "1");
            }

            /* Foxconn added start pling 03/30/2009 */
            /* Fix antenna diversiy per Netgear Bing's request */
#if 0//(!defined WNR3500v2VCNA)        // pling added 04/10/2009, vnca don't want fixed antenna
            eval("wl", "down");
            eval("wl", "nphy_antsel", "0x02", "0x02", "0x02", "0x02");
            eval("wl", "up");
#endif
            /* Foxconn added end pling 03/30/2009 */
            //eval("wl", "interference", "2");    // pling added 03/27/2009, per Netgear Fanny request

#if ( (defined SAMBA_ENABLE) || (defined HSDPA) )
                if (!acosNvramConfig_match("wla_wlanstate", "Enable") || acosNvramConfig_match("wifi_on_off", "0"))
                {/*water, 05/15/2009, @disable wireless, router will reboot continually*/
                 /*on WNR3500L, WNR3500U, MBR3500, it was just a work around..*/
                    eval("wl", "down");
                }
#endif

			/* Fall through */
		  }
      nvram_set ("wireless_restart", "1");		  
		case TIMER:
            /* Foxconn removed start pling 07/12/2006 */
#if 0
			dprintf("TIMER\n");
			do_timer();
#endif
            /* Foxconn removed end pling 07/12/2006 */
			/* Fall through */
		case IDLE:
			dprintf("IDLE\n");
			/* foxconn modified start, zacker, 05/20/2010, @spec_1.9 */
			//state = IDLE;
			state = next_signal();
			if (state != IDLE)
				break;
			/* foxconn modified end, zacker, 05/20/2010, @spec_1.9 */

#ifdef CAPI_AP
			if (start_aput == TRUE) {
				system("/usr/sbin/wfa_aput_all&");
				start_aput = FALSE;
			}
#endif /* CAPI_AP */

			/* foxconn added start, zacker, 09/17/2009, @wps_led */
			if (nvram_match("wps_start",   "none"))
			    /* Foxconn add modified, Tony W.Y. Wang, 12/03/2009 */
				//send_wps_led_cmd(WPS_LED_BLINK_OFF, 0);
				if (acosNvramConfig_match("dome_led_status", "ON"))
                    send_wps_led_cmd(WPS_LED_BLINK_OFF, 3);
                else if (acosNvramConfig_match("dome_led_status", "OFF"))
                    send_wps_led_cmd(WPS_LED_BLINK_OFF, 2);
			/* foxconn added end, zacker, 09/17/2009, @wps_led */

			/* Wait for user input or state change */
			while (signalled == -1) {
				if (!noconsole && (!shell_pid || kill(shell_pid, 0) != 0))
					shell_pid = run_shell(0, 1);
				else {

					sigsuspend(&sigset);
				}
#ifdef LINUX26
				/*Foxconn modify start by Hank 07/31/2013*/
				/*for speed up USB3.0 throughput*/
				system("echo 1 > /proc/sys/vm/drop_caches");
				//system("echo 4096 > /proc/sys/vm/min_free_kbytes");
				/*Foxconn modify end by Hank 07/31/2013*/
#elif defined(__CONFIG_SHRINK_MEMORY__)
				eval("cat", "/proc/shrinkmem");
#endif	/* LINUX26 */
			}
			state = signalled;
			signalled = -1;
			break;

		case WSC_RESTART:
			dprintf("WSC_RESTART\n");
			/* foxconn modified start, zacker, 05/20/2010, @spec_1.9 */
			//state = IDLE;
			state = next_signal();
			/* foxconn modified end, zacker, 05/20/2010, @spec_1.9 */
			stop_wps();    /* Foxconn modify by aspen Bai, 08/01/2008 */
			start_wps();    /* Foxconn modify by aspen Bai, 08/01/2008 */
			break;

            /* Foxconn added start pling 06/14/2007 */
            /* We come here only if user press "apply" in Wireless GUI */
		case WLANRESTART:
		
		    stop_wps(); 
		    stop_nas();
            stop_eapd();
            stop_bcmupnp();

			/*Foxconn add start by Antony 06/16/2013*/
				stop_bsd();
			/*Foxconn add end by Antony 06/16/2013*/
            
			stop_wlan();
            
			/*Foxconn add start by Hank 06/14/2012*/
			/*Enable 2.4G auto channel detect, kill acsd stop change channel*/
			//if((nvram_match("wla_channel", "0") || nvram_match("wlg_channel", "0")) && nvram_match("enable_sta_mode","0"))
			if(nvram_match("enable_sta_mode","0"))
	            stop_acsd();
			/*Foxconn add end by Hank 06/14/2012*/
			eval("read_bd");    /* sync foxconn and brcm nvram params */
                       
            /* Foxconn Bob added start 07/24/2015, force disable PMF to fix IOT issue with Nexus 5 */
            disable_mfp();
            /* Foxconn Bob added end 07/24/2015, force disable PMF to fix IOT issue with Nexus 5 */

            /* wklin modified start, 01/29/2007 */
            /* hide unnecessary warnings (Invaid XXX, out of range xxx etc...)*/
            {
                #include <fcntl.h>
                int fd1, fd2;
                fd1 = dup(2);
                fd2 = open("/dev/null", O_WRONLY);
                close(2);
                dup2(fd2, 2);
                close(fd2);
                start_wlan(); //<-- to hide messages generated here
                close(2);
                dup2(fd1, 2);
                close(fd1);
            }
            /* wklin modified end, 01/29/2007 */
            #if 0
            /* Foxconn add start, Tony W.Y. Wang, 03/25/2010 @Single Firmware Implementation */
            if (nvram_match("sku_name", "NA"))
            {
                printf("set wl country and power of NA\n");
                eval("wl", "country", "Q1/15");
                /* Foxconn modify start, Max Ding, 12/27/2010 "US/39->US/8" for open DFS band 2&3 channels */
                //eval("wl", "-i", "eth2", "country", "US/39");
                eval("wl", "-i", "eth2", "country", "Q1/15");
                /* Foxconn modify end, Max Ding, 12/27/2010 */
                /* Foxconn remove start, Max Ding, 12/27/2010 fix time zone bug for NA sku */
                //nvram_set("time_zone", "-8");
                /* Foxconn remove end, Max Ding, 12/27/2010 */
                nvram_set("wla_region", "11");
                nvram_set("wla_temp_region", "11");
                nvram_set("wl_country", "Q1");
                nvram_set("wl_country_code", "Q1");
                nvram_set("ver_type", "NA");
            }
            /*
            else if (nvram_match("sku_name", "WW"))
            {
                printf("set wl country and power of WW\n");
                eval("wl", "country", "EU/5");
                eval("wl", "-i", "eth2", "country", "EU/5");
                nvram_set("time_zone", "0");
                nvram_set("wla_region", "5");
                nvram_set("wla_temp_region", "5");
                nvram_set("wl_country", "EU5");
                nvram_set("wl_country_code", "EU5");
                nvram_set("ver_type", "WW");
            }
            */
            /* Foxconn add end, Tony W.Y. Wang, 03/25/2010 @Single Firmware Implementation */
            #endif
            
            save_wlan_time();
            start_bcmupnp();
            start_wps();
            start_eapd();
            start_nas();
            if(nvram_match("enable_sta_mode","0") )
			//if((nvram_match("wla_channel", "0") || nvram_match("wlg_channel", "0")) && nvram_match("enable_sta_mode","0"))
				start_acsd();
            sleep(2);           /* Wait for WSC to start */
            start_wl();
#ifdef ARP_PROTECTION
            config_arp_table();
#endif 
		/*Foxconn add start by Antony 06/16/2013 Start the bandsteering*/
    
      if((strcmp(nvram_safe_get("wla_ssid"),nvram_safe_get("wlg_ssid") )!=0))
          nvram_set("enable_band_steering", "0");      	

      if(strcmp(nvram_safe_get("wla_secu_type"),nvram_safe_get("wlg_secu_type") )!=0)
          nvram_set("enable_band_steering", "0");      	

      if(strcmp(nvram_safe_get("wla_secu_type"),"None") || strcmp(nvram_safe_get("wlg_secu_type"),"None"))
      {
          if(strcmp(nvram_safe_get("wla_passphrase"),nvram_safe_get("wlg_passphrase"))!=0) 
              nvram_set("enable_band_steering", "0");
      }

			if(nvram_match("enable_band_steering", "1") && nvram_match("wla_wlanstate", "Enable")&& nvram_match("wlg_wlanstate", "Enable"))
				start_bsd();

			if(nvram_match("wl_5g_bandsteering", "1") && nvram_match("wlh_wlanstate", "Enable")&& nvram_match("wlg_wlanstate", "Enable"))
				start_bsd();
				
			/*Foxconn add end by Antony 06/16/2013*/

            /* Start wsc if it is in 'unconfiged' state */
            if (nvram_match("wl0_wps_config_state", "0") && !nvram_match("wsc_pin_disable", "1"))
            {
                /* if "unconfig" to "config" mode, force it to built-in registrar and proxy mode */
                nvram_set("wl_wps_reg", "enabled");
                nvram_set("wl0_wps_reg", "enabled");
                nvram_set("wps_proc_status", "0");
                nvram_set("wps_method", "1");
                //nvram_set("wps_config_command", "1");
            }
			/* foxconn modified start, zacker, 05/20/2010, @spec_1.9 */
			//state = IDLE;
			state = next_signal();
			/* foxconn modified end, zacker, 05/20/2010, @spec_1.9 */
		    break;
            /* Foxconn added end pling 06/14/2007 */
        /* Foxconn added start by EricHuang, 01/09/2008 */
		case PPPSTART:
		{
            //char *pptp_argv[] = { "pppd", NULL };
            char *pptp_argv[] = { "pppd", "file", "/tmp/ppp/options", NULL };

		    _eval(pptp_argv, NULL, 0, NULL);
		    
		    /* foxconn modified start, zacker, 05/20/2010, @spec_1.9 */
		    //state = IDLE;
		    state = next_signal();
		    /* foxconn modified end, zacker, 05/20/2010, @spec_1.9 */
		    break;
		}
		/* Foxconn added end by EricHuang, 01/09/2008 */
		    
		default:
			dprintf("UNKNOWN\n");
			return;
		}
	}
}

int
main(int argc, char **argv)
{
#ifdef LINUX26
	char *init_alias = "preinit";
#else
	char *init_alias = "init";
#endif
	char *base = strrchr(argv[0], '/');

	base = base ? base + 1 : argv[0];


	/* init */
#ifdef LINUX26
	if (strstr(base, "preinit")) {
		mount("devfs", "/dev", "tmpfs", MS_MGC_VAL, NULL);
		/* Michael added */
//        mknod("/dev/nvram", S_IRWXU|S_IFCHR, makedev(252, 0));
/*        mknod("/dev/mtdblock16", S_IRWXU|S_IFBLK, makedev(31, 16));
        mknod("/dev/mtdblock17", S_IRWXU|S_IFBLK, makedev(31, 17));
        mknod("/dev/mtd16", S_IRWXU|S_IFCHR, makedev(90, 32));
        mknod("/dev/mtd16ro", S_IRWXU|S_IFCHR, makedev(90, 33));
        mknod("/dev/mtd17", S_IRWXU|S_IFCHR, makedev(90, 34));
        mknod("/dev/mtd17ro", S_IRWXU|S_IFCHR, makedev(90, 35));*/
		/* Michael ended */
		mknod("/dev/console", S_IRWXU|S_IFCHR, makedev(5, 1));
		mknod("/dev/aglog", S_IRWXU|S_IFCHR, makedev(AGLOG_MAJOR_NUM, 0));
		mknod("/dev/wps_led", S_IRWXU|S_IFCHR, makedev(WPS_LED_MAJOR_NUM, 0));
#ifdef __CONFIG_UTELNETD__
		mkdir("/dev/pts", 0777);	
		mknod("/dev/pts/ptmx", S_IRWXU|S_IFCHR, makedev(5, 2));
		mknod("/dev/pts/0", S_IRWXU|S_IFCHR, makedev(136, 0));
		mknod("/dev/pts/1", S_IRWXU|S_IFCHR, makedev(136, 1));
#endif	/* __CONFIG_UTELNETD__ */
		/* Foxconn added start pling 12/26/2011, for WNDR4000AC */
#if (defined GPIO_EXT_CTRL)
		mknod("/dev/ext_led", S_IRWXU|S_IFCHR, makedev(EXT_LED_MAJOR_NUM, 0));
#endif
		/* Foxconn added end pling 12/26/2011 */
#else /* LINUX26 */
	if (strstr(base, "init")) {
#endif /* LINUX26 */
		main_loop();
		return 0;
	}

	/* Set TZ for all rc programs */
	setenv("TZ", nvram_safe_get("time_zone"), 1);

	/* rc [stop|start|restart ] */
	if (strstr(base, "rc")) {
		if (argv[1]) {
			if (strncmp(argv[1], "start", 5) == 0)
				return kill(1, SIGUSR2);
			else if (strncmp(argv[1], "stop", 4) == 0)
				return kill(1, SIGINT);
			else if (strncmp(argv[1], "restart", 7) == 0)
				return kill(1, SIGHUP);
		    /* Foxconn added start by EricHuang, 11/24/2006 */
		    else if (strcmp(argv[1], "wlanrestart") == 0)
		        return kill(1, SIGQUIT);
		    /* Foxconn added end by EricHuang, 11/24/2006 */
		} else {
			fprintf(stderr, "usage: rc [start|stop|restart|wlanrestart]\n");
			return EINVAL;
		}
	}

#ifdef __CONFIG_NAT__
	/* ppp */
	else if (strstr(base, "ip-up"))
		return ipup_main(argc, argv);
	else if (strstr(base, "ip-down"))
		return ipdown_main(argc, argv);

	/* udhcpc [ deconfig bound renew ] */
	else if (strstr(base, "udhcpc"))
		return udhcpc_wan(argc, argv);
#endif	/* __CONFIG_NAT__ */

#if 0 /* foxconn wklin removed, 05/14/2009 */
	/* ldhclnt [ deconfig bound renew ] */
	else if (strstr(base, "ldhclnt"))
		return udhcpc_lan(argc, argv);

	/* stats [ url ] */
	else if (strstr(base, "stats"))
		return http_stats(argv[1] ? : nvram_safe_get("stats_server"));
#endif

	/* erase [device] */
	else if (strstr(base, "erase")) {
		/* foxconn modified, zacker, 07/09/2010 */
		/*
		if (argv[1] && ((!strcmp(argv[1], "boot")) ||
			(!strcmp(argv[1], "linux")) ||
			(!strcmp(argv[1], "rootfs")) ||
			(!strcmp(argv[1], "nvram")))) {
		*/
		if (argv[1]) {
			return mtd_erase(argv[1]);
		} else {
			fprintf(stderr, "usage: erase [device]\n");
			return EINVAL;
		}
	}
	

	/* write [path] [device] */
	else if (strstr(base, "write")) {
		if (argc >= 3)
			return mtd_write(argv[1], argv[2]);
		else {
			fprintf(stderr, "usage: write [path] [device]\n");
			return EINVAL;
		}
	}

	/* hotplug [event] */
	else if (strstr(base, "hotplug")) {
		if (argc >= 2) {
            //printf("hotplug argv[1]=%s\n",argv[1]);

			if (!strcmp(argv[1], "net"))
				return hotplug_net();
		/*foxconn modified start, water, @usb porting, 11/11/2008*/
/*#ifdef __CONFIG_WCN__
			else if (!strcmp(argv[1], "usb"))
				return hotplug_usb();
#endif*/
        /*for mount usb disks, 4m board does not need these codes.*/
#if (defined SAMBA_ENABLE || defined HSDPA) /* Foxconn add, FredPeng, 03/16/2009 @HSDPA */
			/* else if (!strcmp(argv[1], "usb"))
				return usb_hotplug(); */
				/*return hotplug_usb();*/
			else if (!strcmp(argv[1], "block"))
                return hotplug_block(); /* wklin modified, 02/09/2011 */
#endif
#if defined(LINUX_2_6_36)
			else if (!strcmp(argv[1], "platform"))
				return coma_uevent();
#endif /* LINUX_2_6_36 */
            /* Foxconn added start pling 10/05/2012 */
            /* For USB LED after Kcode printer detection */
#if (defined INCLUDE_USB_LED)
            else
            {
                char *driver = getenv("PHYSDEVDRIVER");

                //printf("hotplug else case driver=%s \n",driver);
                if (driver && strstr(driver, "NetUSB"))
                {
                    hotplug_NetUSB();
                }
            }
#endif
            /* Foxconn added end pling 10/05/2012 */

        /*foxconn modified end, water, @usb porting, 11/11/2008*/
		} else {
			fprintf(stderr, "usage: hotplug [event]\n");
			return EINVAL;
		}
	}

	return EINVAL;
}
