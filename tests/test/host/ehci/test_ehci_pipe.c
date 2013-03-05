/*
 * test_ehci_pipe.c
 *
 *  Created on: Feb 27, 2013
 *      Author: hathach
 */

/*
 * Software License Agreement (BSD License)
 * Copyright (c) 2012, hathach (tinyusb.net)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the tiny usb stack.
 */

#include "unity.h"
#include "tusb_option.h"
#include "errors.h"
#include "binary.h"

#include "hal.h"
#include "mock_osal.h"
#include "hcd.h"
#include "usbh_hcd.h"
#include "ehci.h"
#include "test_ehci.h"

extern ehci_data_t ehci_data;
usbh_device_info_t usbh_device_info_pool[TUSB_CFG_HOST_DEVICE_MAX+1];

LPC_USB0_Type lpc_usb0;
LPC_USB1_Type lpc_usb1;

uint8_t const control_max_packet_size = 64;
uint8_t dev_addr;
uint8_t hub_addr;
uint8_t hub_port;
uint8_t hostid;
uint8_t xfer_data [100];

ehci_qhd_t *async_head;

tusb_descriptor_endpoint_t const desc_ept_bulk_in =
{
    .bLength          = sizeof(tusb_descriptor_endpoint_t),
    .bDescriptorType  = TUSB_DESC_ENDPOINT,
    .bEndpointAddress = 0x81,
    .bmAttributes     = { .xfer = TUSB_XFER_BULK },
    .wMaxPacketSize   = 512,
    .bInterval        = 0
};

//--------------------------------------------------------------------+
// Setup/Teardown + helper declare
//--------------------------------------------------------------------+
void setUp(void)
{
  memclr_(&lpc_usb0, sizeof(LPC_USB0_Type));
  memclr_(&lpc_usb1, sizeof(LPC_USB1_Type));

  memclr_(usbh_device_info_pool, sizeof(usbh_device_info_t)*(TUSB_CFG_HOST_DEVICE_MAX+1));
  memclr_(&ehci_data, sizeof(ehci_data_t));
  memclr_(xfer_data, sizeof(xfer_data));

  hcd_init();

  dev_addr = 1;

  hostid = RANDOM(CONTROLLER_HOST_NUMBER) + TEST_CONTROLLER_HOST_START_INDEX;
  hub_addr = 2;
  hub_port = 2;

  for (uint8_t i=0; i<TUSB_CFG_HOST_DEVICE_MAX; i++)
  {
    usbh_device_info_pool[i].core_id  = hostid;
    usbh_device_info_pool[i].hub_addr = hub_addr;
    usbh_device_info_pool[i].hub_port = hub_port;
  }

  async_head =  get_async_head( hostid );
}

void tearDown(void)
{
}

void verify_open_qhd(ehci_qhd_t *p_qhd)
{
  TEST_ASSERT_EQUAL(dev_addr, p_qhd->device_address);
  TEST_ASSERT_FALSE(p_qhd->inactive_next_xact);
  TEST_ASSERT_EQUAL(0, p_qhd->nak_count_reload); // TODO NAK Reload disable
  TEST_ASSERT_EQUAL(hub_addr, p_qhd->hub_address);
  TEST_ASSERT_EQUAL(hub_port, p_qhd->hub_port);
  TEST_ASSERT_EQUAL(1, p_qhd->mult);
  TEST_ASSERT(p_qhd->qtd_overlay.next.terminate);
  TEST_ASSERT(p_qhd->qtd_overlay.alternate.terminate);

  //------------- HCD -------------//
  TEST_ASSERT(p_qhd->used);
  TEST_ASSERT_NULL(p_qhd->p_qtd_list);
}

//--------------------------------------------------------------------+
// CONTROL PIPE
//--------------------------------------------------------------------+
void verify_control_open_qhd(ehci_qhd_t *p_qhd)
{
  verify_open_qhd(p_qhd);

  TEST_ASSERT_EQUAL(control_max_packet_size, p_qhd->max_package_size);
  TEST_ASSERT_EQUAL(0, p_qhd->endpoint_number);
  TEST_ASSERT_EQUAL(1, p_qhd->data_toggle_control);
  TEST_ASSERT_EQUAL(0, p_qhd->smask);
  TEST_ASSERT_EQUAL(0, p_qhd->cmask);
}

