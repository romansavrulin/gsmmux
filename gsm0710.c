/*
 *
 * GSM 07.10 Implementation with User Space Serial Ports
 *
 * Copyright (C) 2003  Tuukka Karvonen <tkarvone@iki.fi>
 *
 * Version 1.0 October 2003
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Modified November 2004 by David Jander <david@protonic.nl>
 *  - Hacked to use Pseudo-TTY's instead of the (obsolete?) USSP driver.
 *  - Fixed some bugs which prevented it from working with Sony-Ericsson modems
 *  - Seriously broke hardware handshaking.
 *  - Changed commandline interface to use getopts:
 *
 * Modified January 2006 by Tuukka Karvonen <tkarvone@iki.fi> and
 * Antti Haapakoski <antti.haapakoski@iki.fi>
 *  - Applied patches received from Ivan S. Dubrov
 *  - Disabled possible CRLF -> LFLF conversions in serial port initialization
 *  - Added minicom like serial port option setting if baud rate is configured.
 *    This was needed to get the options right on some platforms and to
 *    wake up some modems.
 *  - Added possibility to pass PIN code for modem in initialization
 *   (Sometimes WebBox modems seem to hang if PIN is given on a virtual channel)
 *  - Removed old code that was commented out
 *  - Added support for Unix98 scheme pseudo terminals (/dev/ptmx)
 *    and creation of symlinks for slave devices
 *  - Corrected logging of slave port names
 *  - at_command looks for AT/ERROR responses with findInBuf function instead
 *    of strstr function so that incoming carbage won't confuse it
 *
 * Modified March 2006 by Tuukka Karvonen <tkarvone@iki.fi>
 *  - Added -r option which makes the mux driver to restart itself in case
 *    the modem stops responding. This should make the driver more fault
 *    tolerant.
 *  - Some code restructuring that was required by the automatic restarting
 *  - buffer.c to use syslog instead of PDEBUG
 *  - fixed open_pty function to grant right for Unix98 scheme pseudo
 *    terminals even though symlinks are not in use
 *
 * New Usage:
 * gsmMuxd [options] <pty1> <pty2> ...
 *
 * To see the options, type:
 * ./gsmMuxd -h
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _GNU_SOURCE
// To get ptsname grandpt and unlockpt definitions from stdlib.h
#define _GNU_SOURCE
#endif
#include <features.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <string.h>
#include <paths.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
//syslog
#include <syslog.h>

#include "buffer.h"
#include "gsm0710.h"

extern int _debug;
extern int max_frame_size;
extern int serial_fd;
extern int faultTolerant;
extern int restart;

extern volatile int terminate;
extern Channel_Status *cstatus;
extern int terminateCount;
/** Writes a frame to a logical channel. C/R bit is set to 1.
 * Doesn't support FCS counting for UI frames.
 *
 * PARAMS:
 * channel - channel number (0 = control)

 * input   - the data to be written
 * count   - the length of the data
 * type    - the type of the frame (with possible P/F-bit)
 *
 * RETURNS:
 * number of characters written
 */
int write_frame(int channel, const char *input, int count, unsigned char type) {
	// flag, EA=1 C channel, frame type, length 1-2
	unsigned char prefix[5] = { F_FLAG, EA | CR, 0, 0, 0 };
	unsigned char postfix[2] = { 0xFF, F_FLAG };
	int prefix_length = 4, c;

	if (_debug)
		syslog(LOG_DEBUG, "send frame to ch: %d \n", channel);
	// EA=1, Command, let's add address
	prefix[1] = prefix[1] | ((63 & (unsigned char) channel) << 2);
	// let's set control field
	prefix[2] = type;

	// let's not use too big frames
	count = min(max_frame_size, count);

	// length
	if (count > 127) {
		prefix_length = 5;
		prefix[3] = ((127 & count) << 1);
		prefix[4] = (32640 & count) >> 7;
	} else {
		prefix[3] = 1 | (count << 1);
	}
	// CRC checksum
	postfix[0] = make_fcs(prefix + 1, prefix_length - 1);

	c = write(serial_fd, prefix, prefix_length);
	if (c != prefix_length) {
		if (_debug)
			syslog(LOG_DEBUG,
					"Couldn't write the whole prefix to the serial port for the virtual port %d. Wrote only %d  bytes.",
					channel, c);
		return 0;
	}
	if (count > 0) {
		c = write(serial_fd, input, count);
		if (count != c) {
			if (_debug)
				syslog(LOG_DEBUG,
						"Couldn't write all data to the serial port from the virtual port %d. Wrote only %d bytes.\n",
						channel, c);
			return 0;
		}
	}
	c = write(serial_fd, postfix, 2);
	if (c != 2) {
		if (_debug)
			syslog(LOG_DEBUG,
					"Couldn't write the whole postfix to the serial port for the virtual port %d. Wrote only %d bytes.",
					channel, c);
		return 0;
	}

	return count;
}

