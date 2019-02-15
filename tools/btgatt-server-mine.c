#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/l2cap.h"
#include "lib/uuid.h"
#include "lib/configReader.h"

#include "src/shared/mainloop.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/timeout.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"

#define UUID_GAP			0x1800

#define ATT_CID 4

#define PRLOG(...) \
	do { \
		printf(__VA_ARGS__); \
		print_prompt(); \
	} while (0)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define COLOR_OFF	"\x1B[0m"
#define COLOR_RED	"\x1B[0;91m"
#define COLOR_GREEN	"\x1B[0;92m"
#define COLOR_YELLOW	"\x1B[0;93m"
#define COLOR_BLUE	"\x1B[0;94m"
#define COLOR_MAGENTA	"\x1B[0;95m"
#define COLOR_BOLDGRAY	"\x1B[1;30m"
#define COLOR_BOLDWHITE	"\x1B[1;37m"

static const char test_device_name[] = "Very Long Test Device Name For Testing "
				"ATT Protocol Operations On GATT Server";
static bool verbose = false;

static service *services = NULL;
static int servicesSize = 0;

struct server {
	int fd;
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_server *gatt;

	uint8_t *device_name;
	size_t name_len;

	uint16_t gatt_svc_chngd_handle;
	bool svc_chngd_enabled;

	uint16_t mine_handle;
  bool mine_visible;

  bool desc_config_enabled;
};

static void print_prompt(void)
{
	printf(COLOR_BLUE "[GATT server]" COLOR_OFF "# ");
	fflush(stdout);
}

static void att_disconnect_cb(int err, void *user_data)
{
	printf("Device disconnected: %s\n", strerror(err));

	mainloop_quit();
}

static void att_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	PRLOG(COLOR_BOLDGRAY "%s" COLOR_BOLDWHITE "%s\n" COLOR_OFF, prefix,
									str);
}

static void gatt_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	PRLOG(COLOR_GREEN "%s%s\n" COLOR_OFF, prefix, str);
}

static void confirm_write(struct gatt_db_attribute *attr, int err,
							void *user_data);

// GAP SERVICE BEGIN
static void gap_device_name_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;
	size_t len = 0;
	const uint8_t *value = NULL;

	PRLOG("GAP Device Name Read called\n");

	len = server->name_len;

	if (offset > len) {
		error = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	len -= offset;
	value = len ? &server->device_name[offset] : NULL;

done:
	gatt_db_attribute_read_result(attrib, id, error, value, len);
}

static void gap_device_name_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;

	PRLOG("GAP Device Name Write called\n");

	/* If the value is being completely truncated, clean up and return */
	if (!(offset + len)) {
		free(server->device_name);
		server->device_name = NULL;
		server->name_len = 0;
		goto done;
	}

	/* Implement this as a variable length attribute value. */
	if (offset > server->name_len) {
		error = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (offset + len != server->name_len) {
		uint8_t *name;

		name = realloc(server->device_name, offset + len);
		if (!name) {
			error = BT_ATT_ERROR_INSUFFICIENT_RESOURCES;
			goto done;
		}

		server->device_name = name;
		server->name_len = offset + len;
	}

	if (value)
		memcpy(server->device_name + offset, value, len);

done:
	gatt_db_attribute_write_result(attrib, id, error);
}

static void gap_device_name_ext_prop_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	uint8_t value[2];

	PRLOG("Device Name Extended Properties Read called\n");

	value[0] = BT_GATT_CHRC_EXT_PROP_RELIABLE_WRITE;
	value[1] = 0;

	gatt_db_attribute_read_result(attrib, id, 0, value, sizeof(value));
}


