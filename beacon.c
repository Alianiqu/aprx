/* **************************************************************** *
 *                                                                  *
 *  APRX -- 2nd generation receive-only APRS-i-gate with            *
 *          minimal requirement of esoteric facilities or           *
 *          libraries of any kind beyond UNIX system libc.          *
 *                                                                  *
 * (c) Matti Aarnio - OH2MQK,  2007-2009                            *
 *                                                                  *
 * **************************************************************** */

#include "aprx.h"

struct beaconmsg {
	time_t nexttime;
	const struct aprx_interface *interface;
	const char *src;
	const char *dest;
	const char *via;
	const char *msg;
	const char *filename;
	int8_t	    beaconmode; // -1: net only, 0: both, +1: radio only
	int8_t	    timefix;
};

static struct beaconmsg **beacon_msgs;

static int beacon_msgs_count;
static int beacon_msgs_cursor;

static time_t beacon_nexttime;
static time_t beacon_last_nexttime;
static int    beacon_cycle_size = 20*60; // 20 minutes



static const char* scan_int(const char *p, int len, int *val, int *seen_space) {
	int i;
	char c;
	*val = 0;
	for (i = 0; i < len; ++i, ++p) {
		c = *p;
		if (('0' <= c && c <= '9') && !(*seen_space)) {
			*val = (*val) * 10 + (c - '0');
		} else if (c == ' ') {
			*val = (*val) * 10;
			*seen_space = 1;
		} else {
			return NULL;
		}
	}
	return p;
}

int validate_degmin_input(const char *s, int maxdeg)
{
	int deg;
	int m1, m2;
	char c;
	const char *t;
	int seen_space = 0;
	if (maxdeg > 90) {
		t = scan_int(s, 3, &deg, &seen_space);
		if (t != s+3) return 1; // scan failure
		if (deg > 179) return 1; // too large value
		s = t;
		t = scan_int(s, 2, &m1, &seen_space);
		if (t != s+2) return 1;
		if (m1 > 59) return 1;
		s = t;
		c = *s;
		if (!seen_space && c == '.') {
			// OK
		} else if (!seen_space && c == ' ') {
			seen_space = 1;
		} else {
			return 1; // Bad char..
		}
		++s;
		t = scan_int(s, 2, &m2, &seen_space);
		if (t != s+2) return 1;
		s = t;
		c = *s;
		if (c != 'E' && c != 'e' && c != 'W' && c != 'w') return 1;
	} else {
		t = scan_int(s, 2, &deg, &seen_space);
		if (t != s+2) return 1; // scan failure
		if (deg > 89) return 1; // too large value
		s = t;
		t = scan_int(s, 2, &m1, &seen_space);
		if (t != s+2) return 1;
		if (m1 > 59) return 1;
		s = t;
		c = *s;
		if (!seen_space && c == '.') {
			// OK
		} else if (!seen_space && c == ' ') {
			seen_space = 1;
		} else {
			return 1; // Bad char..
		}
		++s;
		t = scan_int(s, 2, &m2, &seen_space);
		if (t != s+2) return 1;
		s = t;
		c = *s;
		if (c != 'N' && c != 'n' && c != 'S' && c != 's') return 1;
	}
	return 0;		/* zero for OK */
}


static void beacon_reset(void)
{
	beacon_nexttime = now + 30;	/* start 30 seconds from now */
	beacon_msgs_cursor = 0;
}

