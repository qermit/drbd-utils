/*
   drbdadm_postparse.c actions to do after config parsing

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2012, LINBIT Information Technologies GmbH

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "drbdtool_common.h"
#include "drbdadm.h"

static void inherit_volumes(struct volumes *from, struct d_host_info *host);
static void check_volume_sets_equal(struct d_resource *, struct d_host_info *, struct d_host_info *);
static void expand_opts(struct options *common, struct options *options);

static void append_names(struct names *head, struct names *to_copy)
{
	struct d_name *new, *copy;

	STAILQ_FOREACH(copy, to_copy, link) {
		new = malloc(sizeof(struct d_name));
		new->name = strdup(copy->name);
		insert_tail(head, new);
	}
}

void set_on_hosts_in_res(struct d_resource *res)
{
	struct d_resource *l_res;
	struct d_host_info *host, *host2;
	struct d_name *h;

	for_each_host(host, &res->all_hosts) {
		if (host->lower_name) {
			for_each_resource(l_res, &config) {
				if (!strcmp(l_res->name, host->lower_name))
					break;
			}

			if (l_res == NULL) {
				err("%s:%d: in resource %s, "
				    "referenced resource '%s' not defined.\n",
				    res->config_file, res->start_line,
				    res->name, host->lower_name);
				config_valid = 0;
				continue;
			}

			/* Simple: host->on_hosts = concat_names(l_res->me->on_hosts, l_res->peer->on_hosts); */
			for_each_host(host2, &l_res->all_hosts)
				if (!host2->lower_name) {
					append_names(&host->on_hosts, &host2->on_hosts);

					for_each_host(h, &host2->on_hosts) {
						struct d_volume *vol;

						for_each_volume(vol, &host->volumes)
							check_uniq("device-minor", "device-minor:%s:%u", h->name,
                                                                  vol->device_minor);

                                               for_each_volume(vol, &host->volumes)
                                                       if (vol->device)
                                                               check_uniq("device", "device:%s:%s", h->name,
                                                                          vol->device);
                                       }
				}

			host->lower = l_res;

			/* */
			if (addr_scope_local(host->address.addr))
				STAILQ_FOREACH(h, &host->on_hosts, link)
					check_uniq("IP", "%s:%s:%s", h->name, host->address.addr, host->address.port);

		}
	}
}

struct d_host_info *find_host_info_by_name(struct d_resource* res, char *name)
{
	struct d_host_info *host;

	for_each_host(host, &res->all_hosts)
		if (hostname_in_list(name, &host->on_hosts))
			return host;

	return NULL;
}

static struct d_host_info *find_host_info_by_address(struct d_resource* res, struct d_address *address)
{
	struct d_host_info *host;

	for_each_host(host, &res->all_hosts)
		if (!strcmp(host->address.addr, address->addr) &&
		    !strcmp(host->address.af, address->af) &&
		    !strcmp(host->address.port, address->port))
			return host;

	return NULL;
}

static bool generate_implicit_node_id(int *addr_hash, struct d_host_info **host_info_array)
{
	if (addr_hash[0] > addr_hash[1]) {
		host_info_array[0]->node_id = strdup("0");
		host_info_array[1]->node_id = strdup("1");
	} else if (addr_hash[0] < addr_hash[1]) {
		host_info_array[0]->node_id = strdup("1");
		host_info_array[1]->node_id = strdup("0");
	} else {
		return false;
	}
	return true;
}

static void _set_host_info_in_host_address_pairs(struct d_resource *res,
						 struct connection *conn,
						 struct path *path)
{
	struct hname_address *ha;
	struct d_host_info *host_info;
	int addr_hash[2], i = 0;
	struct d_host_info *host_info_array[2];

	STAILQ_FOREACH(ha, &path->hname_address_pairs, link) {
		if (ha->host_info) { /* Implicit connection have that already set. */
			host_info = ha->host_info;
			if (i == 2) {
				err("LOGIC BUG in set_host_info_in_host_address_pairs()\n");
				exit(20);
			}
			if (!host_info->address.addr) {
				err("\"connection-mesh\" (for \"%s\") with a host (\"%s\") "
						"that has no \"address\" defined\n",
						res->name, ha->name);
				config_valid = 0;
				return;
			}
			addr_hash[i] = crc32c(0x1a656f21,
					(void *)host_info->address.addr,
					strlen(host_info->address.addr));
			host_info_array[i++] = host_info;
		} else if (ha->by_address) {
			host_info = find_host_info_by_address(res, &ha->address);
			/* The name will be used for nice comments only ... */
			ha->name = strdup(names_to_str_c(&host_info->on_hosts, '_'));
		} else {
			host_info = find_host_info_by_name(res, ha->name);
		}
		if (!host_info && !strcmp(ha->name, "_remote_host")) {
			if (conn->peer)
				host_info = conn->peer; /* With new format we create one for _peer_node_id */
			else
				continue; /* Old drbdsetup does not houtput a host section */
		}

		if (!host_info) {
			err("%s:%d: in resource %s a hostname (\"%s\") is given\n"
			    "with a \"host\" keyword, has no \"address\" keyword, and not matching\n"
			    "host section (\"on\" keyword)\n",
			    config_file, ha->config_line, res->name, ha->name);
			config_valid = 0;
			/* Can't continue. */
			return;
		}
		ha->host_info = host_info;
		if (!(ha->address.addr && ha->address.af && ha->address.port)) {
			bool have_address = true;
			bool have_port = true;

			if (!(ha->address.addr && ha->address.af)) {
				if (host_info->address.addr && host_info->address.af) {
					ha->address.addr = host_info->address.addr;
					ha->address.af = host_info->address.af;
				} else
					have_address = false;
			}
			if (!ha->address.port) {
				if (host_info->address.port)
					ha->address.port = host_info->address.port;
				else
					have_port = false;
			}
			if (!(have_address && have_port)) {
				err("%s:%d: Resource %s, host %s: "
				    "cannot determine which %s%s%s to use\n",
				    config_file, ha->config_line, res->name,
				    ha->name, have_address ? "" : "address",
				    have_address != have_port ? "" : " and ",
				    have_port ? "" : "port");
				config_valid = 0;
			}
		}

		fline = ha->config_line;
	}

