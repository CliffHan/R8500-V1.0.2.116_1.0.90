/*
 * Linux network interface code
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
 * $Id: interface.c 439947 2013-11-28 11:17:56Z $
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/route.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if_arp.h>
#include <proto/ethernet.h>
#include <shutils.h>
#include <bcmnvram.h>
#include <bcmutils.h>
#include <bcmparams.h>
#include <rc.h>
#include <ambitCfg.h>

int
ifconfig(char *name, int flags, char *addr, char *netmask)
{
	int s;
	struct ifreq ifr;
	struct in_addr in_addr, in_netmask, in_broadaddr;

	/* Open a raw socket to the kernel */
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
		goto err;

	/* Set interface name */
	strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name)-1);
	ifr.ifr_name[sizeof(ifr.ifr_name)-1] = 0;

	/* Set interface flags */
	ifr.ifr_flags = flags;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
		goto err;

	/* Set IP address */
	if (addr) {
		inet_aton(addr, &in_addr);
		sin_addr(&ifr.ifr_addr).s_addr = in_addr.s_addr;
		ifr.ifr_addr.sa_family = AF_INET;
		if (ioctl(s, SIOCSIFADDR, &ifr) < 0)
			goto err;
	}

	/* Set IP netmask and broadcast */
	if (addr && netmask) {
		inet_aton(netmask, &in_netmask);
		sin_addr(&ifr.ifr_netmask).s_addr = in_netmask.s_addr;
		ifr.ifr_netmask.sa_family = AF_INET;
		if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0)
			goto err;

		in_broadaddr.s_addr = (in_addr.s_addr & in_netmask.s_addr) | ~in_netmask.s_addr;
		sin_addr(&ifr.ifr_broadaddr).s_addr = in_broadaddr.s_addr;
		ifr.ifr_broadaddr.sa_family = AF_INET;
		if (ioctl(s, SIOCSIFBRDADDR, &ifr) < 0)
			goto err;
	}

	close(s);

	return 0;

err:
	if (s >= 0)
		close(s);
	perror(name);
	return errno;
}

int
ifconfig_get(char *name, int *flags, unsigned long *addr, unsigned long *netmask)
{
	int s;
	struct ifreq ifr;

	/* Open a raw socket to the kernel */
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
		goto err;

	/* Set interface name */
	strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name) - 1);
	ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';

	/* Check MAC first */
	if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0)
		goto err;
	else if (memcmp(ifr.ifr_hwaddr.sa_data, "\0\0\0\0\0\0", 6) == 0)
		goto err;

	/* Get interface flags */
	if (flags) {
		if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0)
			goto err;
		else
			memcpy(flags, &(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr),
				sizeof(*flags));
	}

	/* Get IP address */
	if (addr) {
		if (ioctl(s, SIOCGIFADDR, &ifr) < 0)
			goto err;
		else
			memcpy(addr, &(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr),
				sizeof(*addr));
	}

	/* Get Net mask */
	if (netmask) {
		if (ioctl(s, SIOCGIFNETMASK, &ifr) < 0)
			goto err;
		else {
			memcpy(netmask, &(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr),
				sizeof(*netmask));
		}
	}

	close(s);

	return 0;

err:
	if (s >= 0)
		close(s);
	perror(name);
	return errno;
}

static int
route_manip(int cmd, char *name, int metric, char *dst, char *gateway, char *genmask)
{
	int s;
	struct rtentry rt;

	/* Open a raw socket to the kernel */
	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		goto err;

	/* Fill in rtentry */
	memset(&rt, 0, sizeof(rt));
	if (dst)
		inet_aton(dst, &sin_addr(&rt.rt_dst));
	if (gateway)
		inet_aton(gateway, &sin_addr(&rt.rt_gateway));
	if (genmask)
		inet_aton(genmask, &sin_addr(&rt.rt_genmask));
	rt.rt_metric = metric;
	rt.rt_flags = RTF_UP;
	if (sin_addr(&rt.rt_gateway).s_addr)
		rt.rt_flags |= RTF_GATEWAY;
	if (sin_addr(&rt.rt_genmask).s_addr == INADDR_BROADCAST)
		rt.rt_flags |= RTF_HOST;
	rt.rt_dev = name;

	/* Force address family to AF_INET */
	rt.rt_dst.sa_family = AF_INET;
	rt.rt_gateway.sa_family = AF_INET;
	rt.rt_genmask.sa_family = AF_INET;

	if (ioctl(s, cmd, &rt) < 0)
		goto err;

	close(s);
	return 0;

err:
	if (s >= 0)
		close(s);
	perror(name);
	return errno;
}

int
route_add(char *name, int metric, char *dst, char *gateway, char *genmask)
{
	return route_manip(SIOCADDRT, name, metric, dst, gateway, genmask);
}

