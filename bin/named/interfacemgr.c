/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <isc/assertions.h>
#include <isc/error.h>
#include <isc/mem.h>
#include <isc/result.h>
#include <isc/socket.h>
#include <isc/types.h>
#include <isc/net.h>
#include <isc/interfaceiter.h>
#include <isc/util.h>

#include <dns/dispatch.h>

#include <named/client.h>
#include <named/globals.h>
#include <named/listenlist.h>
#include <named/log.h>
#include <named/interfacemgr.h>

#define IFMGR_MAGIC		0x49464D47U	/* IFMG. */	
#define NS_INTERFACEMGR_VALID(t) ((t) != NULL && (t)->magic == IFMGR_MAGIC)

struct ns_interfacemgr {
	unsigned int		magic;		/* Magic number. */
	int			references;
	isc_mutex_t		lock;
	isc_mem_t *		mctx;		/* Memory context. */
	isc_taskmgr_t *		taskmgr;	/* Task manager. */
	isc_socketmgr_t *	socketmgr;	/* Socket manager. */
	ns_clientmgr_t *	clientmgr;	/* Client manager. */
	unsigned int		generation;	/* Current generation no. */
	ns_listenlist_t *	listenon;
	ISC_LIST(ns_interface_t) interfaces;	/* List of interfaces. */
};

static void purge_old_interfaces(ns_interfacemgr_t *mgr);

isc_result_t
ns_interfacemgr_create(isc_mem_t *mctx, isc_taskmgr_t *taskmgr,
		       isc_socketmgr_t *socketmgr, ns_clientmgr_t *clientmgr,
		       ns_interfacemgr_t **mgrp)
{
	isc_result_t result;
	ns_interfacemgr_t *mgr;

	REQUIRE(mctx != NULL);
	REQUIRE(mgrp != NULL);
	REQUIRE(*mgrp == NULL);
	
	mgr = isc_mem_get(mctx, sizeof(*mgr));
	if (mgr == NULL)
		return (DNS_R_NOMEMORY);

	result = isc_mutex_init(&mgr->lock);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	mgr->mctx = mctx;
	mgr->taskmgr = taskmgr;
	mgr->socketmgr = socketmgr;
	mgr->clientmgr = clientmgr;
	mgr->generation = 1;
	mgr->listenon = NULL;
	ISC_LIST_INIT(mgr->interfaces);

	result = ns_listenlist_default(mctx, ns_g_port,
				       &mgr->listenon);
	if (result != DNS_R_SUCCESS)
		goto cleanup;
	
	mgr->references = 1;
	mgr->magic = IFMGR_MAGIC;
	*mgrp = mgr;
	return (DNS_R_SUCCESS);

 cleanup:
	isc_mem_put(mctx, mgr, sizeof(*mgr));
	return (result);
}

static void
ns_interfacemgr_destroy(ns_interfacemgr_t *mgr)
{
	REQUIRE(NS_INTERFACEMGR_VALID(mgr));
	ns_listenlist_detach(&mgr->listenon);
	isc_mutex_destroy(&mgr->lock);
	mgr->magic = 0;
	isc_mem_put(mgr->mctx, mgr, sizeof *mgr);
}

void
ns_interfacemgr_attach(ns_interfacemgr_t *source,
		       ns_interfacemgr_t **target)
{
	REQUIRE(NS_INTERFACEMGR_VALID(source));
	LOCK(&source->lock);
	INSIST(source->references > 0);
	source->references++;
	UNLOCK(&source->lock);
	*target = source;
}

void 
ns_interfacemgr_detach(ns_interfacemgr_t **targetp)
{
	isc_result_t need_destroy = ISC_FALSE;
	ns_interfacemgr_t *target = *targetp;
	REQUIRE(target != NULL);
	REQUIRE(NS_INTERFACEMGR_VALID(target));
	LOCK(&target->lock);
	REQUIRE(target->references > 0);
	target->references--;
	if (target->references == 0)
		need_destroy = ISC_TRUE;
	UNLOCK(&target->lock);
	if (need_destroy)
		ns_interfacemgr_destroy(target);
	*targetp = NULL;
}