	if (conn->implicit && i == 2 && !host_info_array[0]->node_id && !host_info_array[1]->node_id) {
		/* This is drbd-8.3 / drbd-8.4 compatibility, auto created node-id */
		bool have_node_ids;

		have_node_ids = generate_implicit_node_id(addr_hash, host_info_array);

		if (!have_node_ids) {
			/* That might be a config with equal node addresses, since it is
			  127.0.0.1:xxx with a proxy... */
			i = 0;
			path = STAILQ_FIRST(&conn->paths); /* there may only be one */
			STAILQ_FOREACH(ha, &path->hname_address_pairs, link) {
				if (!ha->host_info)
					continue;

				if (!ha->proxy)
					break;

				addr_hash[i++] = crc32c(0x1a656f21,
							(void *)ha->proxy->outside.addr,
							strlen(ha->proxy->outside.addr));
			}
			have_node_ids = generate_implicit_node_id(addr_hash, host_info_array);
		}
		if (!have_node_ids) {
			err("BAD LUCK, equal hashes\n");
			exit(20);
		}
	}
}

static void set_host_info_in_host_address_pairs(struct d_resource *res, struct connection *conn)
{
	struct path *path;

	for_each_path(path, &conn->paths)
		_set_host_info_in_host_address_pairs(res, conn, path);
}

static bool test_proxy_on_host(struct d_resource* res, struct d_host_info *host)
{
	struct connection *conn;
	struct path *path;
	for_each_connection(conn, &res->connections) {
		struct hname_address *ha;

		for_each_path(path, &conn->paths) {
			STAILQ_FOREACH(ha, &path->hname_address_pairs, link) {
				if (!ha->proxy)
					continue;
				if (ha->host_info == host) {
					return hostname_in_list(hostname, &ha->proxy->on_hosts);
				}
			}
		}
	}
	return false;
}

void set_me_in_resource(struct d_resource* res, int match_on_proxy)
{
	struct d_host_info *host;
	struct connection *conn;

	/* Determine the local host section */
	for_each_host(host, &res->all_hosts) {
		/* do we match  this host? */
		if (match_on_proxy) {
			if (!test_proxy_on_host(res, host))
			       continue;
		} else if (host->by_address) {
			if (!have_ip(host->address.af, host->address.addr) &&
				/* for debugging only, e.g. __DRBD_NODE__=10.0.0.1 */
			    strcmp(hostname, host->address.addr))
				continue;
		} else if (host->lower) {
			if (!host->lower->me)
				continue;
		} else if (STAILQ_EMPTY(&host->on_hosts)) {
			/* huh? a resource without hosts to run on?! */
			continue;
		} else {
			if (!hostname_in_list(hostname, &host->on_hosts) &&
			    strcmp("_this_host", STAILQ_FIRST(&host->on_hosts)->name))
				continue;
		}
		/* we matched. */
		if (res->ignore) {
			config_valid = 0;
			err("%s:%d: in resource %s, %s %s { ... }:\n"
			    "\tYou cannot ignore and define at the same time.\n",
			    res->config_file, host->config_line, res->name,
			    host->lower ? "stacked-on-top-of" : "on",
			    host->lower ? host->lower->name : names_to_str(&host->on_hosts));
		}
		if (res->me && res->me != host) {
			config_valid = 0;
			err("%s:%d: in resource %s, %s %s { ... } ... %s %s { ... }:\n"
			    "\tThere are multiple host sections for this node.\n",
			    res->config_file, host->config_line, res->name,
			    res->me->lower ? "stacked-on-top-of" : "on",
			    res->me->lower ? res->me->lower->name : names_to_str(&res->me->on_hosts),
			    host->lower ? "stacked-on-top-of" : "on",
			    host->lower ? host->lower->name : names_to_str(&host->on_hosts));
		}
		res->me = host;
		host->used_as_me = 1;
		if (host->lower)
			res->stacked = 1;
	}

	/* If there is no me, implicitly ignore that resource */
	if (!res->me) {
		res->ignore = 1;
		return;
	}

	/* set con->my_address in every path in every connection */
	for_each_connection(conn, &res->connections) {
		struct path *path;
		struct hname_address *h = NULL;

		for_each_path(path, &conn->paths) {
			STAILQ_FOREACH(h, &path->hname_address_pairs, link) {
				if (h->host_info == res->me)
					break;
			}

			if (h) {
				h->used_as_me = 1;
				if (!path->my_address)
					path->my_address = h->address.addr ? &h->address : &res->me->address;
				path->my_proxy = h->proxy;
			} else {
				if (!conn->peer) /* Keep w/o addresses form "drbdsetup show" for adjust */
					conn->ignore = 1;
			}
		}
	}
}