static void populate_gap_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *tmp;
	uint16_t appearance;

	/* Add the GAP service */
	bt_uuid16_create(&uuid, UUID_GAP);
	service = gatt_db_add_service(server->db, &uuid, true, 6);

	/*
	 * Device Name characteristic. Make the value dynamically read and
	 * written via callbacks.
	 */
	bt_uuid16_create(&uuid, GATT_CHARAC_DEVICE_NAME);
	gatt_db_service_add_characteristic(service, &uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_EXT_PROP,
					gap_device_name_read_cb,
					gap_device_name_write_cb,
					server);

	bt_uuid16_create(&uuid, GATT_CHARAC_EXT_PROPER_UUID);
	gatt_db_service_add_descriptor(service, &uuid, BT_ATT_PERM_READ,
					gap_device_name_ext_prop_read_cb,
					NULL, server);

	/*
	 * Appearance characteristic. Reads and writes should obtain the value
	 * from the database.
	 */
	bt_uuid16_create(&uuid, GATT_CHARAC_APPEARANCE);
	tmp = gatt_db_service_add_characteristic(service, &uuid,
							BT_ATT_PERM_READ,
							BT_GATT_CHRC_PROP_READ,
							NULL, NULL, server);

	/*
	 * Write the appearance value to the database, since we're not using a
	 * callback.
	 */
	put_le16(128, &appearance);
	gatt_db_attribute_write(tmp, 0, (void *) &appearance,
							sizeof(appearance),
							BT_ATT_OP_WRITE_REQ,
							NULL, confirm_write,
							NULL);

	gatt_db_service_set_active(service, true);
}
// GAP SERVICE END

// MINE SERVICE BEGIN
static void interact_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	uint8_t ecode = 0;

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

  PRLOG("Received %d bytes\n", len);
  PRLOG("Data received %s\n", value);

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void counter_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	//struct server *server = user_data;
	uint8_t value[2];

	value[0] = 0x01;
	value[1] = 0x00;

	gatt_db_attribute_read_result(attrib, id, 0, value, 2);
}

static void desc_user_desc_interactor_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	//struct server *server = user_data;
	uint8_t error = 0;
	size_t len = 0;
	const uint8_t *value = NULL;
	const unsigned char *text = (unsigned char *) "Write any value here to move the cat's paw and increment the awesomeness counter";

	PRLOG("Descriptor user config Read called, with offset: %d\n", offset);

  len = strlen((char *)text);
  if (offset > len) {
    error = BT_ATT_ERROR_INVALID_OFFSET;
    goto done;
  }

	len -= offset;
	value = len ? &text[offset] : NULL;

done:
	gatt_db_attribute_read_result(attrib, id, error, value, len);
}

static void  desc_user_desc_counter_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	//struct server *server = user_data;
	uint8_t error = 0;
	size_t len = 0;
	const uint8_t *value = NULL;
	const unsigned char *text = (unsigned char *) "Indicates the number of time you have been awesome so far";

	PRLOG("Descriptor user config Read called, with offset: %d\n", offset);

  len = strlen((char *)text);
  if (offset > len) {
    error = BT_ATT_ERROR_INVALID_OFFSET;
    goto done;
  }

	len -= offset;
	value = len ? &text[offset] : NULL;

done:
	gatt_db_attribute_read_result(attrib, id, error, value, len);
}

static void desc_conf_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2];

	PRLOG("Descriptor config Read called\n");

	value[0] = server->desc_config_enabled ? 0x02 : 0x00;
	value[1] = 0x00;

	gatt_db_attribute_read_result(attrib, id, 0, value, sizeof(value));
}

static void desc_conf_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	if (!value || len != 2) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 0x00)
		server->desc_config_enabled = false;
	else if (value[0] == 0x01) {
		if (server->desc_config_enabled) {
			PRLOG("Desc config already Enabled\n");
			goto done;
		}

		server->desc_config_enabled= true;
	} else
		ecode = 0x80;

	PRLOG("Desc config Enabled: %s\n",
				server->desc_config_enabled? "true" : "false");

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

//static const uint128_t service_mine_uuid_btorder = {
//  .data = {0x79, 0x50, 0x90, 0xc7, 0x42, 0x0d, 0x40, 0x48,
//    0xa2, 0x4e, 0x18, 0xe6, 0x01, 0x80, 0xe2, 0x3c } };

