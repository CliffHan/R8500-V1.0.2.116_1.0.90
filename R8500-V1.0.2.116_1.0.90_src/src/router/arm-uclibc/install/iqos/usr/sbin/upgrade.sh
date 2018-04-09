#!/bin/sh

# Filename definition
IDP=tdts
FWD=tdts_udbfw
QOS=tdts_udb

# Necessary command definition
RM="rm -f"
MV="mv"
CP="cp"
PS="ps"
KILL="kill"
LSMOD="lsmod"
RMMOD="rmmod"
INSMOD="insmod"
SETUP="setup.sh"

# Predict path definition
MAIN_PATH=/tmp/trend
UPDATE_PATH=/tmp/media/nand/TMDPI/update
BACKUP_PATH=/tmp/media/nand/TMDPI/backup
ARCHIVE_PATH=/tmp/media/nand/TMDPI/archive

# Error code definition
SUCC=0
FAIL_START_IDP=-1
FAIL_START_FWD=-2
FAIL_START_QOS=-3
FAIL_STOP_FWD=-4
FAIL_STOP_IDP=-5
FAIL_STOP_QOS=-6
FAIL_MOVE_IDP=-7
FAIL_MOVE_FWD=-8
FAIL_MOVE_QOS=-9

stop_sys()
{
	echo "--------------------"
	echo "Stop iQoS System...."

    bcmiqosd stop
	return "$SUCC"
}

start_sys()
{
	echo "--------------------"
	echo "Start iQoS System...."
    bcmiqosd start

	return "$SUCC"
}

move_file()
{
	printf "\t--------------------\n"
	printf "\tMove files from $1 to $2....\n"

	# Move IDP to indicated location
	if [ -f $1/$IDP.ko ]; then
		printf "\t\tMove $IDP.ko from $1 to $2\n"
		[ "$($MV $1/$IDP.ko $2)" ] && printf "\t\tMove $IDP.ko fail\n" && return "$FAIL_MOVE_IDP"
	else
		printf "\t\t$1/$IDP.ko doesn't exist\n" && return "$FAIL_MOVE_IDP"
	fi

	# Move bw_forward to indicated location
	if [ -f $1/$FWD.ko ]; then
		printf "\t\tMove $FWD.ko from $1 to $2\n"
		[ "$($MV $1/$FWD.ko $2)" ] && printf "\t\tMove $FWD.ko fail\n" && return "$FAIL_MOVE_FWD"
	else
		printf "\t\t$1/$FWD.ko doesn't exist\n" && return "$FAIL_MOVE_FWD"
	fi

	# Move QOS to indicated location
	if [ -f $1/$QOS.ko ]; then
		printf "\t\tMove $QOS.ko from $1 to $2\n"
		[ "$($MV $1/$QOS.ko $2)" ] && printf "\t\tMove $QOS.ko fail\n" && return "$FAIL_MOVE_QOS"
	else
		printf "\t\t$1/$QOS.ko doesn't exist\n" && return "$FAIL_MOVE_QOS"
	fi

	return "$SUCC"
}