static void set_peer_in_connection(struct d_resource* res, struct connection *conn, int peer_required)
{
	struct hname_address *host = NULL, *candidate = NULL;
	struct d_host_info *host_info;
	struct path *path;

	if (res->ignore || conn->ignore)
		return;

	/* me must be already set */
	if (!res->me) {
		/* should have been implicitly ignored. */
		err("%s:%d: in resource %s:\n"
		    "\tcannot determine the peer, don't even know myself!\n",
		    res->config_file, res->start_line, res->name);
		exit(E_THINKO);
	}

	for_each_path(path, &conn->paths) {
		int nr_hosts = 0, candidates = 0;

		STAILQ_FOREACH(host, &path->hname_address_pairs, link) {
			nr_hosts++;
			if (!host->used_as_me) {
				candidates++;
				candidate = host;
			}
		}

		if (nr_hosts < 2) {
			if (peer_required) {
				err("%s:%d: in connection in resource %s:\n"
				    "\tMissing statement 'host <PEER> '.\n",
				    res->config_file, conn->config_line, res->name);
				config_valid = 0;
			}
			return;
		}

		if (candidates == 1 && nr_hosts == 2) {
			if (conn->peer) {
				host_info = conn->peer;
			} else {
				host_info = find_host_info_by_name(res, candidate->name);
				conn->peer = host_info;
			}
			path->peer_address = candidate->address.addr ? &candidate->address : &host_info->address;
			path->peer_proxy = candidate->proxy;
			path->connect_to = path->my_proxy ? &path->my_proxy->inside : path->peer_address;
			continue;
		}

		err("%s:%d: in connection in resource %s:\n"
		    "\tBug in set_peer_in_connection()\n",
		    res->config_file, conn->config_line, res->name);
		config_valid = 0;
		return;
	}
}

void create_implicit_net_options(struct connection *conn)
{
	char *value;

	if (find_opt(&conn->net_options, "_name"))
		return;

	if (conn->name)
		value = conn->name;
	else if (conn->peer)
		value = names_to_str_c(&conn->peer->on_hosts, '_');
	else
		return;

	insert_head(&conn->net_options, new_opt(strdup("_name"), strdup(value)));
}

void set_peer_in_resource(struct d_resource* res, int peer_required)
{
	struct connection *conn;
	int peers_addrs_set = 1;

	for_each_connection(conn, &res->connections) {
		struct path *path;
		set_peer_in_connection(res, conn, peer_required);

		for_each_path(path, &conn->paths) {
			if (!path->peer_address)
				peers_addrs_set = 0;
		}
		create_implicit_net_options(conn);
	}
	res->peers_addrs_set = peers_addrs_set;
}

void set_disk_in_res(struct d_resource *res)
{
	struct d_host_info *host;
	struct d_volume *a, *b;

	if (res->ignore)
		return;

	for_each_host(host, &res->all_hosts) {
		if (!host->lower)
			continue;

		if (host->lower->ignore)
			continue;

		check_volume_sets_equal(res, host, host->lower->me);
		if (!config_valid)
			/* don't even bother for broken config. */
			continue;

		/* volume lists are sorted on vnr */
		a = STAILQ_FIRST(&host->volumes);
		b = STAILQ_FIRST(&host->lower->me->volumes);
		while (a) {
			while (b && a->vnr > b->vnr) {
				/* Lower resource has more volumes.
				 * Probably unusual, but we decided
				 * that it should be legal.
				 * Skip those that do not match */
				b = STAILQ_NEXT(b, link);
			}
			if (a && b && a->vnr == b->vnr) {
				if (b->device)
					m_asprintf(&a->disk, "%s", b->device);
				else
					m_asprintf(&a->disk, "/dev/drbd%u", b->device_minor);
				/* stacked implicit volumes need internal meta data, too */
				if (!a->meta_disk)
					m_asprintf(&a->meta_disk, "internal");
				if (!a->meta_index)
					m_asprintf(&a->meta_index, "internal");
				a = STAILQ_NEXT(a, link);
				b = STAILQ_NEXT(b, link);
			} else {
				/* config_invalid should have been set
				 * by check_volume_sets_equal */
				assert(0);
			}
		}
	}
}

static struct d_volume *find_volume(struct volumes *volumes, int vnr)
{
	struct d_volume *vol;

	for_each_volume(vol, volumes)
		if (vol->vnr == vnr)
			return vol;

	return NULL;
}

static void derror(struct d_host_info *host, struct d_resource *res, char *text)
{
	config_valid = 0;
	err("%s:%d: in resource %s, on %s { ... }: '%s' keyword missing.\n",
	    res->config_file, host->config_line, res->name,
	    names_to_str(&host->on_hosts), text);
}

static void inherit_volumes(struct volumes *from, struct d_host_info *host)
{
	struct d_volume *s, *t;
	struct d_name *h;

	for_each_volume(s, from) {
		t = find_volume(&host->volumes, s->vnr);
		if (!t) {
			t = alloc_volume();
			t->device_minor = -1;
			t->vnr = s->vnr;
			insert_volume(&host->volumes, t);
		}
		if (!t->disk && s->disk) {
			t->disk = strdup(s->disk);
			STAILQ_FOREACH(h, &host->on_hosts, link)
				check_uniq("disk", "disk:%s:%s", h->name, t->disk);
		}
		if (!t->device && s->device)
			t->device = strdup(s->device);
		if (t->device_minor == -1U && s->device_minor != -1U)
			t->device_minor = s->device_minor;
		if (!t->meta_disk && s->meta_disk) {
			t->meta_disk = strdup(s->meta_disk);
			if (s->meta_index)
				t->meta_index = strdup(s->meta_index);
		}
	}
}

static void check_volume_complete(struct d_resource *res, struct d_host_info *host, struct d_volume *vol)
{
	if (!vol->device && vol->device_minor == -1U)
		derror(host, res, "device");
	if (vol->disk || vol->meta_disk || vol->meta_index) {
		if (!(vol->disk && !strcmp(vol->disk, "none"))) {
			if (!vol->disk)
				derror(host, res, "disk");
			if (!vol->meta_disk)
				derror(host, res, "meta-disk");
			if (!vol->meta_index)
				derror(host, res, "meta-index");
		}
	}
}