static void beacon_set(struct configfile *cf, const char *p1, char *str, const int beaconmode)
{
	const char *srcaddr  = NULL;
	const char *destaddr = NULL;
	const char *via      = NULL;
	int buflen = strlen(p1) + strlen(str ? str : "") + 10;
	char *buf  = alloca(buflen);
	const char *to   = NULL;
	char *code = NULL;
	char *lat  = NULL;
	char *lon  = NULL;
	char *comment = NULL;
	char *type    = NULL;
	const struct aprx_interface *aif = NULL;
	int has_fault = 0;

#ifdef _FOR_VALGRIND_
	memset(buf, 0, buflen);
#endif

	*buf = 0;
	struct beaconmsg *bm = malloc(sizeof(*bm));
	memset(bm, 0, sizeof(*bm));

	if (debug) {
	  printf("BEACON parameters: ");
	}

	while (*p1) {

		/* if (debug)
		   printf("p1='%s' ",p1); */

		if (strcmp(p1, "to") == 0) {

			to = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			if (strcmp(to,"$mycall") == 0) {
				to = mycall;
			} else {
			  config_STRUPPER((void*)to);
			}


			aif = find_interface_by_callsign(to);
			if ((aif != NULL) && !aif->txok) {
				aif = NULL;  // Not an TX interface :-(
				if (debug)printf("\n");
				printf("%s:%d Sorry, <beacon> to '%s' that is not a TX capable interface.\n",
				       cf->name, cf->linenum, to);
				has_fault = 1;
				goto discard_bm; // sigh..
			}

			if (debug)
				printf("to '%s' ", to);

		} else if (strcmp(p1, "for") == 0) {

			srcaddr = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			// What about ITEM and OBJECT ?

			// if (!validate_callsign_input((char *) srcaddr),1) {
			//   if (debug)printf("\n");
			//   printf("Invalid rfbeacon FOR callsign");
			// }

			if (debug)
				printf("for '%s' ", srcaddr);

		} else if (strcmp(p1, "dest") == 0) {

			destaddr = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			if (debug)
				printf("dest '%s' ", destaddr);

		} else if (strcmp(p1, "via") == 0) {

			via  = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			if (debug)
				printf("via '%s' ", via);

		} else if (strcmp(p1, "lat") == 0) {
			/*  ddmm.mmN   */

			lat = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			if (validate_degmin_input(lat, 90)) {
			  has_fault = 1;
			  printf("Latitude input has bad format: '%s'\n",lat);
			}

			if (debug)
				printf("lat '%s' ", lat);

		} else if (strcmp(p1, "lon") == 0) {
			/*  dddmm.mmE  */

			lon = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			if (validate_degmin_input(lon, 180)) {
			  has_fault = 1;
			  printf("Longitude input has bad format: '%s'\n",lon);
			}

			if (debug)
				printf("lon '%s' ", lon);

		} else if (strcmp(p1, "symbol") == 0) {
			/*   R&    */

			code = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);
			if (strlen(code) != 2) {
			  has_fault = 1;
			  printf("Symbol code lenth is not exactly 2 chars\n");
			}

			if (debug)
				printf("symbol '%s' ", code);

		} else if (strcmp(p1, "type") == 0) {
			/* text up to .. 40 chars */

			type = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			if (debug)
				printf("type '%s' ", type);
			if (type[0] != '!' ||  type[1] != 0)
			  printf("%s:%d Sorry, Supported type is only '!'\n",
				 cf->name, cf->linenum);

		} else if (strcmp(p1, "comment") == 0) {
			/* text up to .. 40 chars */

			comment = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			if (debug)
				printf("comment '%s' ", comment);

		} else if (strcmp(p1, "raw") == 0) {

			p1 = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			bm->msg = strdup(p1);

			// FIXME: validate the data with APRS parser...

			if (debug)
				printf("raw '%s' ", bm->msg);

		} else if (strcmp(p1, "file") == 0) {

			p1 = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);

			bm->filename = strdup(p1);

			if (debug)
				printf("file '%s' ", bm->msg);

		} else if (strcmp(p1, "timefix") == 0) {
			bm->timefix = 1;
			if (debug)
				printf("timefix ");

		} else {

			has_fault = 1;
#if 0
			if (debug)
				printf("Unknown keyword: '%s'", p1);

			p1 = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);
#else
			/* Unknown keyword, a raw message ? */
			bm->msg = strdup(p1);

			if (debug)
				printf("raw '%s' ", bm->msg);

			break;