void
ns_interfacemgr_shutdown(ns_interfacemgr_t *mgr)
{
	REQUIRE(NS_INTERFACEMGR_VALID(mgr));

	LOCK(&mgr->lock);
	/*
	 * Shut down and detach all interfaces.
	 * By incrementing the generation count, we make purge_old_interfaces()
	 * consider all interfaces "old".
	 */
	mgr->generation++;
	purge_old_interfaces(mgr);
	INSIST(ISC_LIST_EMPTY(mgr->interfaces));
	UNLOCK(&mgr->lock);	
}


static isc_result_t
ns_interface_create(ns_interfacemgr_t *mgr, isc_sockaddr_t *addr,
		    ns_interface_t **ifpret)
{
        ns_interface_t *ifp;
	isc_result_t result;
	
	REQUIRE(NS_INTERFACEMGR_VALID(mgr));
	ifp = isc_mem_get(mgr->mctx, sizeof(*ifp));
	if (ifp == NULL)
		return (DNS_R_NOMEMORY);
	ifp->mgr = NULL;
	ifp->generation = mgr->generation;
	ifp->addr = *addr;

	result = isc_mutex_init(&ifp->lock);
	if (result != ISC_R_SUCCESS)
		goto lock_create_failure;

	/*
	 * Create a task.
	 */
	ifp->task = NULL;
	result = isc_task_create(mgr->taskmgr, mgr->mctx, 0, &ifp->task);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_task_create() failed: %s",
				 isc_result_totext(result));
		goto task_create_failure;
	}

	ifp->udpsocket = NULL;
	ifp->udpdispatch = NULL;
	
	ifp->tcpsocket = NULL;
	/*
	 * Create a single TCP client object.  It will replace itself
	 * with a new one as soon as it gets a connection, so the actual
	 * connections will be handled in parallel even though there is
	 * only one client initially.
	 */
	ifp->ntcptarget = 1;
	ifp->ntcpcurrent = 0;

	ns_interfacemgr_attach(mgr, &ifp->mgr);	
	ISC_LIST_APPEND(mgr->interfaces, ifp, link);

	ifp->references = 1;
	ifp->magic = IFACE_MAGIC;
	*ifpret = ifp;

	return (DNS_R_SUCCESS);


	isc_task_detach(&ifp->task);
 task_create_failure:
	isc_mutex_destroy(&ifp->lock);
 lock_create_failure:
	ifp->magic = 0;
	isc_mem_put(mgr->mctx, ifp, sizeof(*ifp));

	return (DNS_R_UNEXPECTED);
}

static isc_result_t
ns_interface_listenudp(ns_interface_t *ifp) {
	isc_result_t result;
	
	/*
	 * Open a UDP socket.
	 */
	result = isc_socket_create(ifp->mgr->socketmgr,
				   isc_sockaddr_pf(&ifp->addr),
				   isc_sockettype_udp,
				   &ifp->udpsocket);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "creating UDP socket: %s",
				 isc_result_totext(result));
		goto udp_socket_failure;
	}
	result = isc_socket_bind(ifp->udpsocket, &ifp->addr);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "binding UDP socket: %s",
				 isc_result_totext(result));
		goto udp_bind_failure;
	}
	/* 
	 * XXXRTH hardwired constants.  We're going to need to determine if
	 * this UDP socket will be shared with the resolver, and if so, we
	 * need to set the hashsize to be be something bigger than 17.
	 */
	result = dns_dispatch_create(ifp->mgr->mctx, ifp->udpsocket, ifp->task,
				     4096, 50, 50, 17, 19, NULL,
				     &ifp->udpdispatch);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "UDP dns_dispatch_create(): %s",
				 isc_result_totext(result));
		goto udp_dispatch_failure;
	}

	result = ns_clientmgr_createclients(ifp->mgr->clientmgr, ns_g_cpus, ifp,
					    ISC_FALSE);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "UDP ns_clientmgr_createclients(): %s",
				 isc_result_totext(result));
		goto addtodispatch_failure;
	}
	return (ISC_R_SUCCESS);

 addtodispatch_failure:
	dns_dispatch_detach(&ifp->udpdispatch);
 udp_dispatch_failure:
 udp_bind_failure:
	isc_socket_detach(&ifp->udpsocket);
 udp_socket_failure:
	return (result);
}