// Fitbit test
static const uint128_t service_mine_uuid_btorder = {
  .data = {0xad, 0xab, 0xfb, 0x00, 0x6e, 0x7d, 0x46, 0x01,
    0xbd, 0xa2, 0xbf, 0xfa, 0xa6, 0x89, 0x56, 0xba} };

//char_write_uuid_btorder 
static const uint128_t char_interact_uuid_btorder = {
  .data = {0xad, 0xab, 0xfb, 0x02, 0x6e, 0x7d, 0x46, 0x01,
    0xbd, 0xa2, 0xbf, 0xfa, 0xa6, 0x89, 0x56, 0xba} };

//char_read_uuid_btorder 
static const uint128_t char_counter_uuid_btorder = {
  .data = {0xad, 0xab, 0xfb, 0x04, 0x6e, 0x7d, 0x46, 0x01,
    0xbd, 0xa2, 0xbf, 0xfa, 0xa6, 0x89, 0x56, 0xba} };

//static const uint128_t char_interact_uuid_btorder = {
//  .data = {0x0b, 0x89, 0xd2, 0xd4, 0x0e, 0xa6, 0x41, 0x41,
//    0x86, 0xbb, 0x0c, 0x5f, 0xb9, 0x1a, 0xb1, 0x4a } };
//
//static const uint128_t char_counter_uuid_btorder = {
//  .data = {0x31, 0x51, 0x7c, 0x58, 0x66, 0xbf, 0x47, 0x0c,
//    0xb6, 0x62, 0xe3, 0x52, 0xa6, 0xc8, 0x0c, 0xba } };

