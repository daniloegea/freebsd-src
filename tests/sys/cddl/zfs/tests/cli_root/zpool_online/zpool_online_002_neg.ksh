#!/usr/local/bin/ksh93 -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
. $STF_SUITE/include/libtest.kshlib

################################################################################
#
# __stc_assertion_start
#
# ID: zpool_online_002_neg
#
# DESCRIPTION:
# Executing 'zpool online' command with bad options fails.
#
# STRATEGY:
# 1. Create an array of badly formed 'zpool online' options.
# 2. Execute each element of the array.
# 3. Verify an error code is returned.
#
# TESTABILITY: explicit
#
# TEST_AUTOMATION_LEVEL: automated
#
# CODING_STATUS: COMPLETED (2005-09-30)
#
# __stc_assertion_end
#
################################################################################
verify_runnable "global"

DISKLIST=$(get_disklist $TESTPOOL)

set -A args "" "-?" "-e fakepool" "-v fakepool" "-ev fakepool" "-ve fakepool" \
	"-t $TESTPOOL" "-t $TESTPOOL/$TESTFS" "-t $TESTPOOL/$TESTFS $DISKLIST" \
	"-t $TESTPOOL/$TESTCTR" "-t $TESTPOOL/$TESTCTR/$TESTFS1" \
	"-t $TESTPOOL/$TESTCTR $DISKLIST" "-t $TESTPOOL/$TESTVOL" \
	"-t $TESTPOOL/$TESTCTR/$TESTFS1 $DISKLIST" \
	"-t $TESTPOOL/$TESTVOL $DISKLIST" \
	"-t $DISKLIST" \
	"$TESTPOOL" "$TESTPOOL/$TESTFS" "$TESTPOOL/$TESTFS $DISKLIST" \
	"$TESTPOOL/$TESTCTR" "$TESTPOOL/$TESTCTR/$TESTFS1" \
	"$TESTPOOL/$TESTCTR $DISKLIST" "$TESTPOOL/$TESTVOL" \
	"$TESTPOOL/$TESTCTR/$TESTFS1 $DISKLIST" "$TESTPOOL/$TESTVOL $DISKLIST" \
	"$DISKLIST"

log_assert "Executing 'zpool online' with bad options fails"

if [[ -z $DISKLIST ]]; then
        log_fail "DISKLIST is empty."
fi

typeset -i i=0

while [[ $i -lt ${#args[*]} ]]; do

	log_mustnot $ZPOOL online ${args[$i]}

	(( i = i + 1 ))
done

log_pass "'zpool online' command with bad options failed as expected."
