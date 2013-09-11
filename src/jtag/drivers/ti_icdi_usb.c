/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2012 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* project specific includes */
#include <helper/binarybuffer.h>
#include <jtag/interface.h>
#include <jtag/hla/hla_layout.h>
#include <jtag/hla/hla_transport.h>
#include <jtag/hla/hla_interface.h>
#include <target/target.h>

#include <target/cortex_m.h>

#include <libusb.h>

#define ICDI_WRITE_ENDPOINT 0x02
#define ICDI_READ_ENDPOINT 0x83

#define ICDI_WRITE_TIMEOUT 1000
#define ICDI_READ_TIMEOUT 1000
#define ICDI_PACKET_SIZE 2048

#define PACKET_START "$"
#define PACKET_END "#"

struct icdi_usb_handle_s {
	libusb_context *usb_ctx;
	libusb_device_handle *usb_dev;

	char *read_buffer;
	char *write_buffer;
	int max_packet;
	int read_count;
};

static int icdi_usb_read_mem32(void *handle, uint32_t addr, uint16_t len, uint8_t *buffer);
static int icdi_usb_write_mem32(void *handle, uint32_t addr, uint16_t len, const uint8_t *buffer);

static int remote_escape_output(const char *buffer, int len, char *out_buf, int *out_len, int out_maxlen)
{
	int input_index, output_index;

	output_index = 0;

	for (input_index = 0; input_index < len; input_index++) {

		char b = buffer[input_index];

		if (b == '$' || b == '#' || b == '}' || b == '*') {
			/* These must be escaped.  */
			if (output_index + 2 > out_maxlen)
				break;
			out_buf[output_index++] = '}';
			out_buf[output_index++] = b ^ 0x20;
		} else {
			if (output_index + 1 > out_maxlen)
				break;
			out_buf[output_index++] = b;
		}
	}

	*out_len = input_index;
	return output_index;
}

static int remote_unescape_input(const char *buffer, int len, char *out_buf, int out_maxlen)
{
	int input_index, output_index;
	int escaped;

	output_index = 0;
	escaped = 0;

	for (input_index = 0; input_index < len; input_index++) {

		char b = buffer[input_index];

		if (output_index + 1 > out_maxlen)
			LOG_ERROR("Received too much data from the target.");

		if (escaped) {
			out_buf[output_index++] = b ^ 0x20;
			escaped = 0;
		} else if (b == '}')
			escaped = 1;
		else
			out_buf[output_index++] = b;
	}

	if (escaped)
		LOG_ERROR("Unmatched escape character in target response.");

	return output_index;
}

