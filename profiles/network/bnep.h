/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

int bnep_init(void);
int bnep_cleanup(void);

uint16_t bnep_service_id(const char *svc);
const char *bnep_uuid(uint16_t id);
const char *bnep_name(uint16_t id);

int bnep_kill_connection(const bdaddr_t *dst);

int bnep_connadd(int sk, uint16_t role, char *dev);
int bnep_if_up(const char *devname);
int bnep_if_down(const char *devname);
int bnep_add_to_bridge(const char *devname, const char *bridge);
int bnep_del_from_bridge(const char *devname, const char *bridge);

typedef void (*bnep_connect_cb) (GIOChannel *chan, char *iface, int err,
								void *data);
int bnep_connect(int sk, uint16_t src, uint16_t dst, bnep_connect_cb conn_cb,
								void *data);
