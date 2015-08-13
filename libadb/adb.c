/*
 * Copyright (c) 2015, Intel Corporation
 * All rights reserved.
 *
 * Authors: Jeremy Compostella <jeremy.compostella@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <lib.h>
#include <vars.h>
#include <usb.h>

#include "adb.h"
#include "adb_socket.h"
#include "service.h"

/* USB configuration */
#define ADB_IF_SUBCLASS		0x42
#define ADB_IF_PROTOCOL		0x01
#define STR_CONFIGURATION	L"ADB"
#define STR_INTERFACE		L"ADB Interface"

/* Protocol definitions */
#define ADB_VERSION	0x01000000
#define SYSTEM_TYPE	"bootloader"

/* Internal data */
typedef enum adb_state {
	ADB_READ_MSG,
	ADB_READ_MSG_PAYLOAD,
	ADB_PROCESS_MSG
} adb_state_t;

static service_t *SERVICES[] = { &reboot_service, &sync_service };
static adb_state_t adb_state;
static adb_pkt_t adb_pkt_in;

static UINT32 adb_pkt_sum(adb_pkt_t *pkt)
{
	UINTN count, sum;
	unsigned char *cur = pkt->data;

	for (sum = 0, count = pkt->msg.data_length; count; count--)
		sum += *cur++;

	return sum;
}

EFI_STATUS adb_send_pkt(adb_pkt_t *pkt, UINT32 command, UINT32 arg0, UINT32 arg1)
{
	EFI_STATUS ret;

	pkt->msg.command = command;
	pkt->msg.arg0 = arg0;
	pkt->msg.arg1 = arg1;

	pkt->msg.magic = pkt->msg.command ^ 0xFFFFFFFF;
	pkt->msg.data_check = adb_pkt_sum(pkt);

	ret = usb_write(&pkt->msg, sizeof(pkt->msg));
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to send adb msg");
		return ret;
	}

	if (!pkt->msg.data_length)
		return EFI_SUCCESS;

	ret = usb_write(pkt->data, pkt->msg.data_length);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to send adb payload");

	return ret;
}

static void adb_read_msg(void)
{
	EFI_STATUS ret;

	adb_state = ADB_READ_MSG;
	ret = usb_read(&adb_pkt_in.msg, sizeof(adb_pkt_in.msg));
	if (EFI_ERROR(ret))
		efi_perror(ret, L"usb_read failed for next adb message");
}

static void adb_read_msg_payload()
{
	EFI_STATUS ret;

	adb_state = ADB_READ_MSG_PAYLOAD;
	ret = usb_read(adb_pkt_in.data, adb_pkt_in.msg.data_length);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"usb_read failed for adb message payload");

}

/* ADB commands */
static void cmd_unsupported(adb_pkt_t *pkt)
{
	char cmd[5] = { '\0', '\0', '\0', '\0', '\0' };
	*(UINT32 *)cmd = pkt->msg.command;
	*(UINT32 *)cmd = MKID(cmd[0], cmd[1], cmd[2], cmd[3]);

	error(L"'%a' adb message is not supported", cmd);
}

static void cmd_connect(adb_pkt_t *pkt)
{
	EFI_STATUS ret;
	static adb_pkt_t out_pkt;

	if (pkt->msg.arg0 != ADB_VERSION) {
		error(L"Unsupported adb version 0x%08x", pkt->msg.arg0);
		return;
	}

	if (pkt->msg.arg1 != ADB_MAX_PAYLOAD) {
		error(L"Unsupported payload size %d bytes", pkt->msg.arg1);
		return;
	}

	out_pkt.data = (unsigned char *)SYSTEM_TYPE "::";
	out_pkt.msg.data_length = strlen(out_pkt.data);

	ret = adb_send_pkt(&out_pkt, pkt->msg.command, pkt->msg.arg0,
			   pkt->msg.arg1);
	if (EFI_ERROR(ret))
		error(L"Failed to send connection packet");
}

static void cmd_open(adb_pkt_t *pkt)
{
	char *name = (char *)pkt->data;
	char *arg = name;
	service_t *srv = NULL;
	UINTN i;

	if (!pkt->msg.data_length) {
		error(L"Received OPEN packet without any data");
		return;
	}

	while (*arg != ':' && *arg != '\0')
		arg++;
	if (*arg == ':')
		*arg++ = '\0';

	for (i = 0; i < ARRAY_SIZE(SERVICES); i++)
		if (!strcmp((CHAR8 *)name, (CHAR8 *)SERVICES[i]->name)) {
			srv = SERVICES[i];
			break;
		}

	asock_open(pkt->msg.arg0, srv, arg);
}