#endif
		}

		p1 = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);
	}
	if (debug)
		printf("\n");
	if (has_fault)
		goto discard_bm;

	if (aif == NULL && beaconmode >= 0) {
		if (debug)
			printf("%s:%d Lacking 'to' keyword for this beacon definition.\n",
			       cf->name, cf->linenum);
	}

	if (srcaddr == NULL)
		srcaddr = mycall;

	if (srcaddr == NULL) {
		if (debug)
			printf("%s:%d Lacking the 'for' keyword for this beacon definition.\n", cf->name, cf->linenum);
		has_fault = 1;
		goto discard_bm;
	}


	if (destaddr == NULL)
		destaddr = tocall;

	bm->src       = srcaddr != NULL ? strdup(srcaddr) : NULL;
	bm->dest      = strdup(destaddr);
	bm->via       = via != NULL ? strdup(via) : NULL;
	bm->interface = aif;
	bm->beaconmode = beaconmode;

	if (!bm->msg && !bm->filename) {
		/* Not raw packet, perhaps composite ? */
		if (!type) type = "!";
		if (code && strlen(code) == 2 && lat && strlen(lat) == 8 &&
		    lon && strlen(lon) == 9) {
			sprintf(buf, "%s%s%c%s%c%s", type, lat, code[0], lon,
				code[1], comment ? comment : "");
			bm->msg = strdup(buf);
		} else {
			if (!code || (code && strlen(code) != 2))
				printf("%s:%d .. BEACON definition failure; symbol parameter missing or wrong size\n", cf->name, cf->linenum);
			if (!lat || (lat && strlen(lat) != 8))
				printf("%s:%d .. BEACON definition failure; lat(itude) parameter missing or wrong size\n", cf->name, cf->linenum);
			if (!lon || (lon && strlen(lon) != 9))
				printf("%s:%d .. BEACON definition failure; lon(gitude) parameter missing or wrong size\n", cf->name, cf->linenum);
			/* parse failure, abandon the alloc too */
			has_fault = 1;
			goto discard_bm;
		}
	}

	if (debug) {
	  switch (beaconmode) {
	  case 1:
	    printf("RFONLY");
	    break;
	  case 0:
	    printf("RF+NET");
	    break;
	  default:
	    printf("NETONLY");
	    break;
	  }
	  printf(" BEACON FOR ");
	  if (srcaddr == NULL)
	    printf("***>%s", destaddr);
	  else
	    printf("%s>,%s",srcaddr,destaddr);
	  if (via != NULL)
	    printf(",%s", via);
	  printf("'  '%s'\n", bm->msg);
	}

	/* realloc() works also when old ptr is NULL */
	beacon_msgs = realloc(beacon_msgs,
			      sizeof(bm) * (beacon_msgs_count + 3));

	beacon_msgs[beacon_msgs_count++] = bm;
	beacon_msgs[beacon_msgs_count] = NULL;
	
	if (bm->msg != NULL) {  // Make this into AX.25 UI frame
	                        // with leading control byte..
	  int len = strlen(bm->msg);
	  char *msg = realloc((void*)bm->msg, len+3); // make room
	  memmove(msg+2, msg, len+1); // move string end \0 also
	  msg[0] = 0x03;  // Control byte
	  msg[1] = 0xF0;  // PID 0xF0
	  bm->msg = msg;
	}

	beacon_reset();

	if (0) {
	discard_bm:
	  if (bm->dest != NULL) free((void*)(bm->dest));
	  if (bm->msg  != NULL) free((void*)(bm->msg));
	  free(bm);
	}
	return;
}

void beacon_config(struct configfile *cf)
{
	char *name, *param1;
	char *str = cf->buf;
	int   beaconmode = 0;

	while (readconfigline(cf) != NULL) {
		if (configline_is_comment(cf))
			continue;	/* Comment line, or empty line */

		// It can be severely indented...
		str = config_SKIPSPACE(cf->buf);

		name = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);
		config_STRLOWER(name);

		param1 = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);

		if (strcmp(name, "</beacon>") == 0)
		  break;

		if (strcmp(name, "beacon") == 0) {
		  beacon_set(cf, param1, str, beaconmode);

		} else if (strcmp(name, "beaconmode") == 0) {
		  if (strcasecmp(param1, "both") == 0) {
		    beaconmode = 0;

		  } else if (strcasecmp(param1,"radio") == 0) {
		    beaconmode = 1;

		  } else if (strcasecmp(param1,"aprsis") == 0) {
		    beaconmode = -1;

		  } else {
		    printf("%s:%d Unknown beaconmode parameter keyword: '%s'\n",
			   cf->name, cf->linenum, param1);
		  }

		} else {
		  printf("%s:%d Unknown config keyword: '%s'\n",
			 cf->name, cf->linenum, name);
		  continue;
		}
	}
}