static const uint128_t desc_config_uuid_btorder = {
  .data = {0x00, 0x00, 0x29, 0x02, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };

static const uint128_t desc_user_desc_uuid_btorder = {
  .data = {0x00, 0x00, 0x29, 0x01, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };

static void populate_mine_service(struct server *server)
{
  bt_uuid_t uuid;
  struct gatt_db_attribute *service, *charac;

  if(services == NULL) {
    uint128_t service_mine_uuid, char_interact_uuid, char_counter_uuid, desc_config_uuid, desc_user_desc_uuid;

    // Convert uuid btorder to uuid
    btoh128(&service_mine_uuid_btorder, &service_mine_uuid);
    btoh128(&char_interact_uuid_btorder, &char_interact_uuid);
    btoh128(&char_counter_uuid_btorder, &char_counter_uuid);
    btoh128(&desc_config_uuid_btorder, &desc_config_uuid);
    btoh128(&desc_user_desc_uuid_btorder, &desc_user_desc_uuid);

    bt_uuid128_create(&uuid, service_mine_uuid);
    //bt_uuid16_create(&uuid, 0x180d);
    service = gatt_db_add_service(server->db, &uuid, true, 8);

    // Writable character
    bt_uuid128_create(&uuid, char_interact_uuid);
    //bt_uuid16_create(&uuid, 0x2a39);
    charac = gatt_db_service_add_characteristic(service, &uuid,
        BT_ATT_PERM_WRITE,
        BT_GATT_CHRC_PROP_WRITE,
        //BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
        NULL, interact_write_cb,
        server);

    bt_uuid128_create(&uuid, desc_user_desc_uuid);
    gatt_db_service_add_descriptor(charac, &uuid,
        BT_ATT_PERM_READ,
        desc_user_desc_interactor_read_cb,
        NULL, server);

    // Stuff which is needed for the android app
    bt_uuid128_create(&uuid, char_counter_uuid);
    charac = gatt_db_service_add_characteristic(service, &uuid,
        BT_ATT_PERM_READ,
        BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_NOTIFY,
        //BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
        counter_read_cb, NULL,
        server);

    bt_uuid128_create(&uuid, desc_user_desc_uuid);
    gatt_db_service_add_descriptor(charac, &uuid,
        BT_ATT_PERM_READ,
        desc_user_desc_counter_read_cb,
        NULL, server);

    bt_uuid128_create(&uuid, desc_config_uuid);
    gatt_db_service_add_descriptor(charac, &uuid,
        BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
        desc_conf_read_cb,
        desc_conf_write_cb, server);

    if (server->mine_visible)
      gatt_db_service_set_active(service, true);

  } else {

    /* Create service */
    for (int i = 0; i < servicesSize; i++) {

      if (bt_string_to_uuid(&uuid, services[i].uuid) < 0) {
        fprintf(stderr, "Error converting string uuid (\"%s\") to uuid for service %d\n", services[i].uuid, i);
        continue;
      }

      service = gatt_db_add_service(server->db, &uuid, true, 8);

      /* Create characteristics */
      for (int j = 0; j < services[i].characteristicsSize; j++) {
        characteristic c = services[i].characteristics[j];

        if (bt_string_to_uuid(&uuid, c.uuid) < 0) {
          fprintf(stderr, "Error converting string uuid (\"%s\") to uuid for characteristic %d in service %d\n", c.uuid, j, i);
          continue;
        }

        // TODO: somehow decide what do do with callback functions
        charac = gatt_db_service_add_characteristic(service, &uuid,
            BT_ATT_PERM_WRITE | BT_ATT_PERM_READ,
            BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_NOTIFY | BT_GATT_CHRC_PROP_WRITE,
            NULL, NULL,
            server);

        for (int k = 0; k < c.descriptionsSize; k++) {
          description d = c.descriptions[k];

          if (bt_string_to_uuid(&uuid, d.uuid) < 0) {
            fprintf(stderr, "Error converting string uuid (\"%s\") to uuid for description %d in characteristic %d in service %d\n", d.uuid, k, j, i);
            continue;
          }

          //TODO: do something to write the correct text
          gatt_db_service_add_descriptor(charac, &uuid,
              BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
              NULL,
              NULL, server);
        }
      }

      if (server->mine_visible)
        gatt_db_service_set_active(service, true);
    }
  }
}

// MINE SERVICE END


static void usage(void)
{
	printf("btgatt-server\n");
	printf("Usage:\n\tbtgatt-server [options]\n");

	printf("Options:\n"
		"\t-c, --config <path>\t\tPath to config file with service definition\n"
		"\t-i, --index <id>\t\tSpecify adapter index, e.g. hci0\n"
		"\t-m, --mtu <mtu>\t\t\tThe ATT MTU to use\n"
		"\t-s, --security-level <sec>\tSet security level (low|"
								"medium|high)\n"
		"\t-t, --type [random|public] \t The source address type\n"
		"\t-v, --verbose\t\t\tEnable extra logging\n"
		"\t-r, --mine\t\tEnable my service\n"
		"\t-h, --help\t\t\tDisplay help\n");
}

static struct option main_options[] = {
	{ "config",		1, 0, 'c' },
	{ "index",		1, 0, 'i' },
	{ "mtu",		1, 0, 'm' },
	{ "security-level",	1, 0, 's' },
	{ "type",		1, 0, 't' },
	{ "verbose",		0, 0, 'v' },
	{ "mine",		0, 0, 'm' },
	{ "help",		0, 0, 'h' },
	{ }
};

static bool parse_args(char *str, int expected_argc,  char **argv, int *argc)
{
	char **ap;

	for (ap = argv; (*ap = strsep(&str, " \t")) != NULL;) {
		if (**ap == '\0')
			continue;

		(*argc)++;
		ap++;

		if (*argc > expected_argc)
			return false;
	}

	return true;
}

// GATT SERVER COMMAND HANDLERS BEGIN
static void conf_cb(void *user_data)
{
	PRLOG("Received confirmation\n");
}

static void notify_usage(void)
{
	printf("Usage: notify [options] <value_handle> <value>\n"
					"Options:\n"
					"\t -i, --indicate\tSend indication\n"
					"e.g.:\n"
					"\tnotify 0x0001 00 01 00\n");
}

static struct option notify_options[] = {
	{ "indicate",	0, 0, 'i' },
	{ }
};

static void cmd_notify(struct server *server, char *cmd_str)
{
	int opt, i;
	char *argvbuf[516];
	char **argv = argvbuf;
	int argc = 1;
	uint16_t handle;
	char *endptr = NULL;
	int length;
	uint8_t *value = NULL;
	bool indicate = false;

	if (!parse_args(cmd_str, 514, argv + 1, &argc)) {
		printf("Too many arguments\n");
		notify_usage();
		return;
	}

	optind = 0;
	argv[0] = "notify";
	while ((opt = getopt_long(argc, argv, "+i", notify_options,
								NULL)) != -1) {
		switch (opt) {
		case 'i':
			indicate = true;
			break;
		default:
			notify_usage();
			return;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		notify_usage();
		return;
	}

	handle = strtol(argv[0], &endptr, 16);
	if (!endptr || *endptr != '\0' || !handle) {
		printf("Invalid handle: %s\n", argv[0]);
		return;
	}

	length = argc - 1;

	if (length > 0) {
		if (length > UINT16_MAX) {
			printf("Value too long\n");
			return;
		}

		value = malloc(length);
		if (!value) {
			printf("Failed to construct value\n");
			return;
		}

		for (i = 1; i < argc; i++) {
			if (strlen(argv[i]) != 2) {
				printf("Invalid value byte: %s\n",
								argv[i]);
				goto done;
			}

			value[i-1] = strtol(argv[i], &endptr, 16);
			if (endptr == argv[i] || *endptr != '\0'
							|| errno == ERANGE) {
				printf("Invalid value byte: %s\n",
								argv[i]);
				goto done;
			}
		}
	}

	if (indicate) {
		if (!bt_gatt_server_send_indication(server->gatt, handle,
							value, length,
							conf_cb, NULL, NULL))
			printf("Failed to initiate indication\n");
	} else if (!bt_gatt_server_send_notification(server->gatt, handle,
								value, length))
		printf("Failed to initiate notification\n");

done:
	free(value);
}

static void mine_usage(void)
{
	printf("Usage: mine on|off\n");
}

static void cmd_mine(struct server *server, char *cmd_str)
{
	bool enable;
	uint8_t pdu[4];
	struct gatt_db_attribute *attr;

	if (!cmd_str) {
		mine_usage();
		return;
	}

	if (strcmp(cmd_str, "on") == 0)
		enable = true;
	else if (strcmp(cmd_str, "off") == 0)
		enable = false;
	else {
		mine_usage();
		return;
	}

	if (enable == server->mine_visible) {
		printf("Heart Rate Service already %s\n",
						enable ? "visible" : "hidden");
		return;
	}

	server->mine_visible = enable;
	attr = gatt_db_get_attribute(server->db, server->mine_handle);
	gatt_db_service_set_active(attr, server->mine_visible);

	if (!server->svc_chngd_enabled)
		return;

	put_le16(server->mine_handle, pdu);
	put_le16(server->mine_handle + 7, pdu + 2);

	bt_gatt_server_send_indication(server->gatt,
						server->gatt_svc_chngd_handle,
						pdu, 4, conf_cb, NULL, NULL);
}

// PRINT SERVICES BEGIN
static void print_uuid(const bt_uuid_t *uuid)
{
	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_t uuid128;

	bt_uuid_to_uuid128(uuid, &uuid128);
	bt_uuid_to_string(&uuid128, uuid_str, sizeof(uuid_str));

	printf("%s\n", uuid_str);
}

static void print_incl(struct gatt_db_attribute *attr, void *user_data)
{
	struct server *server = user_data;
	uint16_t handle, start, end;
	struct gatt_db_attribute *service;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_incl_data(attr, &handle, &start, &end))
		return;

	service = gatt_db_get_attribute(server->db, start);
	if (!service)
		return;

	gatt_db_attribute_get_service_uuid(service, &uuid);

	printf("\t  " COLOR_GREEN "include" COLOR_OFF " - handle: "
					"0x%04x, - start: 0x%04x, end: 0x%04x,"
					"uuid: ", handle, start, end);
	print_uuid(&uuid);
}

static void print_desc(struct gatt_db_attribute *attr, void *user_data)
{
	printf("\t\t  " COLOR_MAGENTA "descr" COLOR_OFF
					" - handle: 0x%04x, uuid: ",
					gatt_db_attribute_get_handle(attr));
	print_uuid(gatt_db_attribute_get_type(attr));
}

static void print_chrc(struct gatt_db_attribute *attr, void *user_data)
{
	uint16_t handle, value_handle;
	uint8_t properties;
	uint16_t ext_prop;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_char_data(attr, &handle,
								&value_handle,
								&properties,
								&ext_prop,
								&uuid))
		return;

	printf("\t  " COLOR_YELLOW "charac" COLOR_OFF
				" - start: 0x%04x, value: 0x%04x, "
				"props: 0x%02x, ext_prop: 0x%04x, uuid: ",
				handle, value_handle, properties, ext_prop);
	print_uuid(&uuid);

	gatt_db_service_foreach_desc(attr, print_desc, NULL);
}

static void print_service(struct gatt_db_attribute *attr, void *user_data)
{
	struct server *server = user_data;
	uint16_t start, end;
	bool primary;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_service_data(attr, &start, &end, &primary,
									&uuid))
		return;

	printf(COLOR_RED "service" COLOR_OFF " - start: 0x%04x, "
				"end: 0x%04x, type: %s, uuid: ",
				start, end, primary ? "primary" : "secondary");
	print_uuid(&uuid);

	gatt_db_service_foreach_incl(attr, print_incl, server);
	gatt_db_service_foreach_char(attr, print_chrc, NULL);

	printf("\n");
}

