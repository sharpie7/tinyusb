/**************************************************************************/
/*!
    @file     usbd.c
    @author   hathach (tinyusb.org)

    @section LICENSE

    Software License Agreement (BSD License)

    Copyright (c) 2013, hathach (tinyusb.org)
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    3. Neither the name of the copyright holders nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ''AS IS'' AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    This file is part of the tinyusb stack.
*/
/**************************************************************************/

#include "tusb_option.h"

#if MODE_DEVICE_SUPPORTED

#define _TINY_USB_SOURCE_FILE_

//--------------------------------------------------------------------+
// INCLUDE
//--------------------------------------------------------------------+
#include "tusb.h"
#include "tusb_descriptors.h" // TODO callback include
#include "usbd_dcd.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+
usbd_device_info_t usbd_devices[CONTROLLER_DEVICE_NUMBER];

// TODO fix/compress number of class driver
static usbd_class_driver_t const usbd_class_drivers[TUSB_CLASS_MAPPED_INDEX_START] =
{
#if DEVICE_CLASS_HID
    [TUSB_CLASS_HID] =
    {
        .init                    = hidd_init,
        .open                    = hidd_open,
        .control_request_subtask = hidd_control_request_subtask,
        .xfer_cb                 = hidd_xfer_cb,
        .close                   = hidd_close
    },
#endif

#if TUSB_CFG_DEVICE_MSC
    [TUSB_CLASS_MSC] =
    {
        .init                    = mscd_init,
        .open                    = mscd_open,
        .control_request_subtask = mscd_control_request_subtask,
        .xfer_cb                 = mscd_xfer_cb,
        .close                   = mscd_close
    },
#endif

#if TUSB_CFG_DEVICE_CDC
    [TUSB_CLASS_CDC] =
    {
        .init                    = cdcd_init,
        .open                    = cdcd_open,
        .control_request_subtask = cdcd_control_request_subtask,
        .xfer_cb                 = cdcd_xfer_cb,
        .close                   = cdcd_close
    },
#endif

};

//--------------------------------------------------------------------+
// INTERNAL OBJECT & FUNCTION DECLARATION
//--------------------------------------------------------------------+
static tusb_error_t usbd_set_configure_received(uint8_t coreid, uint8_t config_number);
static tusb_error_t get_descriptor(uint8_t coreid, tusb_control_request_t const * const p_request, uint8_t ** pp_buffer, uint16_t * p_length);

//--------------------------------------------------------------------+
// APPLICATION INTERFACE
//--------------------------------------------------------------------+
bool tusbd_is_configured(uint8_t coreid)
{
  return usbd_devices[coreid].state == TUSB_DEVICE_STATE_CONFIGURED;
}

//--------------------------------------------------------------------+
// IMPLEMENTATION
//--------------------------------------------------------------------+

//------------- OSAL Task -------------//
enum {
  USBD_TASK_QUEUE_DEPTH = 8
};

typedef enum {
  USBD_EVENTID_SETUP_RECEIVED = 1,
  USBD_EVENTID_XFER_DONE
}usbd_eventid_t;

typedef struct ATTR_ALIGNED(4)
{
  uint8_t coreid;
  uint8_t event_id;
  uint8_t sub_event_id;
  uint8_t reserved;

  union {
    tusb_control_request_t setup_received; // USBD_EVENTID_SETUP_RECEIVED

    struct { // USBD_EVENTID_XFER_DONE
      endpoint_handle_t edpt_hdl;
      uint32_t xferred_byte;
    }xfer_done;
  };
}usbd_task_event_t;

STATIC_ASSERT(sizeof(usbd_task_event_t) <= 12, "size is not correct");

OSAL_TASK_DEF(usbd_task, 150, TUSB_CFG_OS_TASK_PRIO);
OSAL_QUEUE_DEF(usbd_queue_def, USBD_TASK_QUEUE_DEPTH, usbd_task_event_t);
OSAL_SEM_DEF(usbd_control_xfer_semaphore_def);

static osal_queue_handle_t usbd_queue_hdl;
/*static*/ osal_semaphore_handle_t usbd_control_xfer_sem_hdl; // TODO may need to change to static with wrapper function