// Prints information on a frame
void print_frame(GSM0710_Frame * frame) {
	if (_debug) {
		syslog(LOG_DEBUG, "is in %s\n", __FUNCTION__);
		syslog(LOG_DEBUG, "Received ");
	}

	switch ((frame->control & ~PF)) {
	case SABM:
		if (_debug)
			syslog(LOG_DEBUG, "SABM ");
		break;
	case UIH:
		if (_debug)
			syslog(LOG_DEBUG, "UIH ");
		break;
	case UA:
		if (_debug)
			syslog(LOG_DEBUG, "UA ");
		break;
	case DM:
		if (_debug)
			syslog(LOG_DEBUG, "DM ");
		break;
	case DISC:
		if (_debug)
			syslog(LOG_DEBUG, "DISC ");
		break;
	case UI:
		if (_debug)
			syslog(LOG_DEBUG, "UI ");
		break;
	default:
		if (_debug)
			syslog(LOG_DEBUG, "unkown (control=%d) ", frame->control);
		break;
	}
	if (_debug)
		syslog(LOG_DEBUG, " frame for channel %d.\n", frame->channel);

	if (frame->data_length > 0) {
		if (_debug) {
			syslog(LOG_DEBUG, "frame->data = %s / size = %d\n", frame->data,
					frame->data_length);
			//fwrite(frame->data, sizeof(char), frame->data_length, stdout);
			syslog(LOG_DEBUG, "\n");
		}
	}

}

/* Handles commands received from the control channel.
 */
void handle_command(GSM0710_Frame * frame) {
#if 1
	unsigned char type, signals;
	int length = 0, i, type_length, channel, supported = 1;
	unsigned char *response;
	// struct ussp_operation op;

	if (_debug)
		syslog(LOG_DEBUG, "is in %s\n", __FUNCTION__);

	if (frame->data_length > 0) {
		type = frame->data[0];  // only a byte long types are handled now
		// skip extra bytes
		for (i = 0; (frame->data_length > i && (frame->data[i] & EA) == 0); i++)
			;
		i++;
		type_length = i;
		if ((type & CR) == CR) {
			// command not ack

			// extract frame length
			while (frame->data_length > i) {
				length = (length * 128) + ((frame->data[i] & 254) >> 1);
				if ((frame->data[i] & 1) == 1)
					break;
				i++;
			}
			i++;

			switch ((type & ~CR)) {
			case C_CLD:
				syslog(LOG_INFO,
						"The mobile station requested mux-mode termination.\n");
				if (faultTolerant) {
					// Signal restart
					restart = 1;
				} else {
					terminate = 1;
					terminateCount = -1;    // don't need to close down channels
				}
				break;
			case C_TEST:

				if(_debug){
					syslog(LOG_DEBUG,"Test command: ");
					syslog(LOG_DEBUG,"frame->data = %s  / frame->data_length = %d\n",frame->data + i, frame->data_length - i);
					//fwrite(frame->data + i, sizeof(char), frame->data_length - i, stdout);
				}

				break;
			case C_MSC:
				if (i + 1 < frame->data_length) {
					channel = ((frame->data[i] & 252) >> 2);
					i++;
					signals = (frame->data[i]);
					// op.op = USSP_MSC;
					// op.arg = USSP_RTS;
					// op.len = 0;

					if (_debug)
						syslog(LOG_DEBUG,
								"Modem status command on channel %d.\n",
								channel);
					if ((signals & S_FC) == S_FC) {
						if (_debug)
							syslog(LOG_DEBUG, "No frames allowed.\n");
					} else {
						// op.arg |= USSP_CTS;
						if (_debug)
							syslog(LOG_DEBUG, "Frames allowed.\n");
					}
					if ((signals & S_RTC) == S_RTC) {
						// op.arg |= USSP_DSR;
						if (_debug)
							syslog(LOG_DEBUG, "RTC\n");
					}
					if ((signals & S_IC) == S_IC) {
						// op.arg |= USSP_RI;
						if (_debug)
							syslog(LOG_DEBUG, "Ring\n");
					}
					if ((signals & S_DV) == S_DV) {
						// op.arg |= USSP_DCD;
						if (_debug)
							syslog(LOG_DEBUG, "DV\n");
					}
					// if (channel > 0)
					//     write(ussp_fd[(channel - 1)], &op, sizeof(op));
				} else {
					syslog(LOG_ERR,
							"ERROR: Modem status command, but no info. i: %d, len: %d, data-len: %d\n",
							i, length, frame->data_length);
				}
				break;
			default:
				syslog(LOG_ALERT,
						"Unknown command (%d) from the control channel.\n",
						type);
				response = malloc(sizeof(char) * (2 + type_length));
				response[0] = C_NSC;
				// supposes that type length is less than 128
				response[1] = EA & ((127 & type_length) << 1);
				i = 2;
				while (type_length--) {
					response[i] = frame->data[(i - 2)];
					i++;
				}
				write_frame(0, response, i, UIH);
				free(response);
				supported = 0;
				break;
			}

			if (supported) {
				// acknowledge the command
				frame->data[0] = frame->data[0] & ~CR;
				write_frame(0, frame->data, frame->data_length, UIH);
			}
		} else {
			// received ack for a command
			if (COMMAND_IS(C_NSC, type)) {
				syslog(LOG_ALERT,
						"The mobile station didn't support the command sent.\n");
			} else {
				if (_debug)
					syslog(LOG_DEBUG,
							"Command acknowledged by the mobile station.\n");
			}
		}
	}
#endif
}