static void cmd_services(struct server *server, char *cmd_str)
{
	gatt_db_foreach_service(server->db, NULL, print_service, server);
}
// PRINT SERVICES END

static void cmd_help(struct server *server, char *cmd_str);

typedef void (*command_func_t)(struct server *server, char *cmd_str);

static struct {
	char *cmd;
	command_func_t func;
	char *doc;
} command[] = {
	{ "help", cmd_help, "\tDisplay help message" },
	{ "notify", cmd_notify, "\tSend handle-value notification" },
	{ "mine", cmd_mine, "\tHide/Unhide my Service" },
	{ "services", cmd_services, "\tEnumerate all services" },
	{ }
};

static void cmd_help(struct server *server, char *cmd_str)
{
	int i;

	printf("Commands:\n");
	for (i = 0; command[i].cmd; i++)
		printf("\t%-15s\t%s\n", command[i].cmd, command[i].doc);
}
// GATT SERVER COMMAND HANDLERS END

// CONNECTION HANDLER
static int l2cap_le_att_listen_and_accept(bdaddr_t *src, int sec,
							uint8_t src_type)
{
	int sk, nsk;
	struct sockaddr_l2 srcaddr, addr;
	socklen_t optlen;
	struct bt_security btsec;
	char ba[18];

	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sk < 0) {
		perror("Failed to create L2CAP socket");
		return -1;
	}

	/* Set up source address */
	memset(&srcaddr, 0, sizeof(srcaddr));
	srcaddr.l2_family = AF_BLUETOOTH;
	srcaddr.l2_cid = htobs(ATT_CID);
	srcaddr.l2_bdaddr_type = src_type;
	bacpy(&srcaddr.l2_bdaddr, src);

	if (bind(sk, (struct sockaddr *) &srcaddr, sizeof(srcaddr)) < 0) {
		perror("Failed to bind L2CAP socket");
		goto fail;
	}

	/* Set the security level */
	memset(&btsec, 0, sizeof(btsec));
	btsec.level = sec;
	if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &btsec,
							sizeof(btsec)) != 0) {
		fprintf(stderr, "Failed to set L2CAP security level\n");
		goto fail;
	}

	if (listen(sk, 10) < 0) {
		perror("Listening on socket failed");
		goto fail;
	}

	printf("Started listening on ATT channel. Waiting for connections\n");

	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);
	nsk = accept(sk, (struct sockaddr *) &addr, &optlen);
	if (nsk < 0) {
		perror("Accept failed");
		goto fail;
	}

	ba2str(&addr.l2_bdaddr, ba);
	printf("Connect from %s\n", ba);
	close(sk);

	return nsk;