tusb_error_t usbd_control_request_subtask(uint8_t coreid, tusb_control_request_t const * const p_request)
{
  OSAL_SUBTASK_BEGIN

  tusb_error_t error = TUSB_ERROR_NONE;

  //------------- Standard Control such as those in enumeration -------------//
  if( TUSB_REQUEST_RECIPIENT_DEVICE == p_request->bmRequestType_bit.recipient &&
      TUSB_REQUEST_TYPE_STANDARD    == p_request->bmRequestType_bit.type )
  {
    if ( TUSB_REQUEST_GET_DESCRIPTOR == p_request->bRequest )
    {
      uint8_t* p_buffer = NULL;
      uint16_t length = 0;

      error = get_descriptor(coreid, p_request, &p_buffer, &length);

      if ( TUSB_ERROR_NONE == error )
      {
        dcd_pipe_control_xfer(coreid, p_request->bmRequestType_bit.direction, p_buffer, length, false);
      }
    }
    else if ( TUSB_REQUEST_SET_ADDRESS == p_request->bRequest )
    {
      dcd_controller_set_address(coreid, (uint8_t) p_request->wValue);
      usbd_devices[coreid].state = TUSB_DEVICE_STATE_ADDRESSED;
    }
    else if ( TUSB_REQUEST_SET_CONFIGURATION == p_request->bRequest )
    {
      usbd_set_configure_received(coreid, (uint8_t) p_request->wValue);
    }else
    {
      error = TUSB_ERROR_DCD_CONTROL_REQUEST_NOT_SUPPORT;
    }
  }

  //------------- Class/Interface Specific Request -------------//
  else if ( TUSB_REQUEST_RECIPIENT_INTERFACE == p_request->bmRequestType_bit.recipient)
  {
    OSAL_VAR tusb_std_class_code_t class_code;

    class_code = usbd_devices[coreid].interface2class[ u16_low_u8(p_request->wIndex) ];

    if ( (TUSB_CLASS_AUDIO <= class_code) && (class_code <= TUSB_CLASS_AUDIO_VIDEO) &&
         usbd_class_drivers[class_code].control_request_subtask )
    {
      OSAL_SUBTASK_INVOKED_AND_WAIT( usbd_class_drivers[class_code].control_request_subtask(coreid, p_request), error );
    }else
    {
      error = TUSB_ERROR_DCD_CONTROL_REQUEST_NOT_SUPPORT;
    }
  }

  //------------- Endpoint Request -------------//
  else if ( TUSB_REQUEST_RECIPIENT_ENDPOINT == p_request->bmRequestType_bit.recipient &&
            TUSB_REQUEST_TYPE_STANDARD      == p_request->bmRequestType_bit.type &&
            TUSB_REQUEST_CLEAR_FEATURE      == p_request->bRequest )
  {
    dcd_pipe_clear_stall(coreid, u16_low_u8(p_request->wIndex) );
  } else
  {
    error = TUSB_ERROR_DCD_CONTROL_REQUEST_NOT_SUPPORT;
  }

  if(TUSB_ERROR_NONE != error)
  { // Response with Protocol Stall if request is not supported
    dcd_pipe_control_stall(coreid);
    //    ASSERT(error == TUSB_ERROR_NONE, VOID_RETURN);
  }else if (p_request->wLength == 0)
  {
    dcd_pipe_control_xfer(coreid, p_request->bmRequestType_bit.direction, NULL, 0, false); // zero length for non-data
  }

  OSAL_SUBTASK_END
}