static void check_volumes_complete(struct d_resource *res, struct d_host_info *host)
{
	struct d_volume *vol;
	unsigned vnr = -1U;

	for_each_volume(vol, &host->volumes) {
		if (vnr == -1U || vnr < vol->vnr)
			vnr = vol->vnr;
		else
			err("internal error: in %s: unsorted volumes list\n",
			    res->name);
		check_volume_complete(res, host, vol);
	}
}

static void check_meta_disk(struct d_volume *vol, struct d_host_info *host)
{
	struct d_name *h;
	/* when parsing "drbdsetup show[-all]" output,
	 * a detached volume will only have device/minor,
	 * but no disk or meta disk. */
	if (vol->meta_disk == NULL)
		return;
	if (strcmp(vol->meta_disk, "internal") != 0) {
		/* index either some number, or "flexible" */
		STAILQ_FOREACH(h, &host->on_hosts, link)
			check_uniq("meta-disk", "%s:%s[%s]", h->name, vol->meta_disk, vol->meta_index);
	}
}

static void check_volume_sets_equal(struct d_resource *res, struct d_host_info *host1, struct d_host_info *host2)
{
	struct d_volume *a, *b;

	/* change the error output, if we have been called to
	 * compare stacked with lower resource volumes */
	int compare_stacked = host1->lower && host1->lower->me == host2;

	if (host1 == host2)
		return;

	a = STAILQ_FIRST(&host1->volumes);
	b = STAILQ_FIRST(&host2->volumes);

	/* volume lists are supposed to be sorted on vnr */
	while (a || b) {
		while (a && (!b || a->vnr < b->vnr)) {
			err("%s:%d: in resource %s, on %s { ... }: volume %d not defined on %s\n",
			    config_file, line, res->name,
			    names_to_str(&host1->on_hosts), a->vnr,
			    compare_stacked ? host1->lower->name : names_to_str(&host2->on_hosts));
			a = STAILQ_NEXT(a, link);
			config_valid = 0;
		}
		while (b && (!a || a->vnr > b->vnr)) {
			/* Though unusual, it is "legal" for a lower resource
			 * to have more volumes than the resource stacked on
			 * top of it.  Warn (if we have a terminal),
			 * but consider it as valid. */
			if (!(compare_stacked && no_tty))
				err("%s:%d: in resource %s, on %s { ... }: "
				    "volume %d missing (present on %s)\n",
				    config_file, line, res->name,
				    names_to_str(&host1->on_hosts), b->vnr,
				    compare_stacked ? host1->lower->name : names_to_str(&host2->on_hosts));
			if (!compare_stacked)
				config_valid = 0;
			b = STAILQ_NEXT(b, link);
		}
		if (a && b && a->vnr == b->vnr) {
			a = STAILQ_NEXT(a, link);
			b = STAILQ_NEXT(b, link);
		}
	}
}

/* Ensure that in all host sections the same volumes are defined */
static void check_volumes_hosts(struct d_resource *res)
{
	struct d_host_info *host1, *host2;

	host1 = STAILQ_FIRST(&res->all_hosts);

	if (!host1)
		return;

	for_each_host(host2, &res->all_hosts)
		check_volume_sets_equal(res, host1, host2);
}

static struct hname_address *alloc_hname_address()
{
	struct hname_address *ha;

	ha = calloc(1, sizeof(struct hname_address));
	if (ha == NULL) {
		err("calloc", ": %m\n");
		exit(E_EXEC_ERROR);
	}
	return ha;
}

static void create_implicit_connections(struct d_resource *res)
{
	struct connection *conn;
	struct path *path;
	struct hname_address *ha;
	struct d_host_info *host_info;
	int hosts = 0;

	if (!STAILQ_EMPTY(&res->connections))
		return;

	conn = alloc_connection();
	conn->implicit = 1;
	path = alloc_path();
	path->implicit = 1;
	insert_tail(&conn->paths, path);

	for_each_host(host_info, &res->all_hosts) {
		if (++hosts == 3) {
			err("Resource %s:\n\t"
			    "Use explicit 'connection' sections with more than two 'on' sections.\n",
		            res->name);
			break;
		}
		if (host_info->address.af && host_info->address.addr && host_info->address.port) {
			ha = alloc_hname_address();
			ha->host_info = host_info;
			ha->proxy = host_info->proxy_compat_only;
			if (!host_info->lower) {
				ha->name = STAILQ_FIRST(&host_info->on_hosts)->name;
			} else {
				ha->name = strdup(names_to_str_c(&host_info->on_hosts, '_'));
				ha->address = host_info->address;
				ha->faked_hostname = 1;
				ha->parsed_address = 1; /* not true, but makes dump nicer */
			}
			STAILQ_INSERT_TAIL(&path->hname_address_pairs, ha, link);
		}
	}

	if (hosts == 2)
		STAILQ_INSERT_TAIL(&res->connections, conn, link);
	else
		free_connection(conn);
}

static struct d_host_info *find_host_info_or_invalid(struct d_resource *res, char *name)
{
	struct d_host_info *host_info = find_host_info_by_name(res, name);

	if (!host_info) {
		err("%s:%d: in resource %s:\n\t"
		    "There is no 'on' section for hostname '%s' named in the connection-mesh\n",
		    res->config_file, res->start_line, res->name, name);
		config_valid = 0;
	}

	return host_info;
}