fail:
	close(sk);
	return -1;
}

// SERVER HANDLING BEGIN
static void confirm_write(struct gatt_db_attribute *attr, int err,
							void *user_data)
{
	if (!err)
		return;

	fprintf(stderr, "Error caching attribute %p - err: %d\n", attr, err);
	exit(1);
}

static void populate_db(struct server *server)
{
	populate_gap_service(server);
	//populate_gatt_service(server);
	populate_mine_service(server);
}

static struct server *server_create(int fd, uint16_t mtu, bool mine_visible)
{
	struct server *server;
	size_t name_len = strlen(test_device_name);

	server = new0(struct server, 1);
	if (!server) {
		fprintf(stderr, "Failed to allocate memory for server\n");
		return NULL;
	}

	server->att = bt_att_new(fd, false);
	if (!server->att) {
		fprintf(stderr, "Failed to initialze ATT transport layer\n");
		goto fail;
	}

	if (!bt_att_set_close_on_unref(server->att, true)) {
		fprintf(stderr, "Failed to set up ATT transport layer\n");
		goto fail;
	}

	if (!bt_att_register_disconnect(server->att, att_disconnect_cb, NULL,
									NULL)) {
		fprintf(stderr, "Failed to set ATT disconnect handler\n");
		goto fail;
	}