static int icdi_send_packet(void *handle, int len)
{
	unsigned char cksum = 0;
	struct icdi_usb_handle_s *h;
	int result, retry = 0;
	int transferred = 0;

	assert(handle != NULL);
	h = (struct icdi_usb_handle_s *)handle;

	/* check we have a large enough buffer for checksum "#00" */
	if (len + 3 > h->max_packet) {
		LOG_ERROR("packet buffer too small");
		return ERROR_FAIL;
	}

	/* calculate checksum - offset start of packet */
	for (int i = 1; i < len; i++)
		cksum += h->write_buffer[i];

	len += sprintf(&h->write_buffer[len], PACKET_END "%02x", cksum);

#ifdef _DEBUG_USB_COMMS_
	char buffer[50];
	char ch = h->write_buffer[1];
	if (ch == 'x' || ch == 'X')
		LOG_DEBUG("writing packet: <binary>");
	else {
		memcpy(buffer, h->write_buffer, len >= 50 ? 50-1 : len);
		buffer[len] = 0;
		LOG_DEBUG("writing packet: %s", buffer);
	}
#endif

	while (1) {

		result = libusb_bulk_transfer(h->usb_dev, ICDI_WRITE_ENDPOINT, (unsigned char *)h->write_buffer, len,
				&transferred, ICDI_WRITE_TIMEOUT);
		if (result != 0 || transferred != len) {
			LOG_DEBUG("Error TX Data %d", result);
			return ERROR_FAIL;
		}

		/* check that the client got the message ok, or shall we resend */
		result = libusb_bulk_transfer(h->usb_dev, ICDI_READ_ENDPOINT, (unsigned char *)h->read_buffer, h->max_packet,
					&transferred, ICDI_READ_TIMEOUT);
		if (result != 0 || transferred < 1) {
			LOG_DEBUG("Error RX Data %d", result);
			return ERROR_FAIL;
		}

#ifdef _DEBUG_USB_COMMS_
		LOG_DEBUG("received reply: '%c' : count %d", h->read_buffer[0], transferred);
#endif

		if (h->read_buffer[0] == '-') {
			LOG_DEBUG("Resending packet %d", ++retry);
		} else {
			if (h->read_buffer[0] != '+')
				LOG_DEBUG("Unexpected Reply from ICDI: %c", h->read_buffer[0]);
			break;
		}

		if (retry == 3) {
			LOG_DEBUG("maximum nack retries attempted");
			return ERROR_FAIL;
		}
	}

	retry = 0;
	h->read_count = transferred;

	while (1) {

		/* read reply from icdi */
		result = libusb_bulk_transfer(h->usb_dev, ICDI_READ_ENDPOINT, (unsigned char *)h->read_buffer + h->read_count,
				h->max_packet - h->read_count, &transferred, ICDI_READ_TIMEOUT);

#ifdef _DEBUG_USB_COMMS_
		LOG_DEBUG("received data: count %d", transferred);
#endif

		/* check for errors but retry for timeout */
		if (result != 0) {

			if (result == LIBUSB_ERROR_TIMEOUT) {
				LOG_DEBUG("Error RX timeout %d", result);
			} else {
				LOG_DEBUG("Error RX Data %d", result);
				return ERROR_FAIL;
			}
		}

		h->read_count += transferred;

		/* we need to make sure we have a full packet, including checksum */
		if (h->read_count > 5) {

			/* check that we have received an packet delimiter
			 * we do not validate the checksum
			 * reply should contain $...#AA - so we check for # */
			if (h->read_buffer[h->read_count - 3] == '#')
				return ERROR_OK;
		}

		if (retry++ == 3) {
			LOG_DEBUG("maximum data retries attempted");
			break;
		}
	}

	return ERROR_FAIL;
}

static int icdi_send_cmd(void *handle, const char *cmd)
{
	struct icdi_usb_handle_s *h;
	h = (struct icdi_usb_handle_s *)handle;

	int cmd_len = snprintf(h->write_buffer, h->max_packet, PACKET_START "%s", cmd);
	return icdi_send_packet(handle, cmd_len);
}

static int icdi_send_remote_cmd(void *handle, const char *data)
{
	struct icdi_usb_handle_s *h;
	h = (struct icdi_usb_handle_s *)handle;

	size_t cmd_len = sprintf(h->write_buffer, PACKET_START "qRcmd,");
	cmd_len += hexify(h->write_buffer + cmd_len, data, 0, h->max_packet - cmd_len);

	return icdi_send_packet(handle, cmd_len);
}

static int icdi_get_cmd_result(void *handle)
{
	struct icdi_usb_handle_s *h;
	int offset = 0;
	char ch;

	assert(handle != NULL);
	h = (struct icdi_usb_handle_s *)handle;

	do {
		ch = h->read_buffer[offset++];
		if (offset > h->read_count)
			return ERROR_FAIL;
	} while (ch != '$');

	if (memcmp("OK", h->read_buffer + offset, 2) == 0)
		return ERROR_OK;

	if (h->read_buffer[offset] == 'E') {
		/* get error code */
		char result;
		if (unhexify(&result, h->read_buffer + offset + 1, 1) != 1)
			return ERROR_FAIL;
		return result;
	}

	/* for now we assume everything else is ok */
	return ERROR_OK;
}

