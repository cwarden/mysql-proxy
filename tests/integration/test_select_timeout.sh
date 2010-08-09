#!/bin/bash
#  $%BEGINLICENSE%$
#  Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
# 
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License as
#  published by the Free Software Foundation; version 2 of the
#  License.
# 
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
#  GNU General Public License for more details.
# 
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
#  02110-1301  USA
# 
#  $%ENDLICENSE%$
#
# test_select_timeout.sh
#
# A wrapper script to test whether fix for PR-255 (Bug#48570) is present
# in the current proxy codebase.
#
# This bug addresses the problem we face when a DB the proxy is monitoring
# "disappears" from the network while a transaction is in progress, either
# because of network partitioning or a system crash etc.
# Without the fix, the proxy needs to wait for the default timeout of the
# OS we're running on (eg 3 minutes on OpenSolaris) to occur before the 
# connection times out. 
# 
# RATIONALE:
# When trying to test this on a single machine, we face the problem that
# we cannot simulate "the other machine has disappeared". Killing a process
# will not stop the kernel from doing cleanup of the connection, and suspending
# it will also not stop the kernel from responding to TCP keepalive probes
# on behalf of the application.
#
# HIGH LEVEL APPROACH:
# - have a VM with mysqld which we monitor in proxy
# - start a pseudo long-running query against the mysqld in the VM
# - suspend the VM
# - watch the query time out, measure the time it took vs non-fixed time
# 
# PREREQUISITS:
# - VirtualBox 3.2.4-ish or newer (it needs to support the VBoxManage commands
#   used in this script.
# - an installed VM running an OS capable of running mysqld 
#   make sure that you know the IP address of the VM!
# - inside the VM: an installed instance of mysqld listening on port
#   $PROXY_PORT (you are free to chose one, default is 3306)
#
#   MAKE SURE that myslqd is automatically started when the VM is started; I 
#   believe that mysql standard installation takes care of this.
#
# - a proxy that is *running* with proxy-backend-addresses set to point to
#   the VM and the port mysqld in the VM is listening on
#
# GENERAL USAGE:
# you need to set (or accept the defaults) the following variables 
# either in your environment or at invocation of this script: 
# mandatory ones are marked with a ! below, others have a "reasonable" default
# ============================================================
# !VM_NAME      the name (or UUID) of the Virtual machine
# !DBUSER       user to connect to DB as.
# !DBPASS       password for DBUSER
# MYSQL         the (path and) name of the mysql binary.
# VBOXMG        the (path and) name of the mysql binary.
# PROXY_NAME    usually '127.0.0.1' (note: don't use localhost! this causes
#               mysql to use unix domain sockets, which don't support what
#               we're testing)
#               can also be a host name (resolving to a proper IP address)
# PROXY_PORT    port the proxy (not! the DB) is listening on
# SLEEP_TIME    sleep time (in seconds) for the availability test
# QUERY_TIME    duration of "long query" in seconds
#
# if you want to see the script's steps as it chugs along, set DBG=y
# ============================================================
#
# the script is written in a way so that it doesn't matter whether you write
# $ VAR=VALUE script
#   or
# $ script VAR=VALUE
#
# EXIT VALUES:
# 0     success
# 255   failure


# Function definitions
#
# get_vm_state prints the state (running, paused ...) of the named VM
function get_vm_state() {
	out=$(VBoxManage showvminfo $VM_NAME --machinereadable|grep 'VMState=')
	eval $out
	echo $VMState
}

# convenience functions to manipulate VM state
function resume_vm() {
	VBoxManage controlvm $VM_NAME resume >/dev/null 2>&1
}
function pause_vm() {
	VBoxManage controlvm $VM_NAME pause >/dev/null 2>&1
}
function start_vm() {
	VBoxManage startvm $VM_NAME >/dev/null 2>&1
}
function poweroff_vm {
	VBoxManage controlvm $VM_NAME poweroff >/dev/null 2>&1
}

# this function does the actual "query" to the DB via the proxy
function do_sleep() {
	echo "select sleep ($1)" | \
		$MYSQL -h$PROXY_NAME -P$PROXY_PORT -u$DBUSER -p$DBPASS >/dev/null 2>&1
}