static void create_connections_from_mesh(struct d_resource *res, struct mesh *mesh)
{
	struct d_name *hname1, *hname2;
	struct d_host_info *hi1, *hi2;

	for_each_host(hname1, &mesh->hosts) {
		hi1 = find_host_info_or_invalid(res, hname1->name);
		if (!hi1)
			return;
		hname2 = STAILQ_NEXT(hname1, link);
		while (hname2) {
			struct hname_address *ha;
			struct connection *conn;
			struct path *path;

			hi2 = find_host_info_or_invalid(res, hname2->name);
			if (!hi2)
				return;

			if (hi1 == hi2)
				goto skip;

			conn = alloc_connection();
			conn->implicit = 1;
			path = alloc_path();
			path->implicit = 1;
			insert_tail(&conn->paths, path);

			expand_opts(&mesh->net_options, &conn->net_options);

			ha = alloc_hname_address();
			ha->host_info = hi1;
			ha->name = STAILQ_FIRST(&hi1->on_hosts)->name;
			STAILQ_INSERT_TAIL(&path->hname_address_pairs, ha, link);

			ha = alloc_hname_address();
			ha->host_info = hi2;
			ha->name = STAILQ_FIRST(&hi2->on_hosts)->name;
			STAILQ_INSERT_TAIL(&path->hname_address_pairs, ha, link);

			STAILQ_INSERT_TAIL(&res->connections, conn, link);
		skip:
			hname2 = STAILQ_NEXT(hname2, link);
		}
	}
}

static bool addresses_equal(struct d_address *addr1, struct d_address *addr2)
{
	if (strcmp(addr1->af, addr2->af))
		return false;

	if (strcmp(addr1->addr, addr2->addr))
		return false;

	if (strcmp(addr1->port, addr2->port))
		return false;

	return true;
}

static struct hname_address *find_hname_addr_in_res(struct d_resource *res, struct d_address *addr)
{
	struct hname_address *ha;
	struct connection *conn;
	struct path *path;

	for_each_connection(conn, &res->connections) {
		for_each_path(path, &conn->paths) {
			STAILQ_FOREACH(ha, &path->hname_address_pairs, link) {
				struct d_address *addr2;
				addr2 = ha->address.addr ? &ha->address : &ha->host_info->address;

				if (addresses_equal(addr, addr2))
					return ha;
			}
		}
	}

	return NULL;
}

/* An AF/IP/addr triple might be used by multiple connections within one resource,
   but may not be mentioned in any other resource. Also make sure that the two
   endpoints are not configured as the same.
 */
static void check_addr_conflict(struct d_resource *res, struct resources *resources)
{
	struct d_resource *res2;
	struct hname_address *ha1, *ha2;
	struct connection *conn;

	for_each_resource(res2, resources) {
		if (res2 == res)
			continue;

		for_each_connection(conn, &res->connections) {
			struct path *path;

			for_each_path(path, &conn->paths) {
				struct d_address *addr[2];
				int i = 0;

				STAILQ_FOREACH(ha1, &path->hname_address_pairs, link) {
					addr[i] = ha1->address.addr ? &ha1->address : &ha1->host_info->address;
					if (addr_scope_local(addr[i]->addr))
						continue;

					if (ha1->conflicts)
						continue;

					ha2 = find_hname_addr_in_res(res2, addr[i]);
					if (!ha2)
						continue;

					if (ha2->conflicts)
						continue;

					fprintf(stderr, "%s:%d: in resource %s\n"
						"    %s:%s:%s is also used %s:%d (resource %s)\n",
						res->config_file, ha1->config_line, res->name,
						addr[i]->af, addr[i]->addr, addr[i]->port,
						res2->config_file, ha2->config_line, res2->name);
					ha2->conflicts = 1;
					ha1->conflicts = 1;
					config_valid = 0;
					i++;
				}
				if (i == 2 && addresses_equal(addr[0], addr[1]) &&
				    !addr_scope_local(addr[0]->addr)) {
					err("%s:%d: in resource %s %s:%s:%s is used for both endpoints\n",
					    res->config_file, conn->config_line,
					    res->name, addr[0]->af, addr[0]->addr,
					    addr[0]->port);
					config_valid = 0;
				}
			}
		}
	}
}

static void _must_have_two_hosts(struct d_resource *res, struct path *path)
{
	struct hname_address *ha;
	int i = 0;

	STAILQ_FOREACH(ha, &path->hname_address_pairs, link)
		i++;
	if (i != 2) {
		err("%s:%d: Resource %s: %s needs to have two endpoints\n",
		    res->config_file, path->config_line, res->name,
		    path->implicit ? "connection" : "path");
		config_valid = 0;
	}
}

static void must_have_two_hosts(struct d_resource *res, struct connection *conn)
{
	struct path *path;

	for_each_path(path, &conn->paths)
		_must_have_two_hosts(res, path);
}

struct peer_device *find_peer_device(struct connection *conn, int vnr)
{
	struct peer_device *peer_device;

	STAILQ_FOREACH(peer_device, &conn->peer_devices, connection_link) {
		if (peer_device->vnr == vnr)
			return peer_device;
	}

	return NULL;
}

static void fixup_peer_devices(struct d_resource *res)
{
	struct connection *conn;
	struct d_host_info *some_host = STAILQ_FIRST(&res->all_hosts);
	/* At this point all hosts of the resource have the same set of volumes */

	for_each_connection(conn, &res->connections) {
		struct peer_device *peer_device;
		struct d_volume *vol;

		STAILQ_FOREACH(peer_device, &conn->peer_devices, connection_link) {

			vol = volume_by_vnr(&some_host->volumes, peer_device->vnr);
			if (!vol) {
				err("%s:%d: Resource %s: There is a reference to a volume %d that"
				    "is not known in this resource\n",
				    res->config_file, peer_device->config_line, res->name,
				    peer_device->vnr);
				config_valid = 0;
			}
			peer_device->volume = vol;
			STAILQ_INSERT_TAIL(&vol->peer_devices, peer_device, volume_link);
		}
		for_each_volume(vol, &some_host->volumes) {
			peer_device = find_peer_device(conn, vol->vnr);
			if (peer_device)
				continue;
			peer_device = alloc_peer_device();
			peer_device->vnr = vol->vnr;
			peer_device->implicit = 1;
			peer_device->connection = conn;
			peer_device->volume = vol;
			STAILQ_INSERT_TAIL(&conn->peer_devices, peer_device, connection_link);
			STAILQ_INSERT_TAIL(&vol->peer_devices, peer_device, volume_link);
		}
	}
}

