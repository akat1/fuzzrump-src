/*	$NetBSD: auth2-kbdint.c,v 1.14 2021/09/02 11:26:17 christos Exp $	*/
/* $OpenBSD: auth2-kbdint.c,v 1.13 2021/07/02 05:11:20 dtucker Exp $ */

/*
 * Copyright (c) 2000 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "includes.h"
__RCSID("$NetBSD: auth2-kbdint.c,v 1.14 2021/09/02 11:26:17 christos Exp $");
#include <sys/types.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "xmalloc.h"
#include "packet.h"
#include "hostfile.h"
#include "auth.h"
#include "log.h"
#include "misc.h"
#include "servconf.h"
#include "ssherr.h"

/* import */
extern ServerOptions options;

static int
userauth_kbdint(struct ssh *ssh)
{
	int r, authenticated = 0;
	char *lang, *devs;

	if ((r = sshpkt_get_cstring(ssh, &lang, NULL)) != 0 ||
	    (r = sshpkt_get_cstring(ssh, &devs, NULL)) != 0 ||
	    (r = sshpkt_get_end(ssh)) != 0)
		fatal_fr(r, "parse packet");

	debug("keyboard-interactive devs %s", devs);

	if (options.kbd_interactive_authentication)
		authenticated = auth2_challenge(ssh, devs);

	free(devs);
	free(lang);
	return authenticated;
}

Authmethod method_kbdint = {
	"keyboard-interactive",
	userauth_kbdint,
	&options.kbd_interactive_authentication
};