static void fix_beacon_time(char *txt, int txtlen)
{
	int hour, min, sec;
	char hms[8];

	sec = now % (3600*24);
	hour = sec / 3600;
	min  = (sec / 60) % 60;
	sec  = sec % 60;
	sprintf(hms, "%02d%02d%02dh", hour, min, sec);

	txt += 2; txtlen -= 2; // Skip Control+PID

	if (*txt == ';' && txtlen >= 36) { // Object

		// ;434.775-B*111111z6044.06N/02612.79Er
		memcpy( txt+11, hms, 7 ); // Overwrite with new time
	} else if ((*txt == '/' || *txt == '@') && txtlen >= 27) { // Position with timestamp
		memcpy( txt+1, hms, 7 ); // Overwrite with new time
	}
}


static char *msg_read_file(const char *filename, char *buf, int buflen)
{
	FILE *fp = fopen(filename,"r");
	if (!fp) return NULL;
	if (fgets(buf, buflen, fp)) {
		char *p = strchr(buf, '\n');
		if (p) *p = 0;
	} else {
		*buf = 0;
	}
	fclose(fp);
	if (*buf == 0) return NULL;
	return buf;
}

static void beacon_now(void) 
{
	int  destlen;
	int  txtlen, msglen;
	int  i;
	struct beaconmsg *bm;
	char const *txt;
	char *msg;

	if (beacon_msgs_cursor == 0) {
		float beacon_cycle, beacon_increment;
		int   r;
		srand(now);
		r = rand() % 1024;
		beacon_cycle = (beacon_cycle_size -
				0.2*beacon_cycle_size * (r*0.001));
		beacon_increment = beacon_cycle / beacon_msgs_count;
		if (beacon_increment < 3.0)
			beacon_increment = 3.0;	/* Minimum interval: 3 seconds ! */
		if (debug)
			printf("beacons cycle: %.2f minutes, r: %d, increment: %.1f seconds\n",
			       beacon_cycle/60.0, r, beacon_increment);
		for (i = 0; i < beacon_msgs_count; ++i) {
			beacon_msgs[i]->nexttime =
			  now + (long)(i * beacon_increment + 0.5);
		}
		beacon_last_nexttime = now + (long)(beacon_msgs_count * beacon_increment + 0.5);
	}

	/* --- now the business of sending ... */

	bm = beacon_msgs[beacon_msgs_cursor++];

	beacon_nexttime = bm->nexttime;
	if (beacon_msgs_cursor >= beacon_msgs_count) {	/* Last done.. */
		beacon_msgs_cursor = 0;
	        beacon_nexttime    = beacon_last_nexttime;
	}
	
	destlen = strlen(bm->dest) + ((bm->via != NULL) ? strlen(bm->via): 0) +2;

	if (bm->filename != NULL) {
		msg = alloca(2000);
		txt = msg+2;
		msg[0] = 0x03;
		msg[1] = 0xF0;
		if (!msg_read_file(bm->filename, msg+2, 2000-2)) {
			// Failed loading
			syslog(LOG_ERR, "Failed to load anything from beacon file %s", bm->filename);
			return;
		}
	} else {
		msg     = (char*)bm->msg;
		txt     = bm->msg+2; // Skip Control+PID bytes
	}

	txtlen  = strlen(txt);
	msglen  = txtlen+2; // this includes the control+pid bytes

	/* _NO_ ending CRLF, the APRSIS subsystem adds it. */

	/* Send those (rf)beacons.. (a noop if interface == NULL) */
	if (bm->interface != NULL) {
		const char *callsign = bm->interface->callsign;
		int   len  = destlen + 2 + strlen(callsign);
		char *destbuf = alloca(len);
		const char *src = (bm->src != NULL) ? bm->src : callsign;

		if (bm->via != NULL)
		  sprintf(destbuf,"%s>%s,%s",src,bm->dest,bm->via);
		else
		  sprintf(destbuf,"%s>%s", src, bm->dest);

		if (bm->timefix)
		  fix_beacon_time(msg, msglen);

		if (bm->beaconmode <= 0) {
		  // Send them all also as netbeacons..
		  aprsis_queue(destbuf, strlen(destbuf),
			       aprsis_login, txt, txtlen);
		}

		if (bm->beaconmode >= 0) {
		  if (bm->via || strcmp(src, callsign) != 0) {
		    len     = ((bm->via ? strlen(bm->via) : 0) +
			       strlen(callsign));
		    destbuf = alloca(len + 5); // recycled for: viabuf!
		    if (strcmp(src, callsign) != 0) {
		      if (bm->via != NULL)
			sprintf( destbuf, "%s*,%s", callsign, bm->via );
		      else
			sprintf( destbuf, "%s*", callsign );
		    } else {
		      strcpy( destbuf, bm->via );
		    }
		  } else {
		    destbuf = NULL;
		  }
		}

		if (debug) {
		  printf("Now beaconing to interface %s '%s>%s",
			 callsign, src, bm->dest);
		  if (destbuf) printf(",%s", destbuf);
		  printf("' -> '%s',  next beacon in %.2f minutes\n",
			 txt, ((beacon_nexttime - now)/60.0));
		}

		// And to interfaces
		interface_transmit_beacon(bm->interface,
					  src,
					  bm->dest,
					  destbuf,  // re-written via
					  bm->msg, msglen);
	} 
	else {
	    for ( i = 0; i < all_interfaces_count; ++i ) {
		const struct aprx_interface *aif = all_interfaces[i];
		if (aif->txok) {
		    const char *callsign = aif->callsign;
		    int   len  = destlen + 2 + strlen(callsign);
		    char *destbuf = alloca(len);
		    const char *src =
		      (bm->src != NULL) ? bm->src : callsign;

		    if (bm->via != NULL)
		      sprintf(destbuf,"%s>%s,%s", src, bm->dest, bm->via);
		    else
		      sprintf(destbuf,"%s>%s", src, bm->dest);
		    
		    if (bm->timefix)
		      fix_beacon_time((char*)msg, msglen);

		    if (bm->beaconmode <= 0) {
		      // Send them all also as netbeacons..
		      aprsis_queue(destbuf, strlen(destbuf),
				   aprsis_login, txt, txtlen);
		    }
		    if (bm->beaconmode >= 0) {
		      if (bm->via || strcmp(src, callsign) != 0) {
			len     = ((bm->via ? strlen(bm->via) : 0) +
				   strlen(callsign));
			destbuf = alloca(len + 5); // recycled for: viabuf!
			if (strcmp(src, callsign) != 0) {
			  if (bm->via != NULL)
			    sprintf( destbuf, "%s*,%s", callsign, bm->via );
			  else
			    sprintf( destbuf, "%s*", callsign );
			} else {
			  strcpy( destbuf, bm->via );
			}
		      } else {
			destbuf = NULL;
		      }
		    }

		    if (debug) {
		      printf("Now beaconing to interface %s '%s>%s",
			     callsign, src, bm->dest);
		      if (destbuf) printf(",%s", destbuf);
		      printf("' -> '%s',  next beacon in %.2f minutes\n",
			     txt, ((beacon_nexttime - now)/60.0));
		    }

		    // .. and send to all interfaces..
		    interface_transmit_beacon(aif,
					      src,
					      bm->dest,
					      destbuf, // re-written via
					      bm->msg, msglen);
		}
	    }
	}
}

int beacon_prepoll(struct aprxpolls *app)
{
	if (!aprsis_login)
		return 0;	/* No mycall !  hoh... */
	if (!beacon_msgs)
		return 0;	/* Nothing to do */

	if (beacon_nexttime < app->next_timeout)
		app->next_timeout = beacon_nexttime;

	return 0;		/* No poll descriptors, only time.. */
}


int beacon_postpoll(struct aprxpolls *app)
{
	if (!aprsis_login)
		return 0;	/* No mycall !  hoh... */
	if (!beacon_msgs)
		return 0;	/* Nothing to do */
	if (beacon_nexttime > now)
		return 0;	/* Too early.. */

	beacon_now();

	return 0;
}