	server->name_len = name_len + 1;
	server->device_name = malloc(name_len + 1);
	if (!server->device_name) {
		fprintf(stderr, "Failed to allocate memory for device name\n");
		goto fail;
	}

	memcpy(server->device_name, test_device_name, name_len);
	server->device_name[name_len] = '\0';

	server->fd = fd;
	server->db = gatt_db_new();
	if (!server->db) {
		fprintf(stderr, "Failed to create GATT database\n");
		goto fail;
	}

	server->gatt = bt_gatt_server_new(server->db, server->att, mtu);
	if (!server->gatt) {
		fprintf(stderr, "Failed to create GATT server\n");
		goto fail;
	}

	server->mine_visible = mine_visible;

	if (verbose) {
		bt_att_set_debug(server->att, att_debug_cb, "att: ", NULL);
		bt_gatt_server_set_debug(server->gatt, gatt_debug_cb,
							"server: ", NULL);
	}

	/* Random seed for generating fake Heart Rate measurements */
	srand(time(NULL));

	/* bt_gatt_server already holds a reference */
	populate_db(server);

	return server;

fail:
	gatt_db_unref(server->db);
	free(server->device_name);
	bt_att_unref(server->att);
	free(server);

	return NULL;
}

static void server_destroy(struct server *server)
{
	bt_gatt_server_unref(server->gatt);
	gatt_db_unref(server->db);
}
// SERVER HANDLING END

// COMMAND LINE HANDLERS BEGIN
static void prompt_read_cb(int fd, uint32_t events, void *user_data)
{
	ssize_t read;
	size_t len = 0;
	char *line = NULL;
	char *cmd = NULL, *args;
	struct server *server = user_data;
	int i;

	if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
		mainloop_quit();
		return;
	}

	read = getline(&line, &len, stdin);
	if (read < 0)
		return;

	if (read <= 1) {
		cmd_help(server, NULL);
		print_prompt();
		return;
	}

	line[read-1] = '\0';
	args = line;

	while ((cmd = strsep(&args, " \t")))
		if (*cmd != '\0')
			break;

	if (!cmd)
		goto failed;

	for (i = 0; command[i].cmd; i++) {
		if (strcmp(command[i].cmd, cmd) == 0)
			break;
	}

	if (command[i].cmd)
		command[i].func(server, args);
	else
		fprintf(stderr, "Unknown command: %s\n", line);