static isc_result_t
ns_interface_accepttcp(ns_interface_t *ifp) {
	isc_result_t result;
	
	/*
	 * Open a TCP socket.
	 */
	result = isc_socket_create(ifp->mgr->socketmgr,
				   isc_sockaddr_pf(&ifp->addr),
				   isc_sockettype_tcp,
				   &ifp->tcpsocket);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "creating TCP socket: %s",
				 isc_result_totext(result));
		goto tcp_socket_failure;
	}
	result = isc_socket_bind(ifp->tcpsocket, &ifp->addr);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "binding TCP socket: %s",
				 isc_result_totext(result));
		goto tcp_bind_failure;
	}
	result = isc_socket_listen(ifp->tcpsocket, 0);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "listen TCP socket: %s",
				 isc_result_totext(result));
		goto tcp_listen_failure;
	}

	result = ns_clientmgr_createclients(ifp->mgr->clientmgr,
					    ifp->ntcptarget, ifp,
					    ISC_TRUE);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "TCP ns_clientmgr_createclients(): %s",
				 isc_result_totext(result));
		goto accepttcp_failure;
	}
	return (ISC_R_SUCCESS);

 accepttcp_failure:
 tcp_listen_failure:
 tcp_bind_failure:
	isc_socket_detach(&ifp->tcpsocket);
 tcp_socket_failure:
	return (DNS_R_SUCCESS);
}

static isc_result_t
ns_interface_setup(ns_interfacemgr_t *mgr, isc_sockaddr_t *addr,
		   ns_interface_t **ifpret)
{
	isc_result_t result;
	ns_interface_t *ifp = NULL;
	REQUIRE(ifpret != NULL && *ifpret == NULL);
	
	result = ns_interface_create(mgr, addr, &ifp);
	if (result != DNS_R_SUCCESS)
		return (result);

	result = ns_interface_listenudp(ifp);
	if (result != DNS_R_SUCCESS)
		goto cleanup_interface;

	result = ns_interface_accepttcp(ifp);
	if (result != DNS_R_SUCCESS) {
		/*
		 * XXXRTH  We don't currently have a way to easily stop dispatch
		 * service, so we return currently return DNS_R_SUCCESS (the UDP
		 * stuff will work even if TCP creation failed).  This will be fixed
		 * later.
		 */
		result = ISC_R_SUCCESS;
	}
	*ifpret = ifp;
	return (ISC_R_SUCCESS);
	
 cleanup_interface:
	ISC_LIST_UNLINK(ifp->mgr->interfaces, ifp, link);	
	ns_interface_detach(&ifp);
	return (result);
}

static void
ns_interface_destroy(ns_interface_t *ifp) {
	isc_mem_t *mctx = ifp->mgr->mctx;
	REQUIRE(NS_INTERFACE_VALID(ifp));

	if (ifp->udpdispatch != NULL)
		dns_dispatch_detach(&ifp->udpdispatch);
	if (ifp->udpsocket != NULL) {
		isc_socket_cancel(ifp->udpsocket, NULL, ISC_SOCKCANCEL_ALL);
		isc_socket_detach(&ifp->udpsocket);
	}
	if (ifp->tcpsocket != NULL) {
		isc_socket_cancel(ifp->tcpsocket, NULL, ISC_SOCKCANCEL_ALL);
		isc_socket_detach(&ifp->tcpsocket);
	}

	isc_task_detach(&ifp->task);
	isc_mutex_destroy(&ifp->lock);

	ns_interfacemgr_detach(&ifp->mgr);
	
	ifp->magic = 0;
	isc_mem_put(mctx, ifp, sizeof(*ifp));
}

