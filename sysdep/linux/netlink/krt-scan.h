/*
 *	BIRD -- Linux Kernel Netlink Route Syncer -- Scanning
 *
 *	(c) 1998--1999 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#ifndef _BIRD_KRT_SCAN_H_
#define _BIRD_KRT_SCAN_H_

/*
 *  We don't have split iface/scan/set for Netlink. All options
 *  and run-time parameters are declared here instead of splitting
 *  to krt-set.h, krt-iface.h and this file.
 */

struct krt_scan_params {
  int async;				/* Allow asynchronous events */
};

struct krt_scan_status {
  list temp_ifs;			/* Temporary interfaces */
};

#endif