int
route_del(char *name, int metric, char *dst, char *gateway, char *genmask)
{
	return route_manip(SIOCDELRT, name, metric, dst, gateway, genmask);
}

/* configure loopback interface */
void
config_loopback(void)
{
	/* Bring up loopback interface */
	ifconfig("lo", IFUP, "127.0.0.1", "255.0.0.0");

	/* Add to routing table */
	route_add("lo", 0, "127.0.0.0", "0.0.0.0", "255.0.0.0");
}

void update_port_priority(int vlan_id,char *vlan_interface,int priority)
{
    char *ports;
    char vlanxxports[64];
    sprintf(vlanxxports,"vlan%dports",vlan_id);
    ports=acosNvramConfig_get(vlanxxports);

    if(strlen(ports))
    {
        char port_str[32],*next;
        int port;
        char priority_command[64];
   	    foreach(port_str, ports, next) {
   	        port=atoi(port_str);
            
            /*when gmac disabled, port 5 means CPU port*/
   	        if(port==5 || port == 0) // WAN port or CPU port 
   	            break;

            /*foxconn Han edited, 07/02/2015*/
            if(isTriBand() && port == 4) /*port#4 connect to 2nd switch*/
                continue;
 
   	        sprintf(priority_command,"et robowr 0x34 0x%x 0x%X",0x10+port*2,vlan_id+(priority << 13));
   	        system(priority_command);
            //printf("%s %d %s=%s id=0x%X pri=0x%X\n",__func__,__LINE__,vlanxxports,ports,vlan_id,priority);
            //printf("%s\n",priority_command);
   	    }
    }

    /*foxconn Han edited start, 07/02/2015 update external switch configuration*/
    if(!isTriBand())
        return;
#ifdef CONFIG_2ND_SWITCH
    char evlanxxports[64];
    sprintf(evlanxxports,"evlan%dports",vlan_id);
    ports=acosNvramConfig_get(evlanxxports);
    if(strlen(ports))
    {
        char port_str[32],*next;
        int port;
        char priority_command[64];
   	    foreach(port_str, ports, next) {
   	        port=atoi(port_str);
            
   	        if(port==5 || port == 0 || port == 1) /*#1 connect to 1st switch*/
   	            continue;
 
   	        sprintf(priority_command,"et erobowr 0x34 0x%x 0x%X",0x10+port*2,vlan_id+(priority << 13));
   	        system(priority_command);
            //printf("%s %d %s=%s id=0x%X pri=0x%X\n",__func__,__LINE__,evlanxxports,ports,vlan_id,priority);
            //printf("%s\n",priority_command);
   	    }
    }
#endif /*CONFIG_2ND_SWITCH*/
    /*foxconn Han edited end, 07/02/2015 update external switch configuration*/
}