failed:
	print_prompt();

	free(line);
}

static void signal_cb(int signum, void *user_data)
{
	switch (signum) {
	case SIGINT:
	case SIGTERM:
		mainloop_quit();
		break;
	default:
		break;
	}
}
// COMMAND LINE HANDLERS END
int main(int argc, char *argv[])
{
	int opt;
	bdaddr_t src_addr;
	int dev_id = -1;
	int fd;
	int sec = BT_SECURITY_LOW;
	uint8_t src_type = BDADDR_LE_PUBLIC;
	uint16_t mtu = 0;
	sigset_t mask;
	bool mine_visible = false;
	struct server *server;

	while ((opt = getopt_long(argc, argv, "+hvrs:t:m:i:c:",
						main_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage();
			return EXIT_SUCCESS;
		case 'v':
			verbose = true;
			break;
		case 'r':
			mine_visible = true;
			break;
		case 's':
			if (strcmp(optarg, "low") == 0)
				sec = BT_SECURITY_LOW;
			else if (strcmp(optarg, "medium") == 0)
				sec = BT_SECURITY_MEDIUM;
			else if (strcmp(optarg, "high") == 0)
				sec = BT_SECURITY_HIGH;
			else {
				fprintf(stderr, "Invalid security level\n");
				return EXIT_FAILURE;
			}
			break;
		case 't':
			if (strcmp(optarg, "random") == 0)
				src_type = BDADDR_LE_RANDOM;
			else if (strcmp(optarg, "public") == 0)
				src_type = BDADDR_LE_PUBLIC;
			else {
				fprintf(stderr,
					"Allowed types: random, public\n");
				return EXIT_FAILURE;
			}
			break;
		case 'm': {
			int arg;

			arg = atoi(optarg);
			if (arg <= 0) {
				fprintf(stderr, "Invalid MTU: %d\n", arg);
				return EXIT_FAILURE;
			}

			if (arg > UINT16_MAX) {
				fprintf(stderr, "MTU too large: %d\n", arg);
				return EXIT_FAILURE;
			}

			mtu = (uint16_t) arg;
			break;
		}
    case 'c':
      services = readConfig(optarg, &servicesSize);
      if (services == NULL) {
        fprintf(stderr, "The config file couldn't be read");
        return EXIT_FAILURE;
      }
      break;
		case 'i':
			dev_id = hci_devid(optarg);
			if (dev_id < 0) {
				perror("Invalid adapter");
				return EXIT_FAILURE;
			}

			break;
		default:
			fprintf(stderr, "Invalid option: %c\n", opt);
			return EXIT_FAILURE;
		}
	}

	argc -= optind;
	argv -= optind;
	optind = 0;

	if (argc) {
		usage();
		return EXIT_SUCCESS;
	}

	if (dev_id == -1)
		bacpy(&src_addr, BDADDR_ANY);
	else if (hci_devba(dev_id, &src_addr) < 0) {
		perror("Adapter not available");
		return EXIT_FAILURE;
	}

	fd = l2cap_le_att_listen_and_accept(&src_addr, sec, src_type);
	if (fd < 0) {
		fprintf(stderr, "Failed to accept L2CAP ATT connection\n");
		return EXIT_FAILURE;
	}

	mainloop_init();

	server = server_create(fd, mtu, mine_visible);
	if (!server) {
		close(fd);
		return EXIT_FAILURE;
	}

	if (mainloop_add_fd(fileno(stdin),
				EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR,
				prompt_read_cb, server, NULL) < 0) {
		fprintf(stderr, "Failed to initialize console\n");
		server_destroy(server);

		return EXIT_FAILURE;
	}

	printf("Running GATT server\n");

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	mainloop_set_signal(&mask, signal_cb, NULL, NULL);

	print_prompt();

	mainloop_run();

	printf("\n\nShutting down...\n");

	server_destroy(server);

	return EXIT_SUCCESS;
}