void post_parse(struct resources *resources, enum pp_flags flags)
{
	struct d_resource *res;
	struct connection *con;

	/* inherit volumes from resource level into the d_host_info objects */
	for_each_resource(res, resources) {
		struct d_host_info *host;
		for_each_host(host, &res->all_hosts) {
			struct d_volume *vol;
			inherit_volumes(&res->volumes, host);

			for_each_volume(vol, &host->volumes)
				check_meta_disk(vol, host);

			if (host->require_minor)
				check_volumes_complete(res, host);
		}

		check_volumes_hosts(res);
	}

	for_each_resource(res, resources)
		if (res->stacked_on_one)
			set_on_hosts_in_res(res); /* sets on_hosts and host->lower */

	for_each_resource(res, resources) {
		struct d_host_info *host;
		struct mesh *mesh;

		if (!(flags & DRBDSETUP_SHOW)) {
			for_each_connection(con, &res->connections)
				must_have_two_hosts(res, con);
		}

		/* Other steps make no sense. */
		if (!config_valid)
			continue;

		STAILQ_FOREACH(mesh, &res->meshes, link)
			create_connections_from_mesh(res, mesh);
		create_implicit_connections(res);
		for_each_connection(con, &res->connections)
			set_host_info_in_host_address_pairs(res, con);
		for_each_host(host, &res->all_hosts) {
			if (!host->node_id)
				derror(host, res, "node-id");
		}
	}

	if (config_valid) {
		for_each_resource(res, resources)
			check_addr_conflict(res, resources);
	}

	/* Needs "on_hosts" and host->lower already set */
	for_each_resource(res, resources)
		if (!res->stacked_on_one)
			set_me_in_resource(res, flags & MATCH_ON_PROXY);

	/* Needs host->lower->me already set */
	for_each_resource(res, resources)
		if (res->stacked_on_one)
			set_me_in_resource(res, flags & MATCH_ON_PROXY);

	// Needs "me" set already
	for_each_resource(res, resources)
		if (res->stacked_on_one)
			set_disk_in_res(res);

	for_each_resource(res, resources)
		fixup_peer_devices(res);
}

static void expand_opts(struct options *common, struct options *options)
{
	struct d_option *option, *new_option;

	STAILQ_FOREACH(option, common, link) {
		if (!find_opt(options, option->name)) {
			new_option = new_opt(strdup(option->name),
					     option->value ? strdup(option->value) : NULL);
			insert_head(options, new_option);
		}
	}
}

void expand_common(void)
{
	struct d_resource *res;
	struct d_volume *vol, *host_vol;
	struct d_host_info *h;
	struct connection *conn;
	struct d_resource *template;

	for_each_resource(res, &config) {
		/* make sure vol->device is non-NULL */
		for_each_host(h, &res->all_hosts) {
			for_each_volume(vol, &h->volumes) {
				if (vol->disk && !strcmp(vol->disk, "none")) {
					free(vol->disk);
					free(vol->meta_disk);
					free(vol->meta_index);
					vol->disk = NULL;
					vol->meta_disk = NULL;
					vol->meta_index = NULL;
				}

				if (!vol->device)
					m_asprintf(&vol->device, "/dev/drbd%u",
						   vol->device_minor);
			}
		}

		if (res->template)
			template = res->template;
		else
			template = common;

		if (template) {
			expand_opts(&template->net_options, &res->net_options);
			expand_opts(&template->disk_options, &res->disk_options);
			expand_opts(&template->pd_options, &res->pd_options);
			expand_opts(&template->startup_options, &res->startup_options);
			expand_opts(&template->proxy_options, &res->proxy_options);
			expand_opts(&template->handlers, &res->handlers);
			expand_opts(&template->res_options, &res->res_options);

			if (template->stacked_timeouts)
				res->stacked_timeouts = 1;

			expand_opts(&template->proxy_plugins, &res->proxy_plugins);
		}

		/* now that common disk options (if any) have been propagated to the
		 * resource level, further propagate them to the volume level. */
		for_each_host(h, &res->all_hosts) {
			for_each_volume(vol, &h->volumes) {
				expand_opts(&res->disk_options, &vol->disk_options);
				expand_opts(&res->pd_options, &vol->pd_options);
			}
		}

		/* now from all volume/disk-options on resource level to host level */
		for_each_volume(vol, &res->volumes) {
			for_each_host(h, &res->all_hosts) {
				host_vol = volume_by_vnr(&h->volumes, vol->vnr);
				expand_opts(&vol->disk_options, &host_vol->disk_options);
				expand_opts(&vol->pd_options, &host_vol->pd_options);
			}
		}

		/* inherit network options from resource objects into connection objects */
		for_each_connection(conn, &res->connections)
			expand_opts(&res->net_options, &conn->net_options);

		/* inherit proxy options from resource to the proxies in the connections */
		for_each_connection(conn, &res->connections) {
			struct path *path;
			for_each_path(path, &conn->paths) {
				struct hname_address *ha;
				STAILQ_FOREACH(ha, &path->hname_address_pairs, link) {
					if (!ha->proxy)
						continue;

					expand_opts(&res->proxy_options, &ha->proxy->options);
					expand_opts(&res->proxy_plugins, &ha->proxy->plugins);
				}
			}
		}

		/* inherit peer_device options from connections to peer_devices AND
		   tie the peer_device options from the volume to peer_devices */
		for_each_connection(conn, &res->connections) {
			struct peer_device *peer_device;
			STAILQ_FOREACH(peer_device, &conn->peer_devices, connection_link) {
				expand_opts(&conn->pd_options, &peer_device->pd_options);
				expand_opts(&peer_device->volume->pd_options, &peer_device->pd_options);
			}
		}
	}
}

