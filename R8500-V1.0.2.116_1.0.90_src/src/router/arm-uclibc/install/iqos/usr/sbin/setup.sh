#!/bin/sh
cmd="$1";
[ -z "$cmd" ] && cmd="start"

if [ "$2" == "" ]; then
dev_wan=eth0
else
dev_wan="$2";
fi

if [ "$3" == "" ]; then
dev_lan=br0
else
dev_lan="$3";
fi

MAIN_PATH=/tmp/trend

idp_mod=$MAIN_PATH/tdts.ko
udb_mod=$MAIN_PATH/tdts_udb.ko
fw_mod=$MAIN_PATH/tdts_udbfw.ko
#rule=/tmp/trend/rule.trf
rule=$MAIN_PATH/rule.trf
agent=tdts_rule_agent

sess_num=30000

dev=/dev/detector
dev_maj=190
dev_min=0

fwdev=/dev/idpfw
fwdev_maj=191
fwdev_min=0

case "$cmd" in
start)
	if [ ! -f "$rule" ]; then
		echo "Signature file $rule not found"
		exit 1
	fi

	# create dev node
	echo "Creating device nodes..."
	[ ! -c "$dev" ] && mknod $dev c $dev_maj $dev_min
	[ ! -c "$fwdev" ] && mknod $fwdev c $fwdev_maj $fwdev_min
	[ -c $dev ] || echo "...Create $dev failed"
	[ -c $fwdev ] || echo "...Create $fwdev failed"

#	echo "Filter WAN bootp packets..."
#	chain=BWDPI_FILTER
#	iptables -t mangle -N $chain
#	iptables -t mangle -F $chain
#	iptables -t mangle -A $chain -i $dev_wan -p udp --sport 68 --dport 67 -j DROP
#	iptables -t mangle -A $chain -i $dev_wan -p udp --sport 67 --dport 68 -j DROP
#	iptables -t mangle -A PREROUTING -i $dev_wan -p udp -j $chain

	echo "Insert IDP engine..."	
	insmod ./$idp_mod || exit 1

	echo "Running rule agent to setup signature file $rule..."
	(cd /tmp/trend; ./$agent -g)

	echo "Insert UDB ..."
	insmod ./$udb_mod || exit 1

	echo "Insert forward module $fw_mod with param - dev_wan=$dev_wan..."
	insmod ./$fw_mod dev_wan=$dev_wan sess_num=$sess_num tcp_conn_max=4000 udp_flow_max=1000 app_timeout=30 user_timeout=300 || exit 1
	if [ ! -f "$rule" ]; then
		echo "Signature file $rule doesn't exist!"
		exit -1
	fi
	;;
stop)

	echo "Unload fw_mod..."
	rmmod $fw_mod > /dev/null 2>&1
	echo "Unload udb_mod..."
	rmmod $udb_mod > /dev/null 2>&1
	echo "Unload idp_mod..."
	rmmod $idp_mod > /dev/null 2>&1

	echo "Remove device nodes..."
	[ -c "$dev" ] && rm -f $dev 
	[ ! -c "$dev" ] || echo "...Remove $dev failed"
	[ -c "$fwdev" ] && rm -f $fwdev
	[ ! -c "$fwdev" ] || echo "...Remove $fwdev failed"
	
	;;
restart)
	$0 stop $dev_wan $dev_lan
	sleep 2
	$0 start $dev_wan $dev_lan
	;;
esac;