# this is the initial "can I reach the DB" test
# since even this could in theory hang, we need to "watch" it and
# report failure if it does.
function sleep_test() {
	( do_sleep $SLEEP_TIME
	  return 0) &
	S_PID=$!
	sleep $((2 * $SLEEP_TIME))	# use a generous timeout

	# we send the subshell's pid a signal 0 which says "are you (still) there",
	# (on Solaris) status 0 means "yes" and 1 "no" (or no permission).
	# even if "yes", we don't try to terminate it, it times out by itself
	# (besides, killing a whole subshell with all its processes doesn't 
	# look as straightforward as I'd thought)
	kill -0 $S_PID >/dev/null 2>&1
	if [ $? -eq 0 ]; then
		echo "$MYSQL seems to be hanging, cannot continue"
		exit 255
	fi
	return 0
}

# this function uses an approach similar to sleep_test, but
# adds suspension of VM to the mix; we expect the subshell to terminate
# significantly sooner than the specified QUERY_TIME, so that's the time we're 
# prepared to wait before declaring the test failed.
#
# we attempt to resume the VM at all exit points of this function
function real_test () {
	(   T1=$SECONDS
		do_sleep $QUERY_TIME
		T2=$SECONDS
		DIFF=$(($T2 - $T1))

		# if the time difference is sleep time or greater, we have to
		# assume that the VM was resumed, presumable because we found that
		# the query wasn't timing out - IOW, we failed
		if [ $(($DIFF >= $QUERY_TIME)) ]; then
			return 255
		fi
		resume_vm
	    return 0) &
	S_PID=$!
	sleep 2
	pause_vm
	sleep $QUERY_TIME
	kill -0 $S_PID >/dev/null 2>&1
	if [ $? -eq 0 ]; then
		echo "$MYSQL seems to be hanging, cannot continue"
		resume_vm	# cause hanging process to terminate, so cleanup happens
		kill -KILL $S_PID
		exit 255
	else
		return 0
	fi
}

# "main" code starts here
# defaults:
DEF_PROXY_PORT=4040
DEF_PROXY_NAME=127.0.0.1
DEF_MYSQL_PATH=/bin/mysql
DEF_VBOXMG_PATH=/bin/VBoxManage
DEF_SLEEP_TIME=3
DEF_QUERY_TIME=120

# others
VM_STATE=""

# "flags"
vm_was_up=1 	# 1 == false!
vm_was_paused=1
vm_was_down=1

# evaluate arguments of form key=value
while [ $# -gt 0 ]; do
	eval $1
	shift
done

# assign defaults where applicable and necessary, complain about
# missing values where we have no defaults
# mandatory ones first:
VM_NAME=${VM_NAME:?"Please set the name or UUID of the Virtual Machine"}
DBUSER=${DBUSER:?"this variable needs to be set"}
DBPASS=${DBPASS:?"this variable needs to be set"}
# optional settings
MYSQL=${MYSQL:-$DEF_MYSQL_PATH}
VBOXMG=${VBOXMG:-$DEF_VBOXMG_PATH}
PROXY_NAME=${PROXY_NAME:-$DEF_PROXY_NAME}
PROXY_PORT=${PROXY_PORT:-$DEF_PROXY_PORT}
SLEEP_TIME=${SLEEP_TIME:-$DEF_SLEEP_TIME}
QUERY_TIME=${QUERY_TIME:-$DEF_QUERY_TIME}

if [ "$DBG" = "y" ]; then
	set -x
fi

# check executable paths
ENOEXEC="- this path is either invalid or the file is not executable"

if [ ! -x $MYSQL ]; then
	echo $MYSQL $ENOEXEC
	exit 255
fi

if [ ! -x $VBOXMG ]; then
	echo $VBOXMG $ENOEXEC
	exit 255
fi

# if the VM isn't up, start it
VM_STATE=$(get_vm_state)
if [ $VM_STATE = running ]; then
	vm_was_up=0		# remember to not take it down after test
elif [ $VM_STATE = paused ] ; then
	vm_was_paused=0
	resume_vm
else
	vm_was_down=0	# shut it down after we're done
	start_vm
fi

# make sure it's really up
VM_STATE=$(get_vm_state)
if [ ! $VM_STATE = running ]; then
	echo 'could not (re)start Virtual Machine, test failed'
	exit 255
fi

# now that we know that the VM is up, we'll try a simple test to
# verify that mysqld is actually responding

sleep_test

# if we haven't exited yet, sleep test was successful.

real_test

# if we haven't exited, test was successful, return 0

# attempt to return VM to state we found it in
if [ $vm_was_paused ]; then
	pause_vm
elif [ $vm_was_down ]; then
	stop_vm
fi
echo ok
exit 0