struct d_resource *res_by_name(const char *name)
{
	struct d_resource *res;

	for_each_resource(res, &config) {
		if (strcmp(name, res->name) == 0)
			return res;
	}
	return NULL;
}

static int sanity_check_abs_cmd(char *cmd_name)
{
	struct stat sb;

	if (stat(cmd_name, &sb)) {
		/* If stat fails, just ignore this sanity check,
		 * we are still iterating over $PATH probably. */
		return 0;
	}

	if (!(sb.st_mode & S_ISUID) || sb.st_mode & S_IXOTH || sb.st_gid == 0) {
		static int did_header = 0;
		if (!did_header)
			err("WARN:\n"
			    "  You are using the 'drbd-peer-outdater' as fence-peer program.\n"
			    "  If you use that mechanism the dopd heartbeat plugin program needs\n"
			    "  to be able to call drbdsetup and drbdmeta with root privileges.\n\n"
			    "  You need to fix this with these commands:\n");
		did_header = 1;
		err("  chgrp haclient %s\n"
		    "  chmod o-x %s\n"
		    "  chmod u+s %s\n\n",
		    cmd_name, cmd_name, cmd_name);
	}
	return 1;
}

static void sanity_check_cmd(char *cmd_name)
{
	char *path, *pp, *c;
	char abs_path[100];

	if (strchr(cmd_name, '/')) {
		sanity_check_abs_cmd(cmd_name);
	} else {
		path = pp = c = strdup(getenv("PATH"));

		while (1) {
			c = strchr(pp, ':');
			if (c)
				*c = 0;
			snprintf(abs_path, 100, "%s/%s", pp, cmd_name);
			if (sanity_check_abs_cmd(abs_path))
				break;
			if (!c)
				break;
			c++;
			if (!*c)
				break;
			pp = c;
		}
		free(path);
	}
}

/* if the config file is not readable by haclient,
 * dopd cannot work.
 * NOTE: we assume that any gid != 0 will be the group dopd will run as,
 * typically haclient. */
static void sanity_check_conf(char *c)
{
	struct stat sb;

	/* if we cannot stat the config file,
	 * we have other things to worry about. */
	if (stat(c, &sb))
		return;

	/* permissions are funny: if it is world readable,
	 * but not group readable, and it belongs to my group,
	 * I am denied access.
	 * For the file to be readable by dopd (hacluster:haclient),
	 * it is not enough to be world readable. */

	/* ok if world readable, and NOT group haclient (see NOTE above) */
	if (sb.st_mode & S_IROTH && sb.st_gid == 0)
		return;

	/* ok if group readable, and group haclient (see NOTE above) */
	if (sb.st_mode & S_IRGRP && sb.st_gid != 0)
		return;

	err("WARN:\n"
	    "  You are using the 'drbd-peer-outdater' as fence-peer program.\n"
	    "  If you use that mechanism the dopd heartbeat plugin program needs\n"
	    "  to be able to read the drbd.config file.\n\n"
	    "  You need to fix this with these commands:\n"
	    "  chgrp haclient %s\n"
	    "  chmod g+r %s\n\n", c, c);
}

static void sanity_check_perm()
{
	static int checked = 0;
	if (checked)
		return;

	sanity_check_cmd(drbdsetup);
	sanity_check_cmd(drbdmeta);
	sanity_check_conf(config_file);
	checked = 1;
}

static bool host_name_known(struct d_resource *res, char *name)
{
	struct d_host_info *host;

	for_each_host(host, &res->all_hosts)
		if (hostname_in_list(name, &host->on_hosts))
			return 1;

	return 0;
}

/* Check that either all host sections have a proxy subsection, or none */
static void ensure_proxy_sections(struct d_resource *res)
{
	struct d_host_info *host;
	struct connection *conn;
	enum { INIT, HAVE, MISSING } proxy_sect = INIT, prev_proxy_sect;

	for_each_host(host, &res->all_hosts) {
		prev_proxy_sect = proxy_sect;
		proxy_sect = host->proxy_compat_only ? HAVE : MISSING;
		if (prev_proxy_sect == INIT)
			continue;
		if (prev_proxy_sect != proxy_sect) {
			err("%s:%d: in resource %s:\n\t"
			    "Either all 'on' sections must contain a proxy subsection, or none.\n",
			    res->config_file, res->start_line, res->name);
			config_valid = 0;
		}
	}

	for_each_connection(conn, &res->connections) {
		struct path *path;

		for_each_path(path, &conn->paths) {
			struct hname_address *ha;
			proxy_sect = INIT;

			STAILQ_FOREACH(ha, &path->hname_address_pairs, link) {
				prev_proxy_sect = proxy_sect;
				proxy_sect = ha->proxy ? HAVE : MISSING;
				if (prev_proxy_sect == INIT)
					continue;
				if (prev_proxy_sect != proxy_sect) {
					err("%s:%d: in connection in resource %s:\n"
					    "Either all 'host' statements must have a proxy subsection, or none.\n",
					    res->config_file, conn->config_line,
					    res->name);
					config_valid = 0;
				}
			}
		}
	}
}