void test_control_open_addr0_qhd_data(void)
{
  dev_addr = 0;

  ehci_qhd_t * const p_qhd = async_head;

  hcd_pipe_control_open(dev_addr, control_max_packet_size);

  verify_control_open_qhd(p_qhd);
  TEST_ASSERT(p_qhd->head_list_flag);
}

void test_control_open_qhd_data(void)
{
  ehci_qhd_t * const p_qhd = &ehci_data.device[dev_addr].control.qhd;

  hcd_pipe_control_open(dev_addr, control_max_packet_size);

  verify_control_open_qhd(p_qhd);
  TEST_ASSERT_FALSE(p_qhd->head_list_flag);

  //------------- async list check -------------//
  TEST_ASSERT_EQUAL_HEX((uint32_t) p_qhd, align32(async_head->next.address));
  TEST_ASSERT_FALSE(async_head->next.terminate);
  TEST_ASSERT_EQUAL(EHCI_QUEUE_ELEMENT_QHD, async_head->next.type);
}

void test_control_open_highspeed(void)
{
  ehci_qhd_t * const p_qhd = &ehci_data.device[dev_addr].control.qhd;

  usbh_device_info_pool[dev_addr].speed   = TUSB_SPEED_HIGH;

  hcd_pipe_control_open(dev_addr, control_max_packet_size);

  TEST_ASSERT_EQUAL(TUSB_SPEED_HIGH, p_qhd->endpoint_speed);
  TEST_ASSERT_FALSE(p_qhd->non_hs_control_endpoint);
}

void test_control_open_non_highspeed(void)
{
  ehci_qhd_t * const p_qhd = &ehci_data.device[dev_addr].control.qhd;

  usbh_device_info_pool[dev_addr].speed   = TUSB_SPEED_FULL;

  hcd_pipe_control_open(dev_addr, control_max_packet_size);

  TEST_ASSERT_EQUAL(TUSB_SPEED_FULL, p_qhd->endpoint_speed);
  TEST_ASSERT_TRUE(p_qhd->non_hs_control_endpoint);
}

//--------------------------------------------------------------------+
// BULK PIPE
//--------------------------------------------------------------------+
void verify_bulk_open_qhd(ehci_qhd_t *p_qhd, tusb_descriptor_endpoint_t const * desc_endpoint)
{
  verify_open_qhd(p_qhd);

  TEST_ASSERT_FALSE(p_qhd->head_list_flag);
  TEST_ASSERT_EQUAL(desc_endpoint->wMaxPacketSize, p_qhd->max_package_size);
  TEST_ASSERT_EQUAL(desc_endpoint->bEndpointAddress & 0x0F, p_qhd->endpoint_number);
  TEST_ASSERT_EQUAL(0, p_qhd->data_toggle_control);
  TEST_ASSERT_EQUAL(0, p_qhd->smask);
  TEST_ASSERT_EQUAL(0, p_qhd->cmask);
  TEST_ASSERT_FALSE(p_qhd->non_hs_control_endpoint);
  TEST_ASSERT_EQUAL(usbh_device_info_pool[dev_addr].speed, p_qhd->endpoint_speed);

  //  TEST_ASSERT_EQUAL(desc_endpoint->bInterval); TEST highspeed bulk/control OUT

  TEST_ASSERT_EQUAL(desc_endpoint->bEndpointAddress & 0x80 ? EHCI_PID_IN : EHCI_PID_OUT, p_qhd->pid_non_control);

  //------------- async list check -------------//
  TEST_ASSERT_EQUAL_HEX((uint32_t) p_qhd, align32(async_head->next.address));
  TEST_ASSERT_FALSE(async_head->next.terminate);
  TEST_ASSERT_EQUAL(EHCI_QUEUE_ELEMENT_QHD, async_head->next.type);
}

void test_open_bulk_qhd_data(void)
{
  ehci_qhd_t *p_qhd;
  pipe_handle_t pipe_hdl;
  tusb_descriptor_endpoint_t const * desc_endpoint = &desc_ept_bulk_in;

  pipe_hdl = hcd_pipe_open(dev_addr, desc_endpoint);

  p_qhd = &ehci_data.device[ pipe_hdl.dev_addr ].qhd[ pipe_hdl.index ];
  verify_bulk_open_qhd(p_qhd, desc_endpoint);

  //------------- async list check -------------//
  TEST_ASSERT_EQUAL_HEX((uint32_t) p_qhd, align32(async_head->next.address));
  TEST_ASSERT_FALSE(async_head->next.terminate);
  TEST_ASSERT_EQUAL(EHCI_QUEUE_ELEMENT_QHD, async_head->next.type);
}