/* Extracts and handles frames from the receiver buffer.
 *
 * PARAMS:
 * buf - the receiver buffer
 */
int extract_frames(GSM0710_Buffer * buf) {
	// version test for Siemens terminals to enable version 2 functions
	static char version_test[] = "\x23\x21\x04TEMUXVERSION2\0\0";
	int framesExtracted = 0;

	GSM0710_Frame *frame;

	if (_debug)
		syslog(LOG_DEBUG, "is in %s\n", __FUNCTION__);
	while ((frame = gsm0710_buffer_get_frame(buf))) {
		++framesExtracted;
		if ((FRAME_IS(UI, frame) || FRAME_IS(UIH, frame))) {
			if (_debug)
				syslog(LOG_DEBUG,
						"is (FRAME_IS(UI, frame) || FRAME_IS(UIH, frame))\n");
			if (frame->channel > 0) {
				if (_debug)
					syslog(LOG_DEBUG, "Sending data to DLC channel %d\n", frame->channel);
				// data from logical channel
				ussp_send_data(frame->data, frame->data_length,
						frame->channel - 1);
			} else {
				// control channel command
				if (_debug)
					syslog(LOG_DEBUG, "control channel command\n");
				handle_command(frame);
			}
		} else {
			// not an information frame
			if (_debug){
				syslog(LOG_DEBUG, "not an information frame\n");
				print_frame(frame);
			}
			switch ((frame->control & ~PF)) {
			case UA:
				if (_debug)
					syslog(LOG_DEBUG, "is FRAME_IS(UA, frame)\n");
				if (cstatus[frame->channel].opened == 1) {
					syslog(LOG_INFO, "Logical channel %d closed.\n",
							frame->channel);
					cstatus[frame->channel].opened = 0;
				} else {
					cstatus[frame->channel].opened = 1;
					if (frame->channel == 0) {
						syslog(LOG_INFO, "Control channel opened.\n");
						// send version Siemens version test
						write_frame(0, version_test, 18, UIH);
					} else {
						syslog(LOG_INFO, "Logical channel %d opened.\n",
								frame->channel);
					}
				}
				break;
			case DM:
				if (cstatus[frame->channel].opened) {
					syslog(LOG_INFO,
							"DM received, so the channel %d was already closed.\n",
							frame->channel);
					cstatus[frame->channel].opened = 0;
				} else {
					if (frame->channel == 0) {
						syslog(LOG_INFO,
								"Couldn't open control channel.\n->Terminating.\n");
						terminate = 1;
						terminateCount = -1;    // don't need to close channels
					} else {
						syslog(LOG_INFO,
								"Logical channel %d couldn't be opened.\n",
								frame->channel);
					}
				}
				break;
			case DISC:
				if (cstatus[frame->channel].opened) {
					cstatus[frame->channel].opened = 0;
					write_frame(frame->channel, NULL, 0, UA | PF);
					if (frame->channel == 0) {
						syslog(LOG_INFO, "Control channel closed.\n");
						if (faultTolerant) {
							restart = 1;
						} else {
							terminate = 1;
							terminateCount = -1; // don't need to close channels
						}
					} else {
						syslog(LOG_INFO, "Logical channel %d closed.\n",
								frame->channel);
					}
				} else {
					// channel already closed
					syslog(LOG_INFO,
							"Received DISC even though channel %d was already closed.\n",
							frame->channel);
					write_frame(frame->channel, NULL, 0, DM | PF);
				}
				break;
			case SABM:
				// channel open request
				if (cstatus[frame->channel].opened == 0) {
					if (frame->channel == 0) {
						syslog(LOG_INFO, "Control channel opened.\n");
					} else {
						syslog(LOG_INFO, "Logical channel %d opened.\n",
								frame->channel);
					}
				} else {
					// channel already opened
					syslog(LOG_INFO,
							"Received SABM even though channel %d was already closed.\n",
							frame->channel);
				}
				cstatus[frame->channel].opened = 1;
				write_frame(frame->channel, NULL, 0, UA | PF);
				break;
			}
		}

		destroy_frame(frame);
	}
	if (_debug)
		syslog(LOG_DEBUG, "out of %s; framesExtracted: %d\n", __FUNCTION__, framesExtracted);
	return framesExtracted;
}