static void cmd_okay(adb_pkt_t *pkt)
{
	asock_okay(asock_find(pkt->msg.arg1, pkt->msg.arg0));
}

static void cmd_close(adb_pkt_t *pkt)
{
	asock_close(asock_find(pkt->msg.arg1, pkt->msg.arg0));
}

static void cmd_write(adb_pkt_t *pkt)
{
	asock_read(asock_find(pkt->msg.arg1, pkt->msg.arg0),
		   pkt->data, pkt->msg.data_length);
}

typedef struct adb_handler {
	UINT32 command;
	void (*fun)(adb_pkt_t *);
} adb_handler_t;

static adb_handler_t HANDLERS[] = {
	{ A_SYNC, cmd_unsupported },
	{ A_CNXN, cmd_connect },
	{ A_OPEN, cmd_open },
	{ A_OKAY, cmd_okay },
	{ A_CLSE, cmd_close },
	{ A_WRTE, cmd_write },
	{ A_AUTH, cmd_unsupported }
};

static adb_handler_t *get_handler(adb_msg_t *msg)
{
	UINTN i;

	for (i = 0; i < ARRAY_SIZE(HANDLERS); i++)
		if (HANDLERS[i].command == msg->command)
			return &HANDLERS[i];

	return NULL;
}

static void process_msg(void)
{
	adb_handler_t *handler;

	if (adb_state != ADB_PROCESS_MSG)
		return;

	handler = get_handler(&adb_pkt_in.msg);
	if (!handler)
		error(L"Unknown command");
	else
		handler->fun(&adb_pkt_in);

	adb_read_msg();
}

static void adb_process_rx(void *buf, unsigned len)
{
	adb_msg_t *msg;

	switch (adb_state) {
	case ADB_READ_MSG:
		if (buf != &adb_pkt_in || len != sizeof(adb_pkt_in.msg)) {
			error(L"Invalid adb packet buffer reference");
			return;
		}

		msg = (adb_msg_t *)buf;
		if (msg->magic != (msg->command ^ 0xFFFFFFFF)) {
			error(L"Bad magic");
			return;
		}

		if (msg->data_length) {
			adb_read_msg_payload();
			return;
		}

		/* Fastpath for OKAY message for performance purposes.  */
		if (msg->command == A_OKAY) {
			cmd_okay(&adb_pkt_in);
			adb_read_msg();
			return;
		}

		adb_state = ADB_PROCESS_MSG;
		break;

	case ADB_READ_MSG_PAYLOAD:
		if (buf != adb_pkt_in.data) {
			error(L"Invalid adb payload buffer reference");
			return;
		}

		if (len != adb_pkt_in.msg.data_length) {
			error(L"Received 0x%x bytes payload instead of 0x%x bytes",
			      len, adb_pkt_in.msg.data_length);
			return;
		}

		if (adb_pkt_in.msg.data_check != adb_pkt_sum(&adb_pkt_in)) {
			error(L"Corrupted data detected");
			return;
		}

		adb_state = ADB_PROCESS_MSG;
		break;

	default:
		error(L"Inconsistent 0x%x adb state", adb_state);
	}
}

static void adb_process_tx(__attribute__((__unused__)) void *buf,
			   __attribute__((__unused__)) unsigned len)
{
}

static enum boot_target exit_bt;

enum boot_target adb_get_boot_target(void)
{
	return exit_bt;
}

void adb_set_boot_target(enum boot_target bt)
{
	exit_bt = bt;
}

EFI_STATUS adb_init()
{
	static unsigned char in_buf[ADB_MAX_PAYLOAD];
	adb_pkt_in.data = in_buf;
	exit_bt = UNKNOWN_TARGET;

	return usb_init_and_connect(ADB_IF_SUBCLASS,
				    ADB_IF_PROTOCOL,
				    STR_CONFIGURATION,
				    STR_INTERFACE,
				    adb_read_msg,
				    adb_process_rx,
				    adb_process_tx);
}

EFI_STATUS adb_run()
{
	EFI_STATUS ret;

	ret = usb_run();
	if (EFI_ERROR(ret) && ret != EFI_TIMEOUT) {
		efi_perror(ret, L"Error occurred during USB run");
		return ret;
	}

	process_msg();

	return EFI_SUCCESS;
}

EFI_STATUS adb_exit()
{
	asock_close_all();
	usb_disconnect_and_unbind();
	return EFI_SUCCESS;
}