static int icdi_usb_idcode(void *handle, uint32_t *idcode)
{
	return ERROR_OK;
}

static int icdi_usb_write_debug_reg(void *handle, uint32_t addr, uint32_t val)
{
	return icdi_usb_write_mem32(handle, addr, 1, (uint8_t *)&val);
}

static enum target_state icdi_usb_state(void *handle)
{
	int result;
	struct icdi_usb_handle_s *h;
	uint32_t dhcsr;

	h = (struct icdi_usb_handle_s *)handle;

	result = icdi_usb_read_mem32(h, DCB_DHCSR, 1, (uint8_t *)&dhcsr);
	if (result != ERROR_OK)
		return TARGET_UNKNOWN;

	if (dhcsr & S_HALT)
		return TARGET_HALTED;

	return TARGET_RUNNING;
}

static int icdi_usb_version(void *handle)
{
	struct icdi_usb_handle_s *h;
	h = (struct icdi_usb_handle_s *)handle;

	char version[20];

	/* get info about icdi */
	int result = icdi_send_remote_cmd(handle, "version");
	if (result != ERROR_OK)
		return result;

	if (h->read_count < 8) {
		LOG_ERROR("Invalid Reply Received");
		return ERROR_FAIL;
	}

	/* convert reply */
	if (unhexify(version, h->read_buffer + 2, 4) != 4) {
		LOG_WARNING("unable to get ICDI version");
		return ERROR_OK;
	}

	/* null terminate and print info */
	version[4] = 0;

	LOG_INFO("ICDI Firmware version: %s", version);

	return ERROR_OK;
}

static int icdi_usb_query(void *handle)
{
	int result;

	struct icdi_usb_handle_s *h;
	h = (struct icdi_usb_handle_s *)handle;

	result = icdi_send_cmd(handle, "qSupported");
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("query supported failed: 0x%x", result);
		return ERROR_FAIL;
	}

	/* from this we can get the max packet supported */

	/* query packet buffer size */
	char *offset = strstr(h->read_buffer, "PacketSize");
	if (offset) {
		char *separator;
		int max_packet;

		max_packet = strtoul(offset + 11, &separator, 16);
		if (!max_packet)
			LOG_ERROR("invalid max packet, using defaults");
		else
			h->max_packet = max_packet;
		LOG_DEBUG("max packet supported : %" PRIu32 " bytes", h->max_packet);
	}


	/* if required re allocate packet buffer */
	if (h->max_packet != ICDI_PACKET_SIZE) {
		h->read_buffer = realloc(h->read_buffer, h->max_packet);
		h->write_buffer = realloc(h->write_buffer, h->max_packet);
		if (h->read_buffer == 0 || h->write_buffer == 0) {
			LOG_ERROR("unable to reallocate memory");
			return ERROR_FAIL;
		}
	}

	/* set extended mode */
	result = icdi_send_cmd(handle, "!");
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("unable to enable extended mode: 0x%x", result);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int icdi_usb_reset(void *handle)
{
	/* we do this in hla_target.c */
	return ERROR_OK;
}

static int icdi_usb_assert_srst(void *handle, int srst)
{
	/* TODO not supported yet */
	return ERROR_COMMAND_NOTFOUND;
}

static int icdi_usb_run(void *handle)
{
	int result;

	/* resume target at current address */
	result = icdi_send_cmd(handle, "c");
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("continue failed: 0x%x", result);
		return ERROR_FAIL;
	}

	return result;
}

static int icdi_usb_halt(void *handle)
{
	int result;

	/* this query halts the target ?? */
	result = icdi_send_cmd(handle, "?");
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("halt failed: 0x%x", result);
		return ERROR_FAIL;
	}

	return result;
}

static int icdi_usb_step(void *handle)
{
	int result;

	/* step target at current address */
	result = icdi_send_cmd(handle, "s");
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("step failed: 0x%x", result);
		return ERROR_FAIL;
	}

	return result;
}