copy_file()
{
	printf "\t--------------------\n"
	printf "\tcopyfiles from $1 to $2....\n"

	# copy IDP to indicated location
	if [ -f $1/$IDP.ko ]; then
		printf "\t\tcopy $IDP.ko from $1 to $2\n"
		[ "$($CP $1/$IDP.ko $2/)" ] && printf "\t\tcopy $IDP.ko fail\n" && return "$FAIL_MOVE_IDP"
	else
		printf "\t\t$1/$IDP.ko doesn't exist\n" && return "$FAIL_MOVE_IDP"
	fi

	# copy bw_forward to indicated location
	if [ -f $1/$FWD.ko ]; then
		printf "\t\tcopy $FWD.ko from $1 to $2\n"
		[ "$($CP $1/$FWD.ko $2/)" ] && printf "\t\tcopy $FWD.ko fail\n" && return "$FAIL_MOVE_FWD"
	else
		printf "\t\t$1/$FWD.ko doesn't exist\n" && return "$FAIL_MOVE_FWD"
	fi

	# copy QOS to indicated location
	if [ -f $1/$QOS.ko ]; then
		printf "\t\tcopy $QOS.ko from $1 to $2\n"
		[ "$($CP $1/$QOS.ko $2/)" ] && printf "\t\tcopy $QOS.ko fail\n" && return "$FAIL_MOVE_QOS"
	else
		printf "\t\t$1/$QOS.ko doesn't exist\n" && return "$FAIL_MOVE_QOS"
	fi

	return "$SUCC"
}
restore_file()
{
	move_file $BACKUP_PATH $MAIN_PATH 
	#RET=$? && return "$RET"
	RET=$?
	if [ "$RET" == "$SUCC" ]; then
		printf "\tRestore DPI succesfully\n" && start_sys
		if [ "$RET" == "$SUCC" ]; then
			printf "\tRestart DPI succesfully\n" && exit "$RET"
		else
			printf "\tRestart DPI fail\n" && exit "$RET"
		fi
	else
		printf "\tRestore DPI fail\n" && exit "$RET"
	fi
}

backup_file()
{
	move_file $MAIN_PATH $BACKUP_PATH
	RET=$? && return "$RET"
}

update_file()
{
	move_file $UPDATE_PATH $MAIN_PATH
	RET=$? && return "$RET"
}

archive_file()
{
	copy_file $MAIN_PATH $ARCHIVE_PATH
	RET=$? && return "$RET"
}

cleanup_file()
{
    rm -f $UPDATE_PATH/*
    rm -f $BACKUP_PATH/*
    rm -f $ARCHIVE_PATH/*
    rm -f /tmp/media/nand/rule.trf
    rm -f /tmp/media/nand/TmToNtgr_dev_mapping
}

all()
{
	printf "Update DPI System\n"

	stop_sys
	RET=$?
	if [ "$RET" == "$SUCC" ]; then
		printf "\tStop DPI succesfully\n"
	else
		printf "\tStop DPI fail\n" && exit "$RET"
	fi

	backup_file
	RET=$?
	if [ "$RET" == "$SUCC" ]; then
		printf "\tBackup DPI succesfully\n"
	else
		printf "\tBackup DPI fail\n" && restore_file
	fi

	update_file
	RET=$?
	if [ "$RET" == "$SUCC" ]; then
		printf "\tUpdate DPI succesfully\n"
	else
		printf "\tUpdate DPI fail\n" && restore_file
	fi

	start_sys
	RET=$?
	if [ "$RET" == "$SUCC" ]; then
		printf "\tStart DPI succesfully\n"
		archive_file
		RET=$?
		if [ "$RET" == "$SUCC" ]; then
			printf "\tArchive DPI succesfully\n"
		else
			printf "\tArchive DPI fail\n" && restore_file
		fi
	else
		printf "\tStart DPI fail\n" && restore_file
	fi

	exit "$SUCC"
}

help()
{
	printf "Usage:\n"
	printf "\tall: execute full process to update system\n"
	printf "\tstart: start DPI system\n"
	printf "\tstop: stop DPI system\n"
	printf "\tbackup: backup DPI binaries\n"
	printf "\tupdate: update DPI binaries\n"
	printf "\trestore: restore DPI binaries\n"
	printf "\tcleanup: clean up DPI binaries\n"
	printf "\thelp and blank: show help\n"
}

[ "$1" = "all" ] && all && exit 0
[ "$1" = "start" ] && start_sys && exit 0
[ "$1" = "stop" ] && stop_sys && exit 0
[ "$1" = "backup" ] && backup_file && exit 0
[ "$1" = "update" ] && update_file && exit 0
[ "$1" = "archive" ] && archive_file && exit 0
[ "$1" = "restore" ] && restore_file && exit 0
[ "$1" = "cleanup" ] && cleanup_file && exit 0
[ "$1" = "help" ] && help && exit 0
[ "$1" = "" ] && help && exit 0