void
ns_interface_attach(ns_interface_t *source,
		    ns_interface_t **target)
{
	REQUIRE(NS_INTERFACE_VALID(source));
	LOCK(&source->lock);
	INSIST(source->references > 0);
	source->references++;
	UNLOCK(&source->lock);
	*target = source;
}

void 
ns_interface_detach(ns_interface_t **targetp)
{
	isc_result_t need_destroy = ISC_FALSE;
	ns_interface_t *target = *targetp;
	REQUIRE(target != NULL);
	REQUIRE(NS_INTERFACE_VALID(target));
	LOCK(&target->lock);
	REQUIRE(target->references > 0);
	target->references--;
	if (target->references == 0)
		need_destroy = ISC_TRUE;
	UNLOCK(&target->lock);
	if (need_destroy)
		ns_interface_destroy(target);
	*targetp = NULL;
}

/*
 * Search the interface list for an interface whose address and port
 * both match those of 'addr'.  Return a pointer to it, or NULL if not found.
 */
static ns_interface_t *
find_matching_interface(ns_interfacemgr_t *mgr, isc_sockaddr_t *addr) {
        ns_interface_t *ifp;
        for (ifp = ISC_LIST_HEAD(mgr->interfaces); ifp != NULL;
	     ifp = ISC_LIST_NEXT(ifp, link)) {
		if (isc_sockaddr_equal(&ifp->addr, addr))
			break;
	}
        return (ifp);
}

/*
 * Remove any interfaces whose generation number is not the current one.
 */
static void
purge_old_interfaces(ns_interfacemgr_t *mgr) {
        ns_interface_t *ifp, *next;
        for (ifp = ISC_LIST_HEAD(mgr->interfaces); ifp != NULL; ifp = next) {
		INSIST(NS_INTERFACE_VALID(ifp));
		next = ISC_LIST_NEXT(ifp, link);
		if (ifp->generation != mgr->generation) {
			ISC_LIST_UNLINK(ifp->mgr->interfaces, ifp, link);
			ns_interface_detach(&ifp);
		}
	}
}

static void
do_ipv4(ns_interfacemgr_t *mgr) {
	isc_interfaceiter_t *iter = NULL;
	isc_result_t result;

	result = isc_interfaceiter_create(mgr->mctx, &iter);
	if (result != ISC_R_SUCCESS)
		return;
	
	result = isc_interfaceiter_first(iter);
	while (result == ISC_R_SUCCESS) {
		ns_interface_t *ifp;
		isc_interface_t interface;
		ns_listenelt_t *le;
		
		/*
		 * XXX insert code to match against named.conf
		 * "listen-on" statements here.  Also build list of
		 * local addresses and local networks.
		 */
		
		result = isc_interfaceiter_current(iter, &interface);
		if (result != ISC_R_SUCCESS)
			break;

		for (le = ISC_LIST_HEAD(mgr->listenon->elts);
		     le != NULL;
		     le = ISC_LIST_NEXT(le, link))
		{
			int match;
			isc_sockaddr_t listen_addr;
			char buf[128];
			const char *addrstr;

			/*
			 * Construct a socket address for this IP/port
			 * combination.
			 */
			isc_sockaddr_fromin(&listen_addr,
					    &interface.address.type.in,
					    le->port);

			/*
			 * Construct a human-readable version of same.
			 */
			addrstr = inet_ntop(listen_addr.type.sin.sin_family,
					    &listen_addr.type.sin.sin_addr,
					    buf, sizeof(buf));
			if (addrstr == NULL)
				addrstr = "(bad address)";

			/*
			 * See if the address matches the listen-on statement;
			 * if not, ignore the interface.
			 */
			result = dns_acl_match(&listen_addr, NULL,
					       le->acl, &match, NULL);
			if (match <= 0)
				continue;
			
			ifp = find_matching_interface(mgr, &listen_addr);
			if (ifp != NULL) {
				ifp->generation = mgr->generation;
			} else {
				isc_log_write(ns_g_lctx, NS_LOGCATEGORY_NETWORK,
					      NS_LOGMODULE_INTERFACEMGR,
					      ISC_LOG_INFO,
					      "listening on IPv4 interface %s, %s port %u",
					      interface.name, addrstr,
					      ntohs(listen_addr.type.sin.sin_port));
				
				result = ns_interface_setup(mgr, &listen_addr, &ifp);
				if (result != DNS_R_SUCCESS) {
					UNEXPECTED_ERROR(__FILE__, __LINE__,
							 "creating IPv4 interface %s "
							 "failed; interface ignored",
							 interface.name);
				}
				/* Continue. */
			}

		}
		result = isc_interfaceiter_next(iter);
	}
	if (result != ISC_R_NOMORE)
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "IPv4: interface iteration failed: %s",
				 isc_result_totext(result));

	isc_interfaceiter_destroy(&iter);
}