static int icdi_usb_read_regs(void *handle)
{
	/* currently unsupported */
	return ERROR_OK;
}

static int icdi_usb_read_reg(void *handle, int num, uint32_t *val)
{
	int result;
	struct icdi_usb_handle_s *h;
	char cmd[10];

	h = (struct icdi_usb_handle_s *)handle;

	snprintf(cmd, sizeof(cmd), "p%x", num);
	result = icdi_send_cmd(handle, cmd);
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("register read failed: 0x%x", result);
		return ERROR_FAIL;
	}

	/* convert result */
	if (unhexify((char *)val, h->read_buffer + 2, 4) != 4) {
		LOG_ERROR("failed to convert result");
		return ERROR_FAIL;
	}

	return result;
}

static int icdi_usb_write_reg(void *handle, int num, uint32_t val)
{
	int result;
	char cmd[20];

	int cmd_len = snprintf(cmd, sizeof(cmd), "P%x=", num);
	hexify(cmd + cmd_len, (char *)&val, 4, sizeof(cmd));

	result = icdi_send_cmd(handle, cmd);
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("register write failed: 0x%x", result);
		return ERROR_FAIL;
	}

	return result;
}

static int icdi_usb_read_mem(void *handle, uint32_t addr, uint32_t len, uint8_t *buffer)
{
	int result;
	struct icdi_usb_handle_s *h;
	char cmd[20];

	h = (struct icdi_usb_handle_s *)handle;

	snprintf(cmd, sizeof(cmd), "x%x,%x", addr, len);
	result = icdi_send_cmd(handle, cmd);
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("memory read failed: 0x%x", result);
		return ERROR_FAIL;
	}

	/* unescape input */
	int read_len = remote_unescape_input(h->read_buffer + 5, h->read_count - 8, (char *)buffer, len);
	if (read_len != (int)len) {
		LOG_ERROR("read more bytes than expected: actual 0x%" PRIx32 " expected 0x%" PRIx32, read_len, len);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int icdi_usb_write_mem(void *handle, uint32_t addr, uint32_t len, const uint8_t *buffer)
{
	int result;
	struct icdi_usb_handle_s *h;

	h = (struct icdi_usb_handle_s *)handle;

	size_t cmd_len = snprintf(h->write_buffer, h->max_packet, PACKET_START "X%x,%x:", addr, len);

	int out_len;
	cmd_len += remote_escape_output((char *)buffer, len, h->write_buffer + cmd_len,
			&out_len, h->max_packet - cmd_len);

	if (out_len < (int)len) {
		/* for now issue a error as we have no way of allocating a larger buffer */
		LOG_ERROR("memory buffer too small: requires 0x%" PRIx32 " actual 0x%" PRIx32, out_len, len);
		return ERROR_FAIL;
	}

	result = icdi_send_packet(handle, cmd_len);
	if (result != ERROR_OK)
		return result;

	/* check result */
	result = icdi_get_cmd_result(handle);
	if (result != ERROR_OK) {
		LOG_ERROR("memory write failed: 0x%x", result);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int icdi_usb_read_mem8(void *handle, uint32_t addr, uint16_t len, uint8_t *buffer)
{
	return icdi_usb_read_mem(handle, addr, len, buffer);
}

static int icdi_usb_write_mem8(void *handle, uint32_t addr, uint16_t len, const uint8_t *buffer)
{
	return icdi_usb_write_mem(handle, addr, len, buffer);
}

static int icdi_usb_read_mem32(void *handle, uint32_t addr, uint16_t len, uint8_t *buffer)
{
	return icdi_usb_read_mem(handle, addr, len * 4, buffer);
}

static int icdi_usb_write_mem32(void *handle, uint32_t addr, uint16_t len, const uint8_t *buffer)
{
	return icdi_usb_write_mem(handle, addr, len * 4, buffer);
}

static int icdi_usb_close(void *handle)
{
	struct icdi_usb_handle_s *h;

	h = (struct icdi_usb_handle_s *)handle;

	if (h->usb_dev)
		libusb_close(h->usb_dev);

	if (h->usb_ctx)
		libusb_exit(h->usb_ctx);

	if (h->read_buffer)
		free(h->read_buffer);

	if (h->write_buffer)
		free(h->write_buffer);

	free(handle);

	return ERROR_OK;
}

static int icdi_usb_open(struct hl_interface_param_s *param, void **fd)
{
	int retval;
	struct icdi_usb_handle_s *h;

	LOG_DEBUG("icdi_usb_open");

	h = calloc(1, sizeof(struct icdi_usb_handle_s));

	if (h == 0) {
		LOG_ERROR("unable to allocate memory");
		return ERROR_FAIL;
	}

	LOG_DEBUG("transport: %d vid: 0x%04x pid: 0x%04x", param->transport,
		param->vid, param->pid);

	if (libusb_init(&h->usb_ctx) != 0) {
		LOG_ERROR("libusb init failed");
		goto error_open;
	}

	h->usb_dev = libusb_open_device_with_vid_pid(h->usb_ctx, param->vid, param->pid);
	if (!h->usb_dev) {
		LOG_ERROR("open failed");
		goto error_open;
	}

	if (libusb_claim_interface(h->usb_dev, 2)) {
		LOG_DEBUG("claim interface failed");
		goto error_open;
	}

	/* check if mode is supported */
	retval = ERROR_OK;

	switch (param->transport) {
#if 0
		/* TODO place holder as swd is not currently supported */
		case HL_TRANSPORT_SWD:
#endif
		case HL_TRANSPORT_JTAG:
			break;
		default:
			retval = ERROR_FAIL;
			break;
	}

	if (retval != ERROR_OK) {
		LOG_ERROR("mode (transport) not supported by device");
		goto error_open;
	}

	/* allocate buffer */
	h->read_buffer = malloc(ICDI_PACKET_SIZE);
	h->write_buffer = malloc(ICDI_PACKET_SIZE);
	h->max_packet = ICDI_PACKET_SIZE;

	if (h->read_buffer == 0 || h->write_buffer == 0) {
		LOG_DEBUG("malloc failed");
		goto error_open;
	}

	/* query icdi version etc */
	retval = icdi_usb_version(h);
	if (retval != ERROR_OK)
		goto error_open;

	/* query icdi support */
	retval = icdi_usb_query(h);
	if (retval != ERROR_OK)
		goto error_open;

	*fd = h;

	/* set the max target read/write buffer in bytes
	 * as we are using gdb binary packets to transfer memory we have to
	 * reserve half the buffer for any possible escape chars plus
	 * at least 64 bytes for the gdb packet header */
	param->max_buffer = (((h->max_packet - 64) / 4) * 4) / 2;

	return ERROR_OK;

error_open:
	icdi_usb_close(h);

	return ERROR_FAIL;
}

struct hl_layout_api_s icdi_usb_layout_api = {
	.open = icdi_usb_open,
	.close = icdi_usb_close,
	.idcode = icdi_usb_idcode,
	.state = icdi_usb_state,
	.reset = icdi_usb_reset,
	.assert_srst = icdi_usb_assert_srst,
	.run = icdi_usb_run,
	.halt = icdi_usb_halt,
	.step = icdi_usb_step,
	.read_regs = icdi_usb_read_regs,
	.read_reg = icdi_usb_read_reg,
	.write_reg = icdi_usb_write_reg,
	.read_mem8 = icdi_usb_read_mem8,
	.write_mem8 = icdi_usb_write_mem8,
	.read_mem32 = icdi_usb_read_mem32,
	.write_mem32 = icdi_usb_write_mem32,
	.write_debug_reg = icdi_usb_write_debug_reg
};
