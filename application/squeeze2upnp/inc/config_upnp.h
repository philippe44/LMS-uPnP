/*
 *  Squeeze2upnp - LMS to uPNP bridge
 *
 *	(c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#pragma once

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ixml.h" /* for IXML_Document, IXML_Element */

void			SaveConfig(char *name, void *ref, bool full);
void		 	*LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf);
void	 		*FindMRConfig(void *ref, char *UDN);
void 			*LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf, sq_dev_param_t *sq_conf);