//--------------------------------------------------------------------+
// CONTROL TRANSFER
//--------------------------------------------------------------------+
tusb_std_request_t request_get_dev_desc =
{
    .bmRequestType = { .direction = TUSB_DIR_DEV_TO_HOST, .type = TUSB_REQUEST_TYPE_STANDARD, .recipient = TUSB_REQUEST_RECIPIENT_DEVICE },
    .bRequest = TUSB_REQUEST_GET_DESCRIPTOR,
    .wValue   = (TUSB_DESC_DEVICE << 8),
    .wLength  = 18
};

void test_control_xfer_get(void)
{
  ehci_qhd_t * const p_qhd = &ehci_data.device[dev_addr].control.qhd;
  hcd_pipe_control_open(dev_addr, control_max_packet_size);

  //------------- Code Under TEST -------------//
  hcd_pipe_control_xfer(dev_addr, &request_get_dev_desc, xfer_data);

  ehci_qtd_t *p_setup  = &ehci_data.device[dev_addr].control.qtd[0];
  ehci_qtd_t *p_data   = &ehci_data.device[dev_addr].control.qtd[1];
  ehci_qtd_t *p_status = &ehci_data.device[dev_addr].control.qtd[2];

  TEST_ASSERT_EQUAL_HEX( p_setup  , p_qhd->p_qtd_list);
  TEST_ASSERT_EQUAL_HEX( p_data   , p_setup->next.address);
  TEST_ASSERT_EQUAL_HEX( p_status , p_data->next.address );
  TEST_ASSERT_TRUE( p_status->next.terminate );
}

void test_control_addr0_xfer_get(void)
{
  dev_addr = 0;
  ehci_qhd_t * const p_qhd = async_head;
  hcd_pipe_control_open(dev_addr, control_max_packet_size);

  //------------- Code Under TEST -------------//
  hcd_pipe_control_xfer(dev_addr, &request_get_dev_desc, xfer_data);

  ehci_qtd_t *p_setup  = &ehci_data.controller.addr0_qtd[0];
  ehci_qtd_t *p_data   = &ehci_data.controller.addr0_qtd[1];
  ehci_qtd_t *p_status = &ehci_data.controller.addr0_qtd[2];

  TEST_ASSERT_EQUAL_HEX( p_setup  , p_qhd->p_qtd_list);
  TEST_ASSERT_EQUAL_HEX( p_data   , p_setup->next.address);
  TEST_ASSERT_EQUAL_HEX( p_status , p_data->next.address );
  TEST_ASSERT_TRUE( p_status->next.terminate );
}


void test_control_xfer_set(void)
{
  tusb_std_request_t request_set_dev_addr =
  {
      .bmRequestType = { .direction = TUSB_DIR_HOST_TO_DEV, .type = TUSB_REQUEST_TYPE_STANDARD, .recipient = TUSB_REQUEST_RECIPIENT_DEVICE },
      .bRequest = TUSB_REQUEST_SET_ADDRESS,
      .wValue   = 3
  };

  ehci_qhd_t * const p_qhd = &ehci_data.device[dev_addr].control.qhd;
  hcd_pipe_control_open(dev_addr, control_max_packet_size);

  //------------- Code Under TEST -------------//
  hcd_pipe_control_xfer(dev_addr, &request_set_dev_addr, xfer_data);

  ehci_qtd_t *p_setup  = &ehci_data.device[dev_addr].control.qtd[0];
  ehci_qtd_t *p_data   = &ehci_data.device[dev_addr].control.qtd[1];
  ehci_qtd_t *p_status = &ehci_data.device[dev_addr].control.qtd[2];

  TEST_ASSERT_EQUAL_HEX( p_setup  , p_qhd->p_qtd_list);
  TEST_ASSERT_EQUAL_HEX( p_status , p_setup->next.address );
  TEST_ASSERT_TRUE( p_status->next.terminate );
}