// To enable the TASK_ASSERT style (quick return on false condition) in a real RTOS, a task must act as a wrapper
// and is used mainly to call subtasks. Within a subtask return statement can be called freely, the task with
// forever loop cannot have any return at all.
OSAL_TASK_FUNCTION(usbd_task) (void* p_task_para)
{
  OSAL_TASK_LOOP_BEGIN

  OSAL_VAR usbd_task_event_t event;
  tusb_error_t error = TUSB_ERROR_NONE;

  osal_queue_receive(usbd_queue_hdl, &event, OSAL_TIMEOUT_WAIT_FOREVER, &error);
  SUBTASK_ASSERT_STATUS(error);

  if ( USBD_EVENTID_SETUP_RECEIVED == event.event_id )
  {
    OSAL_SUBTASK_INVOKED_AND_WAIT( usbd_control_request_subtask(event.coreid, &event.setup_received), error );
  }else
  {
    uint8_t class_index;
    class_index = std_class_code_to_index( event.xfer_done.edpt_hdl.class_code );

    if (usbd_class_drivers[class_index].xfer_cb)
    {
      usbd_class_drivers[class_index].xfer_cb( event.xfer_done.edpt_hdl, event.sub_event_id, event.xfer_done.xferred_byte);
    }else
    {
      hal_debugger_breakpoint(); // something wrong, no one claims the isr's source
    }
  }

  OSAL_TASK_LOOP_END
}

tusb_error_t usbd_init (void)
{
  ASSERT_STATUS ( dcd_init() );

  //------------- Task init -------------//
  usbd_queue_hdl = osal_queue_create( OSAL_QUEUE_REF(usbd_queue_def) );
  ASSERT_PTR(usbd_queue_hdl, TUSB_ERROR_OSAL_QUEUE_FAILED);

  usbd_control_xfer_sem_hdl = osal_semaphore_create( OSAL_SEM_REF(usbd_control_xfer_semaphore_def) );
  ASSERT_PTR(usbd_queue_hdl, TUSB_ERROR_OSAL_SEMAPHORE_FAILED);

  ASSERT_STATUS( osal_task_create( OSAL_TASK_REF(usbd_task) ));

  //------------- class init -------------//
  for (tusb_std_class_code_t class_code = TUSB_CLASS_AUDIO; class_code <= TUSB_CLASS_AUDIO_VIDEO; class_code++)
  {
    if ( usbd_class_drivers[class_code].init )
    {
      usbd_class_drivers[class_code].init();
    }
  }

  return TUSB_ERROR_NONE;
}

//--------------------------------------------------------------------+
// CONTROL REQUEST
//--------------------------------------------------------------------+
// TODO Host (windows) can get HID report descriptor before set configured
// need to open interface before set configured
static tusb_error_t usbd_set_configure_received(uint8_t coreid, uint8_t config_number)
{
  dcd_controller_set_configuration(coreid);
  usbd_devices[coreid].state = TUSB_DEVICE_STATE_CONFIGURED;

  //------------- parse configuration & open drivers -------------//
  uint8_t* p_desc_configure = (uint8_t*) &app_tusb_desc_configuration;
  uint8_t* p_desc = p_desc_configure + sizeof(tusb_descriptor_configuration_t);

  while( p_desc < p_desc_configure + ((tusb_descriptor_configuration_t*)p_desc_configure)->wTotalLength )
  {
    if ( TUSB_DESC_TYPE_INTERFACE_ASSOCIATION == p_desc[DESCRIPTOR_OFFSET_TYPE])
    {
      p_desc += p_desc[DESCRIPTOR_OFFSET_LENGTH]; // ignore IAD
    }else
    {
      ASSERT( TUSB_DESC_TYPE_INTERFACE == p_desc[DESCRIPTOR_OFFSET_TYPE], TUSB_ERROR_NOT_SUPPORTED_YET );

      uint8_t class_index;
      tusb_descriptor_interface_t* p_desc_interface = (tusb_descriptor_interface_t*) p_desc;

      class_index = p_desc_interface->bInterfaceClass;

      ASSERT( class_index != 0 && usbd_class_drivers[class_index].open != NULL, TUSB_ERROR_NOT_SUPPORTED_YET );
      ASSERT( 0 == usbd_devices[coreid].interface2class[p_desc_interface->bInterfaceNumber], TUSB_ERROR_FAILED); // duplicate interface number TODO alternate setting

      usbd_devices[coreid].interface2class[p_desc_interface->bInterfaceNumber] = class_index;

      uint16_t length=0;
      ASSERT_STATUS( usbd_class_drivers[class_index].open( coreid, p_desc_interface, &length ) );

      ASSERT( length >= sizeof(tusb_descriptor_interface_t), TUSB_ERROR_FAILED );
      p_desc += length;
    }
  }

  return TUSB_ERROR_NONE;
}

