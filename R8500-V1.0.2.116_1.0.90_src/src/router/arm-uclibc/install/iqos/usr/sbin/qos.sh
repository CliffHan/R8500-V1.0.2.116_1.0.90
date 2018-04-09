#!/bin/sh

cmd=$1
iqos_cli=iqos_cli

if [ "$cmd" == "" ];
then
	echo "./qos.sh start|stop|restart"
	exit 1

fi

case "$cmd" in 
start)
	echo "Start qos..."
	echo 8 > /proc/sys/kernel/printk
	(cd /tmp/trend; ./sample.bin -a set_qos_conf)
	(cd /tmp/trend; ./sample.bin -a set_qos_on)
    echo "[qos.sh] Runnig iqos_cli background mode"
    ($iqos_cli -b &)
	;;

stop)
	echo "Stop qos..."
	(cd /tmp/trend; ./sample.bin -a set_qos_off)
    echo "[qos.sh] kill iqos cli"
    (killall $iqos_cli)
	;;

restart)
	echo "Restart qos..."
	(cd /tmp/trend; ./qos.sh stop)
	(cd /tmp/trend; ./qos.sh start)
	;;

esac