static void
do_ipv6(ns_interfacemgr_t *mgr) {
	isc_result_t result;
	ns_interface_t *ifp;
	isc_sockaddr_t listen_addr;
	struct in6_addr in6a;

	in6a = in6addr_any;
	isc_sockaddr_fromin6(&listen_addr, &in6a, ns_g_port);

	ifp = find_matching_interface(mgr, &listen_addr);
	if (ifp != NULL) {
		ifp->generation = mgr->generation;
	} else {
		isc_log_write(ns_g_lctx, NS_LOGCATEGORY_NETWORK,
			      NS_LOGMODULE_INTERFACEMGR, ISC_LOG_INFO,
			      "listening on IPv6 interfaces, port %u",
			      ns_g_port);
		result = ns_interface_setup(mgr, &listen_addr, &ifp);
		if (result != DNS_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "listening on IPv6 interfaces failed");
			/* Continue. */
		}
	}
}

void
ns_interfacemgr_scan(ns_interfacemgr_t *mgr) {

	REQUIRE(NS_INTERFACEMGR_VALID(mgr));

	mgr->generation++;	/* Increment the generation count. */ 

	if (isc_net_probeipv6() == ISC_R_SUCCESS) {
		do_ipv6(mgr);
	} else
		isc_log_write(ns_g_lctx, NS_LOGCATEGORY_NETWORK,
			      NS_LOGMODULE_INTERFACEMGR, ISC_LOG_INFO,
			      "no IPv6 interfaces found");
	if (isc_net_probeipv4() == ISC_R_SUCCESS)
		do_ipv4(mgr);
	else
		isc_log_write(ns_g_lctx, NS_LOGCATEGORY_NETWORK,
			      NS_LOGMODULE_INTERFACEMGR, ISC_LOG_INFO,
			      "no IPv4 interfaces found");

        /*
         * Now go through the interface list and delete anything that
         * does not have the current generation number.  This is
         * how we catch interfaces that go away or change their
         * addresses.
	 */
	purge_old_interfaces(mgr);

	if (ISC_LIST_EMPTY(mgr->interfaces)) {
		isc_log_write(ns_g_lctx, NS_LOGCATEGORY_NETWORK,
			      NS_LOGMODULE_INTERFACEMGR, ISC_LOG_WARNING,
			      "not listening on any interfaces");
		/*
		 * Continue anyway.
		 */
	}
}

void
ns_interfacemgr_setlistenon(ns_interfacemgr_t *mgr,
			    ns_listenlist_t *value)
{
	LOCK(&mgr->lock);
	ns_listenlist_detach(&mgr->listenon);
	ns_listenlist_attach(value, &mgr->listenon);
	UNLOCK(&mgr->lock);
}
