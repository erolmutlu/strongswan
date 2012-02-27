/*
 * Copyright (C) 2012 Martin Willi
 * Copyright (C) 2012 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "radattr_listener.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

#include <daemon.h>

#include <radius_message.h>

/**
 * Maximum size of an attribute to add
 */
#define MAX_ATTR_SIZE 1024

typedef struct private_radattr_listener_t private_radattr_listener_t;

/**
 * Private data of an radattr_listener_t object.
 */
struct private_radattr_listener_t {

	/**
	 * Public radattr_listener_t interface.
	 */
	radattr_listener_t public;

	/**
	 * Directory to look for attribute files
	 */
	char *dir;

	/**
	 * IKE_AUTH message ID to attribute
	 */
	int mid;
};

/**
 * Print RADIUS attributes found in IKE message notifies
 */
static void print_radius_attributes(private_radattr_listener_t *this,
									message_t *message)
{
	radius_attribute_type_t type;
	enumerator_t *enumerator;
	notify_payload_t *notify;
	payload_t *payload;
	chunk_t data;

	enumerator = message->create_payload_enumerator(message);
	while (enumerator->enumerate(enumerator, &payload))
	{
		if (payload->get_type(payload) == NOTIFY)
		{
			notify = (notify_payload_t*)payload;
			if (notify->get_notify_type(notify) == RADIUS_ATTRIBUTE)
			{
				data = notify->get_notification_data(notify);
				if (data.len >= 2)
				{
					type = data.ptr[0];
					data = chunk_skip(data, 2);
					if (chunk_printable(data, NULL, 0))
					{
						DBG1(DBG_IKE, "received RADIUS %N: %.*s",
							 radius_attribute_type_names, type,
							 (int)data.len, data.ptr);
					}
					else
					{
						DBG1(DBG_IKE, "received RADIUS %N: %#B",
							 radius_attribute_type_names, type, &data);

					}
				}
			}
		}
	}
	enumerator->destroy(enumerator);
}

/**
 * Add a RADIUS attribute from a client-ID specific file to an IKE message
 */
static void add_radius_attribute(private_radattr_listener_t *this,
								 identification_t *id, message_t *message)
{
	if (this->dir && message->get_message_id(message) == this->mid)
	{
		char path[PATH_MAX];
		chunk_t data;
		struct stat sb;
		void *addr;
		int fd;

		snprintf(path, sizeof(path), "%s/%Y", this->dir, id);
		fd = open(path, O_RDONLY);
		if (fd != -1)
		{
			if (fstat(fd, &sb) != -1)
			{
				if (sb.st_size <= MAX_ATTR_SIZE)
				{
					addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
					if (addr != MAP_FAILED)
					{
						data = chunk_create(addr, sb.st_size);
						if (data.len >= 2)
						{
							DBG1(DBG_CFG, "adding RADIUS %N attribute",
								 radius_attribute_type_names, data.ptr[0]);
							message->add_notify(message, FALSE,
												RADIUS_ATTRIBUTE, data);
						}
						munmap(addr, sb.st_size);
					}
					else
					{
						DBG1(DBG_CFG, "mapping RADIUS attribute '%s' failed: %s",
							 path, strerror(errno));
					}
				}
				else
				{
					DBG1(DBG_CFG, "RADIUS attribute '%s' exceeds size limit",
						 path);
				}
			}
			else
			{
				DBG1(DBG_CFG, "fstat RADIUS attribute '%s' failed: %s",
					 path, strerror(errno));
			}
			close(fd);
		}
		else
		{
			DBG1(DBG_CFG, "reading RADIUS attribute '%s' failed: %s",
				 path, strerror(errno));
		}
	}
}

METHOD(listener_t, message, bool,
	private_radattr_listener_t *this,
	ike_sa_t *ike_sa, message_t *message, bool incoming)
{
	if (ike_sa->supports_extension(ike_sa, EXT_STRONGSWAN) &&
		message->get_exchange_type(message) == IKE_AUTH)
	{
		if (incoming)
		{
			print_radius_attributes(this, message);
		}
		else
		{
			add_radius_attribute(this, ike_sa->get_my_id(ike_sa), message);
		}
	}
	return TRUE;
}


METHOD(radattr_listener_t, destroy, void,
	private_radattr_listener_t *this)
{
	free(this);
}

/**
 * See header
 */
radattr_listener_t *radattr_listener_create()
{
	private_radattr_listener_t *this;

	INIT(this,
		.public = {
			.listener = {
				.message = _message,
			},
			.destroy = _destroy,
		},
		.dir = lib->settings->get_str(lib->settings,
									  "charon.plugins.radattr.dir", NULL),
		.mid = lib->settings->get_int(lib->settings,
									  "charon.plugins.radattr.message_id", 2),
	);

	return &this->public;
}