static void validate_resource(struct d_resource *res, enum pp_flags flags)
{
	struct d_option *opt;

	/* there may be more than one "resync-after" statement,
	 * see commit 89cd0585 */
	STAILQ_FOREACH(opt, &res->disk_options, link) {
		struct d_resource *rs_after_res;
	next:
		if (strcmp(opt->name, "resync-after"))
			continue;
		rs_after_res = res_by_name(opt->value);
		if (rs_after_res == NULL ||
		    (rs_after_res->ignore && !(flags & MATCH_ON_PROXY))) {
			err("%s:%d: in resource %s:\n\tresource '%s' mentioned in "
			    "'resync-after' option is not known%s.\n",
			    res->config_file, res->start_line, res->name,
			    opt->value, rs_after_res ? " on this host" : "");
			/* Non-fatal if run from some script.
			 * When deleting resources, it is an easily made
			 * oversight to leave references to the deleted
			 * resources in resync-after statements.  Don't fail on
			 * every pacemaker-induced action, as it would
			 * ultimately lead to all nodes committing suicide. */
			if (no_tty) {
				struct d_option *next = STAILQ_NEXT(opt, link);
				STAILQ_REMOVE(&res->disk_options, opt, d_option, link);
				free_opt(opt);
				opt = next;
				if (opt)
					goto next;
				else
					break;
			} else
				config_valid = 0;
		}
	}
	if (STAILQ_EMPTY(&res->all_hosts)) {
		err("%s:%d: in resource %s:\n\ta host sections ('on %s { ... }') is missing.\n",
		    res->config_file, res->start_line, res->name, hostname);
		config_valid = 0;
	}
	if (res->ignore)
		return;
	if (!res->me) {
		err("%s:%d: in resource %s:\n\tmissing section 'on %s { ... }'.\n",
		    res->config_file, res->start_line, res->name, hostname);
		config_valid = 0;
	}
	// need to verify that in the discard-node-nodename options only known
	// nodenames are mentioned.
	if ((opt = find_opt(&res->net_options, "after-sb-0pri"))) {
		if (!strncmp(opt->value, "discard-node-", 13)) {
			if (!host_name_known(res, opt->value + 13)) {
				err("%s:%d: in resource %s:\n\t"
				    "the nodename in the '%s' option is "
				    "not known.\n",
				    res->config_file, res->start_line,
				    res->name, opt->value);
				config_valid = 0;
			}
		}
	}

	if ((opt = find_opt(&res->handlers, "fence-peer"))) {
		if (strstr(opt->value, "drbd-peer-outdater"))
			sanity_check_perm();
	}

	ensure_proxy_sections(res); /* All or none. */
}

static int ctx_set_implicit_volume(struct cfg_ctx *ctx)
{
	struct d_volume *vol, *v = NULL;
	int volumes = 0;

	if (ctx->vol || !ctx->res)
		return 0;

	if (!ctx->res->me) {
		return 0;
	}

	for_each_volume(vol, &ctx->res->me->volumes) {
		volumes++;
		v = vol;
	}

	if (volumes == 1)
		ctx->vol = v;

	return volumes;
}

// Need to convert after from resourcename to minor_number.
static void _convert_after_option(struct d_resource *res, struct d_volume *vol)
{
	struct d_option *opt;
	struct cfg_ctx depends_on_ctx = { };
	int volumes;

	if (res == NULL)
		return;

	STAILQ_FOREACH(opt, &vol->disk_options, link) {
	next:
		if (strcmp(opt->name, "resync-after"))
			continue;
		ctx_by_name(&depends_on_ctx, opt->value, CTX_FIRST);
		volumes = ctx_set_implicit_volume(&depends_on_ctx);
		if (volumes > 1) {
			err("%s:%d: in resource %s:\n\t"
			    "resync-after contains '%s', which is ambiguous, since it contains %d volumes\n",
			    res->config_file, res->start_line, res->name,
			    opt->value, volumes);
			config_valid = 0;
			return;
		}

		if (!depends_on_ctx.res || depends_on_ctx.res->ignore) {
			struct d_option *next = STAILQ_NEXT(opt, link);
			STAILQ_REMOVE(&vol->disk_options, opt, d_option, link);
			free_opt(opt);
			opt = next;
			if (opt)
				goto next;
			else
				break;
		} else {
			free(opt->value);
			m_asprintf(&opt->value, "%d", depends_on_ctx.vol->device_minor);
		}
	}
}

// Need to convert after from resourcename/volume to minor_number.
static void convert_after_option(struct d_resource *res)
{
	struct d_volume *vol;
	struct d_host_info *h;

	for_each_host(h, &res->all_hosts)
		for_each_volume(vol, &h->volumes)
			_convert_after_option(res, vol);
}

// need to convert discard-node-nodename to discard-local or discard-remote.
static void convert_discard_opt(struct options *net_options)
{
	struct d_option *opt;

	if ((opt = find_opt(net_options, "after-sb-0pri"))) {
		if (!strncmp(opt->value, "discard-node-", 13)) {
			if (!strcmp(hostname, opt->value + 13)) {
				free(opt->value);
				opt->value = strdup("discard-local");
			} else {
				free(opt->value);
				opt->value = strdup("discard-remote");
			}
		}
	}
}

void global_validate_maybe_expand_die_if_invalid(int expand, enum pp_flags flags)
{
	struct d_resource *res;
	for_each_resource(res, &config) {
		validate_resource(res, flags);
		if (!config_valid)
			exit(E_CONFIG_INVALID);
		if (expand) {
			struct connection *conn;

			convert_after_option(res);
			convert_discard_opt(&res->net_options);

			for_each_connection(conn, &res->connections)
				convert_discard_opt(&conn->net_options);
		}
		if (!config_valid)
			exit(E_CONFIG_INVALID);
	}
}