static tusb_error_t get_descriptor(uint8_t coreid, tusb_control_request_t const * const p_request, uint8_t ** pp_buffer, uint16_t * p_length)
{
  tusb_std_descriptor_type_t const desc_type = u16_high_u8(p_request->wValue);
  uint8_t const desc_index = u16_low_u8( p_request->wValue );

  if ( TUSB_DESC_TYPE_DEVICE == desc_type )
  {
    (*pp_buffer) = (uint8_t *) &app_tusb_desc_device;
    (*p_length)  = sizeof(tusb_descriptor_device_t);
  }
  else if ( TUSB_DESC_TYPE_CONFIGURATION == desc_type )
  {
    (*pp_buffer) = (uint8_t *) &app_tusb_desc_configuration;
    (*p_length)  = sizeof(app_tusb_desc_configuration);
  }
  else if ( TUSB_DESC_TYPE_STRING == desc_type )
  {
    if ( ! (desc_index < TUSB_CFG_DEVICE_STRING_DESCRIPTOR_COUNT) ) return TUSB_ERROR_DCD_CONTROL_REQUEST_NOT_SUPPORT;

    (*pp_buffer) = (uint8_t *) desc_str_table[desc_index];
    (*p_length)  = **pp_buffer;
  }else
  {
    return TUSB_ERROR_DCD_CONTROL_REQUEST_NOT_SUPPORT;
  }

  (*p_length) = min16_of(p_request->wLength, (*p_length) ); // cannot return more than hosts requires

  return TUSB_ERROR_NONE;
}
//--------------------------------------------------------------------+
// USBD-CLASS API
//--------------------------------------------------------------------+

//--------------------------------------------------------------------+
// USBD-DCD Callback API
//--------------------------------------------------------------------+
void usbd_dcd_bus_event_isr(uint8_t coreid, usbd_bus_event_type_t bus_event)
{
  switch(bus_event)
  {
    case USBD_BUS_EVENT_RESET     :
    case USBD_BUS_EVENT_UNPLUGGED :
      memclr_(&usbd_devices[coreid], sizeof(usbd_device_info_t));
      for (tusb_std_class_code_t class_code = TUSB_CLASS_AUDIO; class_code <= TUSB_CLASS_AUDIO_VIDEO; class_code++)
      {
        if ( usbd_class_drivers[class_code].close ) usbd_class_drivers[class_code].close( coreid );
      }
    break;

    case USBD_BUS_EVENT_SUSPENDED:
      usbd_devices[coreid].state = TUSB_DEVICE_STATE_SUSPENDED;
    break;

    default: break;
  }
}

void usbd_setup_received_isr(uint8_t coreid, tusb_control_request_t * p_request)
{
  usbd_task_event_t task_event =
  {
      .coreid          = coreid,
      .event_id        = USBD_EVENTID_SETUP_RECEIVED,
  };

  task_event.setup_received  = (*p_request);
  osal_queue_send(usbd_queue_hdl, &task_event);
}

void usbd_xfer_isr(endpoint_handle_t edpt_hdl, tusb_event_t event, uint32_t xferred_bytes)
{
  if (edpt_hdl.class_code == 0 ) // Control Transfer
  {
    osal_semaphore_post( usbd_control_xfer_sem_hdl );
  }else
  {
    usbd_task_event_t task_event =
    {
        .coreid       = edpt_hdl.coreid,
        .event_id     = USBD_EVENTID_XFER_DONE,
        .sub_event_id = event
    };

    task_event.xfer_done.xferred_byte = xferred_bytes;
    task_event.xfer_done.edpt_hdl     = edpt_hdl;

    osal_queue_send(usbd_queue_hdl, &task_event);
  }
}

//--------------------------------------------------------------------+
// HELPER
//--------------------------------------------------------------------+

#endif