/* configure/start vlan interface(s) based on nvram settings */
int
start_vlan(void)
{
	int s;
	struct ifreq ifr;
	int i, j;
	unsigned char ea[ETHER_ADDR_LEN];
	char buf[256];
	
	
	
	/* Bob added start, 09/03/2009, to avoid sending router solicitation packets */ 
#ifdef INCLUDE_IPV6
	system("echo 0 > /proc/sys/net/ipv6/conf/all/router_solicitations");
	system("echo 0 > /proc/sys/net/ipv6/conf/default/router_solicitations"); // pling added 08/16/2010
#endif
    /* Bob added end, 09/03/2009, to avoid sending router solicitation packets */ 

	/* set vlan i/f name to style "vlan<ID>" */
	eval("vconfig", "set_name_type", "VLAN_PLUS_VID_NO_PAD");

	/* create vlan interfaces */
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
		return errno;

	for (i = 0; i <= VLAN_MAXVID; i ++) {
		char nvvar_name[16];
		char vlan_id[16];
		char *hwname, *hwaddr;
		char prio[8];
		struct ethtool_drvinfo info;

		/* get the address of the EMAC on which the VLAN sits */
		snprintf(nvvar_name, sizeof(nvvar_name), "vlan%dhwname", i);
		if (!(hwname = nvram_get(nvvar_name)))
			continue;
		snprintf(nvvar_name, sizeof(nvvar_name), "%smacaddr", hwname);
		if (!(hwaddr = nvram_get(nvvar_name)))
			continue;
		ether_atoe(hwaddr, ea);
		/* find the interface name to which the address is assigned */
		for (j = 1; j <= DEV_NUMIFS; j ++) {
			ifr.ifr_ifindex = j;
			if (ioctl(s, SIOCGIFNAME, &ifr))
				continue;
			if (ioctl(s, SIOCGIFHWADDR, &ifr))
				continue;
			if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER)
				continue;
			if (bcmp(ifr.ifr_hwaddr.sa_data, ea, ETHER_ADDR_LEN))
				continue;
			/* Get driver info, it can handle both et0 and et2 have same MAC */
			memset(&info, 0, sizeof(info));
			info.cmd = ETHTOOL_GDRVINFO;
			ifr.ifr_data = (caddr_t)&info;
			if (ioctl(s, SIOCETHTOOL, &ifr) < 0)
				continue;
			if (strcmp(info.driver, hwname) == 0)
				break;
		}
		if (j > DEV_NUMIFS)
			continue;
		if (ioctl(s, SIOCGIFFLAGS, &ifr))
			continue;
		
		/* Bob added start, 09/03/2009, to avoid sending router solicitation packets */ 
#ifdef INCLUDE_IPV6
		sprintf(buf, "echo 0 > /proc/sys/net/ipv6/conf/%s/router_solicitations", ifr.ifr_name);
		system(buf);
#endif
		/* Bob added end, 09/03/2009, to avoid sending router solicitation packets */ 
		
		if (!(ifr.ifr_flags & IFF_UP))
			ifconfig(ifr.ifr_name, IFUP, 0, 0);
		/* create the VLAN interface */
		snprintf(vlan_id, sizeof(vlan_id), "%d", i);
		eval("vconfig", "add", ifr.ifr_name, vlan_id);
		/* setup ingress map (vlan->priority => skb->priority) */
		snprintf(vlan_id, sizeof(vlan_id), "vlan%d", i);
		/* Bob added start, 09/03/2009, to avoid sending router solicitation packets */ 
#ifdef INCLUDE_IPV6
		sprintf(buf, "echo 0 > /proc/sys/net/ipv6/conf/%s/router_solicitations", vlan_id);
		system(buf);
#endif
		/* Bob added end, 09/03/2009, to avoid sending router solicitation packets */ 
		for (j = 0; j < VLAN_NUMPRIS; j ++) {
			snprintf(prio, sizeof(prio), "%d", j);
			eval("vconfig", "set_ingress_map", vlan_id, prio, prio);
            
            //printf("%s %d i=%d vconfig set_ingress_map %s %s %s\n",__func__,__LINE__,i,vlan_id,prio,prio);
/*Foxconn add start, edward zhang, 2013/07/03*/
#ifdef VLAN_SUPPORT

        /*setup egress vlan priority*/
        if(nvram_match("enable_vlan","enable"))
        {
            char vlan_prio[16];
            snprintf(vlan_prio,sizeof(vlan_prio),"%s_prio",vlan_id);

            if(nvram_get(vlan_prio))
            {
                //printf("%s %d i=%d j=%d %s %s=%s\n",__func__,__LINE__,i,j,vlan_id,vlan_prio,nvram_get(vlan_prio));
                update_port_priority(i,vlan_id,atoi(nvram_get(vlan_prio)));
                if(i==atoi(nvram_get("internet_vlan")))
                {
                    if(j==0)
                    {
                        eval("vconfig", "set_egress_map",vlan_id, "0", nvram_get("internet_prio"));
                        //printf("%s %d i=%d j=%d vconfig set_egress_map %s 0 %s\n",__func__,__LINE__,i,j,vlan_id,nvram_get("internet_prio"));
                    }
                }
                else
                {
                    eval("vconfig", "set_egress_map",vlan_id, prio, nvram_get(vlan_prio));
                    //printf("%s %d i=%d j=%d vconfig set_egress_map %s %s %s\n",__func__,__LINE__,i,j,vlan_id,prio,nvram_get(vlan_prio));
                }
            }
        }
#endif
/*Foxconn add end, edward zhang, 2013/07/03*/
		}
	}

    /* Bob added start 09/03/2009 */
#ifdef INCLUDE_IPV6
	if (nvram_match("ipv6ready", "1"))
	{
	    char cmd[32];
	    sprintf(cmd, "ifconfig %s hw ether %s",nvram_get("lan_interface"), nvram_get("lan_hwaddr"));
	    system(cmd);
	}
#endif
	/* Bob added end 09/03/2009 */

	close(s);

	return 0;
}

/* stop/rem vlan interface(s) based on nvram settings */
int
stop_vlan(void)
{
	int i;
	char nvvar_name[16];
	char vlan_id[16];
	char *hwname;

	for (i = 0; i <= VLAN_MAXVID; i ++) {
		/* get the address of the EMAC on which the VLAN sits */
		snprintf(nvvar_name, sizeof(nvvar_name), "vlan%dhwname", i);
		if (!(hwname = nvram_get(nvvar_name)))
			continue;

		/* remove the VLAN interface */
		snprintf(vlan_id, sizeof(vlan_id), "vlan%d", i);
		eval("vconfig", "rem", vlan_id);
	}

	return 0;
}
