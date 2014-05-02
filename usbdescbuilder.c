/* Copyright (c) 2014 LEAP Motion. All rights reserved.
*
* The intellectual and technical concepts contained herein are proprietary and
* confidential to Leap Motion, and are protected by trade secret or copyright
* law. Dissemination of this information or reproduction of this material is
* strictly forbidden unless prior written permission is obtained from LEAP
* Motion.
*/

#include <string.h>
#include <stdarg.h>

#include "USBBldr.h"
#include "usbdescbuilder.h"

// Ongoing: this is a library. The API must be documented (fairly well)
// Ongoing: the whole smart-grouping part

// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// Internals

// On Cypress, I saw two cases in which memcpy() to the fields of the
// descriptor being built wound up at the wrong offsets within the structure.
// A full day of poking around with differing forms of packing and
// structuring did not resolve this. Overriding whatever memcpy() I am
// resolving against on Cypress immediately resolved the issue. Until this
// can be proved out, I'm keeping a local memcpy(). This is
// used only for 2,3, and 4-byte moves and so performance is
// not really a concern.

// For posterity: the failing cases were in
//   usbdescbldr_make_vc_interface_header()
//   usbdescbldr_make_vc_interrupt_ep()

static void *
udb_memcpy(void *dest, const void * src, size_t l)
{
	uint8_t *dp, *sp;

	dp = (uint8_t *) dest;
	sp = (uint8_t *) src;

	while (l--)
		*dp++ = *sp++;

	return dest;
}
#define memcpy(d,s,l) udb_memcpy((d),(s),(l))


static size_t
_bufferAvailable(usbdescbldr_ctx_t *ctx)
{
    // Meaningless unless there's a buffer, so presume the caller
    // has already verified that.
    return ctx->bufferSize - (ctx->append - ctx->buffer);
}


// In case we have no ntohs() et alia:

static uint16_t
_endianShortNoOp(uint16_t s)
{
    return s;
}


static unsigned int
_endianIntNoOp(unsigned int s)
{
    return s;
}


static uint16_t
_endianShortSwap(uint16_t s)
{
    uint16_t result;

    result = ((((s >> 8) & 0xff) << 0) |
              (((s >> 0) & 0xff) << 8));

    return result;
}


static unsigned int
_endianIntSwap(unsigned int s)
{
    unsigned int result;

    result = ((((s >> 24) & 0xff) << 0) |
              (((s >> 16) & 0xff) << 8) |
              (((s >> 8) & 0xff) << 16) |
              (((s >> 0) & 0xff) << 24));

    return result;
}

// //////////////////////////////////////////////////////////////////
// Item actions

static void 
_item_init(usbdescbldr_item_t * item)
{
    memset(item, 0, sizeof(*item));
}



// Make a set of items subordinate to one parent item. This
// is used to allow the parent item to account for their accumulated lengths.
// Pass the context, a result item, and the subordinate items.

usbdescbldr_status_t
usbdescbldr_add_children(usbdescbldr_ctx_t *    ctx,
                         usbdescbldr_item_t *   parent,
                         ...) // Item pointers, then NULL
{
  va_list              va, va_count;
  usbdescbldr_item_t * ip;
  uint32_t             n;
  uint16_t             p16, s16;    // temps for Parent, Subordinate

  if(parent == NULL)
    return USBDESCBLDR_INVALID;

  // Everything should include itself.
  p16 = parent->size;
  if(parent->totalSize != NULL) {
    memcpy(&p16, parent->totalSize, sizeof(p16));
    p16 = ctx->fLittleShortToHost(p16);
  }
  if(p16 == 0)
    p16 = parent->size;

  va_start(va, parent);
  va_copy(va_count, va);

  // Count the new subordinates
  for(n = 0; (ip = va_arg(va_count, usbdescbldr_item_t *)) != NULL; n++)
    ;
  va_end(va_count);

  if(parent->items + n > USBDESCBLDR_MAX_CHILDREN) {
    va_end(va);
    return USBDESCBLDR_TOO_MANY;
  }

  for(; n > 0; n--) {
    ip = va_arg(va, usbdescbldr_item_t *);
    // (Repeating myself:) Everything should include itself.
    // This time, it's mostly just a little convenience for the API-level code (below).
    s16 = ip->size;
    if(ip->totalSize != NULL) {
      memcpy(&s16, ip->totalSize, sizeof(s16));
      s16 = ctx->fLittleShortToHost(s16);
    }
    if(s16 == 0) s16 = ip->size;
    p16 += s16;

    // Just in case for future: maintain the hierarchy
    parent->item[parent->items++] = ip;
  }
  va_end(va);

  // Save the result back into the descriptor
  if(parent->totalSize != NULL) {
    p16 = ctx->fHostToLittleShort(p16);
    memcpy(parent->totalSize, &p16, sizeof(p16));
  }

  return USBDESCBLDR_OK;
}


// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// API


// //////////////////////////////////////////////////////////////////
// Setup and teardown

// Reset internal state and begin a new descriptor.
// Passing NULL for the buffer indicates a 'dry run' --
// go through all the motions, checks, and size computations
// but do not actually create a descriptor.

usbdescbldr_status_t
usbdescbldr_init(usbdescbldr_ctx_t *  ctx,
                 unsigned char *      buffer,
                 size_t               bufferSize)
{
  // Do not concern ourselves with initializing something
  // that wasn't 'end'ed, as we do not have allocations to release.

  const uint16_t endian = 0x1234;

  memset(ctx, 0, sizeof(*ctx));

  // Determine host uint16_t order
  if ((*(uint8_t *)&endian) == 0x34) {
    // Host is little-endian; less to do
    ctx->fLittleShortToHost = _endianShortNoOp;
    ctx->fHostToLittleShort = _endianShortNoOp;
    ctx->fLittleIntToHost = _endianIntNoOp;
    ctx->fHostToLittleInt = _endianIntNoOp;
  } else {
    // Host is big-endian
    ctx->fLittleShortToHost = _endianShortSwap;
    ctx->fHostToLittleShort = _endianShortSwap;
    ctx->fLittleIntToHost = _endianIntSwap;
    ctx->fHostToLittleInt = _endianIntSwap;
  }

  if (buffer != NULL) {
    ctx->buffer = buffer;
    ctx->bufferSize = bufferSize;
    ctx->append = buffer;
    // last_append remains NULL, as there is no 'last' at this point
  }

  ctx->initialized = 1;
  return USBDESCBLDR_OK;
}


// Commit (complete/finish) the descriptor in progress.
// Collections, interfaces, etc left open will be tidied up
// (if possible..?)
usbdescbldr_status_t
usbdescbldr_close(usbdescbldr_ctx_t *  ctx)
{
  return USBDESCBLDR_OK;
}


// Terminate use of the builder. Release any resources.
// Once this is complete, the API requires an _init() before
// anything will perform again.
usbdescbldr_status_t
usbdescbldr_end(usbdescbldr_ctx_t *  ctx)
{
  return USBDESCBLDR_OK;
}

// //////////////////////////////////////////////////////////////////
// Constructions


// Build a device descriptor based upon those values given in the short form by the

usbdescbldr_status_t
usbdescbldr_make_device_descriptor(usbdescbldr_ctx_t *ctx,
                                   usbdescbldr_item_t *item,
                                   usbdescbldr_device_descriptor_short_form_t *form)
{
  USB_DEVICE_DESCRIPTOR *dest;
  uint16_t tShort;

  if(form == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // This item has a fixed length; check for 'fit'
  if(ctx->buffer != NULL) {
    if(sizeof(*dest) >= _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;
  }

  // begin construction
  // Fill in buffer unless in dry-run
  if(ctx->buffer != NULL) {
    dest = (USB_DEVICE_DESCRIPTOR *) ctx->append;
    memset(dest, 0, sizeof(*dest));

    dest->header.bLength = sizeof(*dest);
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_DEVICE;

    tShort = ctx->fHostToLittleShort(form->bcdUSB);
    memcpy(&dest->bcdUSB, &tShort, sizeof(dest->bcdUSB));

    dest->bDeviceClass = form->bDeviceClass;
    dest->bDeviceSubClass = form->bDeviceSubClass;

    dest->bDeviceProtocol = form->bDeviceProtocol;

    // Default the maxPacketSize: 64 for USB 2.x and 3.x 
    if(form->bcdUSB < 0x0300)
      dest->bMaxPacketSize0 = 64;
    else
      dest->bMaxPacketSize0 = 9; // 2^9 == 512

    tShort = ctx->fHostToLittleShort(form->idVendor);
    memcpy(&dest->idVendor, &tShort, sizeof(dest->idVendor));

    tShort = ctx->fHostToLittleShort(form->idProduct);
    memcpy(&dest->idProduct, &tShort, sizeof(dest->idProduct));

    tShort = ctx->fHostToLittleShort(form->bcdDevice);
    memcpy(&dest->bcdDevice, &tShort, sizeof(dest->bcdDevice));

    dest->iManufacturer = form->iManufacturer;
    dest->iProduct = form->iProduct;
    dest->iSerialNumber = form->iSerialNumber;
    dest->bNumConfigurations = form->bNumConfigurations;
  } // filling in buffer

  // Build the output item
  _item_init(item);
  item->size = sizeof(*dest);
  item->address = ctx->append;

  ctx->append += sizeof(*dest);

  return USBDESCBLDR_OK;
}


// Generate a USB Device Qualifier Descriptor.
usbdescbldr_status_t
usbdescbldr_make_device_qualifier_descriptor(usbdescbldr_ctx_t * ctx,
                                             usbdescbldr_item_t * item,
                                             usbdescbldr_device_qualifier_short_form_t * form)
{
  USB_DEVICE_QUALIFIER_DESCRIPTOR *dest;
  uint16_t tShort;

  if(form == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // This item has a fixed length; check for 'fit'
  if (ctx->buffer != NULL) {
    if (sizeof(*dest) > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;
  }

  // begin construction
  // Fill in buffer unless in dry-run
  if (ctx->buffer != NULL) {
    dest = (USB_DEVICE_QUALIFIER_DESCRIPTOR *) ctx->append;

    dest->header.bLength = sizeof(*dest);

    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER;

    tShort = ctx->fHostToLittleShort(form->bcdUSB);
    memcpy(&dest->bcdUSB, &tShort, sizeof(dest->bcdUSB));

    dest->bDeviceClass = form->bDeviceClass;
    dest->bDeviceSubClass = form->bDeviceSubClass;
    dest->bDeviceProtocol = form->bDeviceProtocol;

    // Default the maxPacketSize: 64 for USB 2.x and 3.x 
    if (form->bcdUSB < 0x0300)
      dest->bMaxPacketSize0 = 64;
    else
      dest->bMaxPacketSize0 = 9; // 2^9 == 512

    dest->bNumConfigurations = form->bNumConfigurations;
    dest->bReserved = 0;
  } // filling in buffer

  // Build result
  _item_init(item);
  item->size = sizeof(*dest);
  item->address = ctx->append;

  // Consume buffer
    ctx->append += sizeof(*dest);

  return USBDESCBLDR_OK;
}


// Generate a Device Configuration descriptor. 

usbdescbldr_status_t
usbdescbldr_make_device_configuration_descriptor(usbdescbldr_ctx_t * ctx,
                                                 usbdescbldr_item_t * item,
                                                 usbdescbldr_device_configuration_short_form_t * form)
{
  USB_CONFIGURATION_DESCRIPTOR *dest = NULL;

  if(form == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // This item has a fixed length; check for 'fit'
  if(ctx->buffer != NULL) {
    if(sizeof(*dest) > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;
  }

  // begin construction
  // Fill in buffer unless in dry-run
  if(ctx->buffer != NULL) {
    dest = (USB_CONFIGURATION_DESCRIPTOR *) ctx->append;
    memset(dest, 0, sizeof(*dest));

    dest->header.bLength = sizeof(*dest);
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_CONFIGURATION;

    dest->bNumInterfaces = form->bNumInterfaces;
    dest->bConfigurationValue = form->bConfigurationValue;
    dest->iConfiguration = form->iConfiguration;
    dest->bmAttributes = form->bmAttributes;
    dest->MaxPower = form->bMaxPower;
  } // filling in buffer

  // Build result
  _item_init(item);
  item->size = sizeof(*dest);
  item->address = ctx->append;
  item->totalSize = (uint8_t *) &dest->wTotalLength;

  // Consume buffer
  ctx->append += sizeof(*dest);

  return USBDESCBLDR_OK;
}


// Create the language descriptor (actually string, index 0).

usbdescbldr_status_t
usbdescbldr_make_languageIDs(usbdescbldr_ctx_t *ctx,
                             usbdescbldr_item_t *item,
                             ...)
{
  va_list             va_count, va_do;
  unsigned int        langs;
  uint16_t            lang;
  size_t              needs;
  unsigned char *     drop;
  USB_STRING_DESCRIPTOR *dest = (USB_STRING_DESCRIPTOR *)ctx->append;

  if(ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // There may only be one (this is string index zero..)
  if(ctx->i_string != 0)
    return USBDESCBLDR_TOO_MANY;

  va_start(va_do, item);      // One copy for work
  va_copy(va_count, va_do);   // .. one copy just to count

  // Count args until the terminator
  for(langs = 0; va_arg(va_count, uint32_t) != USBDESCBLDR_LIST_END; langs++)
    ;
  va_end(va_count);

  // Now we know our length
  needs = sizeof(*dest) + langs * sizeof(lang);

  // Bounds check
  if(needs > 0xff) {
    va_end(va_do);
    return USBDESCBLDR_OVERSIZED;
  }

  // If not dry-run, be sure we can write
  if(ctx->buffer != NULL && _bufferAvailable(ctx) < needs) {
    va_end(va_do);
    return USBDESCBLDR_NO_SPACE;
  }

  // Continue construction
  if(ctx->buffer != NULL) {
    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_STRING;

    // In Strings, the string unichars immediately follow the header
    drop = ((unsigned char *) dest) + sizeof(USB_DESCRIPTOR_HEADER);
    for(; langs > 0; langs--) {
      lang = (uint16_t) va_arg(va_do, unsigned int);
      lang = ctx->fHostToLittleShort(lang);
      memcpy(drop, &lang, sizeof(lang));
      drop += sizeof(lang);
    }
  }
  va_end(va_do);

  // Build the item for the caller
  _item_init(item);
  item->address = ctx->append;
  item->size = needs;
  item->index = ctx->i_string;

  // Advance the buffer
  ctx->append += needs;

  // This counts as string index 0
  ctx->i_string++;

  return USBDESCBLDR_OK;
}





// Define a new string and obtain its index.

usbdescbldr_status_t 
usbdescbldr_make_string_descriptor(usbdescbldr_ctx_t *ctx,
                                   usbdescbldr_item_t *item, // OUT
                                   uint8_t *index, // OUT
                                   const char *string) // IN
{
  USB_STRING_DESCRIPTOR *dest;
  size_t needs;
  unsigned char *drop;
  const char *ascii;
  uint16_t wchar;

  if(string == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // String indices are bytes, limiting the number of them:
  if (ctx->i_string > 0xff)
    return USBDESCBLDR_TOO_MANY;
    
  // This has a fixed length, so check up front
  needs = strlen(string) * sizeof(wchar) + sizeof(*dest);
  if (needs > 0xff)
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..

  // Construct
  // (No need to stack)
  if (ctx->buffer != NULL) {
    if (needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_STRING_DESCRIPTOR *) ctx->append;
    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_STRING;

    // In Strings, the string unichars immediately follow the header
    drop = ((unsigned char *) dest) + sizeof(USB_DESCRIPTOR_HEADER);
    for(ascii = string; *ascii; ++ascii) {		// (Do not copy the NULL)
      wchar = (uint16_t) *ascii;			        // (zero-extending, very explicitly)
      wchar = ctx->fHostToLittleShort(wchar);
      memcpy(drop, & wchar, sizeof(wchar));
      drop += sizeof(wchar);
    } 
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;
  item->index = ctx->i_string;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  // Give the caller the assigned string index, if they want it
  if (index != NULL) 
    *index = ctx->i_string;
  ctx->i_string++;

  return USBDESCBLDR_OK;
}


usbdescbldr_status_t
usbdescbldr_make_bos_descriptor(usbdescbldr_ctx_t * ctx,
                                usbdescbldr_item_t * item)
{
  USB_BOS_DESCRIPTOR *dest;

  if(ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;
  
  // Check space
  if (ctx->buffer != NULL) {
    if (sizeof(*dest) > _bufferAvailable(ctx)) 
      return USBDESCBLDR_NO_SPACE;
  }

  // begin construction
  dest = (USB_BOS_DESCRIPTOR *) ctx->append;
  // Fill in buffer unless in dry-run
  if (ctx->buffer != NULL) {
    dest = (USB_BOS_DESCRIPTOR *) ctx->append;
    memset(dest, 0, sizeof(*dest));
    dest->header.bLength = sizeof(*dest);
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_BOS;
  }

  // Build the item 
  _item_init(item);
  item->size = sizeof(*dest);
  item->address = ctx->append;

  ctx->append += sizeof(*dest);

  return USBDESCBLDR_OK;
}

usbdescbldr_status_t
usbdescbldr_make_device_capability_descriptor(usbdescbldr_ctx_t * ctx,
                                              usbdescbldr_item_t * item,
                                              uint8_t	           bDevCapabilityType,
                                              uint8_t *	           typeDependent,	// Anonymous byte data
                                              size_t	           typeDependentSize)
{
  USB_DEVICE_CAPABILITY_DESCRIPTOR *dest;
  size_t needs;

  if(ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  if(typeDependentSize != 0 && typeDependent == NULL)
    return USBDESCBLDR_INVALID;

  // This has a fixed length, so check up front
  needs = sizeof(*dest);
  needs += typeDependentSize;

  if(needs > 0xff)
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_DEVICE_CAPABILITY_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_DEVICE_CAPABILITY;

    dest->bDevCapabilityType = bDevCapabilityType;
    memcpy(dest + 1, typeDependent, typeDependentSize);
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}


// Generate a Standard Interface descriptor.

usbdescbldr_status_t
usbdescbldr_make_standard_interface_descriptor(usbdescbldr_ctx_t * ctx,
                                                usbdescbldr_item_t * item,
                                                usbdescbldr_standard_interface_short_form_t * form)
{
  USB_INTERFACE_DESCRIPTOR *dest;
  size_t needs;

  if(form == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // This has a fixed length, so check up front
  needs = sizeof(*dest);
  if(needs > 0xff)
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_INTERFACE_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_INTERFACE;

    dest->bInterfaceNumber = form->bInterfaceNumber;
    dest->bAlternateSetting = form->bAlternateSetting;
    dest->bNumEndpoints = form->bNumEndpoints;
    dest->bInterfaceClass = form->bInterfaceClass;
    dest->bInterfaceSubClass   = form->bInterfaceSubClass;
    dest->bInterfaceProtocol = form->bInterfaceProtocol;
    dest->iInterface = form->iInterface;
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}

// Generate an Endpoint descriptor.
usbdescbldr_status_t
usbdescbldr_make_endpoint_descriptor(usbdescbldr_ctx_t * ctx,
                                     usbdescbldr_item_t * item,
                                     usbdescbldr_endpoint_short_form_t * form)
{
  USB_ENDPOINT_DESCRIPTOR *dest;
  uint16_t tShort;
  size_t needs;

  if(form == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // This has a fixed length, so check up front
  needs = sizeof(*dest);
  if(needs > 0xff)
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_ENDPOINT_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->bLength = needs;
    dest->bDescriptorType = USB_DESCRIPTOR_TYPE_ENDPOINT;

    dest->bEndpointAddress = form->bEndpointAddress;
    dest->bmAttributes = form->bmAttributes;
    tShort = ctx->fHostToLittleShort(form->wMaxPacketSize);
    memcpy(&dest->wMaxPacketSize, &tShort, sizeof(dest->wMaxPacketSize));
    dest->bInterval = form->bInterval;
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;
  item->index = form->bEndpointAddress;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}

// Generate an Endpoint Companion descriptor for SuperSpeed operation.
usbdescbldr_status_t
usbdescbldr_make_ss_ep_companion_descriptor(usbdescbldr_ctx_t * ctx,
                                            usbdescbldr_item_t * item,
                                            usbdescbldr_ss_ep_companion_short_form_t * form)
{
  USB_SS_EP_COMPANION_DESCRIPTOR *dest;
  uint16_t tShort;
  size_t needs;

  if(form == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // This has a fixed length, so check up front
  needs = sizeof(*dest);
  if(needs > 0xff)
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_SS_EP_COMPANION_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_SS_EP_COMPANION;

    dest->bMaxBurst = form->bMaxBurst;
    dest->bmAttributes = form->bmAttributes;
    tShort = ctx->fHostToLittleShort(form->wBytesPerInterval);
    memcpy(&dest->wBytesPerInterval, &tShort, sizeof(dest->wBytesPerInterval));
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}

// Generate an Interface Association descriptor.

usbdescbldr_status_t
usbdescbldr_make_interface_association_descriptor(usbdescbldr_ctx_t * ctx,
                                                  usbdescbldr_item_t * item,
                                                  usbdescbldr_iad_short_form_t * form)
{
  USB_INTERFACE_ASSOCIATION_DESCRIPTOR *dest;
  size_t needs;

  if(form == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // This has a fixed length, so check up front
  needs = sizeof(*dest);
  if(needs > 0xff)
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_INTERFACE_ASSOCIATION_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION;

    dest->bFirstInterface = form->bFirstInterface;
    dest->bInterfaceCount = form->bInterfaceCount;
    dest->bFunctionClass = form->bFunctionClass;
    dest->bFunctionSubClass = form->bFunctionSubClass;
    dest->bFunctionProtocol = form->bFunctionProtocol;
    dest->iFunction = form->iFunction;
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}



usbdescbldr_status_t
usbdescbldr_make_vc_interface_descriptor(usbdescbldr_ctx_t * ctx,
                                         usbdescbldr_item_t * item,
                                         usbdescbldr_vc_interface_short_form_t * form)
{
  usbdescbldr_standard_interface_short_form_t iForm;

  if(ctx == NULL || item == NULL || form == NULL)
    return USBDESCBLDR_INVALID;

  iForm.bInterfaceNumber = form->bInterfaceNumber;
  iForm.bAlternateSetting = form->bAlternateSetting;
  iForm.bNumEndpoints = form->bNumEndpoints;
  iForm.iInterface = form->iInterface;

  // We can do the rest ourselves:
  iForm.bInterfaceClass = USB_INTERFACE_CC_VIDEO;
  iForm.bInterfaceSubClass = USB_INTERFACE_VC_SC_VIDEOCONTROL;
  iForm.bInterfaceProtocol = USB_INTERFACE_VC_PC_PROTOCOL_15;

  return usbdescbldr_make_standard_interface_descriptor(ctx, item, & iForm);
}



usbdescbldr_status_t
usbdescbldr_make_vs_interface_descriptor(usbdescbldr_ctx_t * ctx,
                                         usbdescbldr_item_t * item,
usbdescbldr_vs_interface_short_form_t * form)
{
  usbdescbldr_standard_interface_short_form_t iForm;

  if(ctx == NULL || item == NULL || form == NULL)
    return USBDESCBLDR_INVALID;

  iForm.bInterfaceNumber = form->bInterfaceNumber;
  iForm.bAlternateSetting = form->bAlternateSetting;
  iForm.bNumEndpoints = form->bNumEndpoints;
  iForm.iInterface = form->iInterface;

  // We can do the rest ourselves:
  iForm.bInterfaceClass = USB_INTERFACE_CC_VIDEO;
  iForm.bInterfaceSubClass = USB_INTERFACE_VC_SC_VIDEOSTREAMING;
  iForm.bInterfaceProtocol = USB_INTERFACE_VC_PC_PROTOCOL_15;

  return usbdescbldr_make_standard_interface_descriptor(ctx, item, & iForm);
}



// The VC CS Interface Header Descriptor. It is treated as a header for 
// numerous items that will follow it once built. The header itself has
// a variable number of interfaces at the end, which are given by their
// interface numbers in a list, terminated with USBDESCBLDR_END_LIST .

usbdescbldr_status_t
usbdescbldr_make_vc_interface_header(usbdescbldr_ctx_t * ctx,
                                     usbdescbldr_item_t * item,
                                     unsigned int dwClockFrequency,
                                     ... // Terminated List of Interface Numbers
)
{
  USB_VC_CS_INTERFACE_DESCRIPTOR * dest = NULL;
  size_t needs;
  uint16_t tShort;
  uint32_t tInt;

  uint8_t   bInCollection;      // Number of interfaces
  uint8_t * drop;
  va_list   va, va_count;

  if(ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // Determine the final length
  va_start(va_count, dwClockFrequency);
  va_copy(va, va_count);

  for(bInCollection = 0; va_arg(va_count, uint32_t) != USBDESCBLDR_LIST_END; bInCollection++)
    // COULD range-check each interface to be a byte value here
    ;
  va_end(va_count);

  needs = sizeof(*dest) + sizeof(uint8_t) * bInCollection;
  if(needs > 0xff) {
    va_end(va);
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..
  }

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx)) {
      va_end(va);
      return USBDESCBLDR_NO_SPACE;
    }

    dest = (USB_VC_CS_INTERFACE_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->header.bDescriptorSubtype = USB_INTERFACE_SUBTYPE_VC_HEADER;

    tShort = ctx->fHostToLittleShort(UVC_CLASS);
    memcpy(&dest->bcdUVC, &tShort, sizeof(dest->bcdUVC));

    tInt = ctx->fHostToLittleInt(dwClockFrequency);
    memcpy(&dest->dwClockFrequency, &tInt, sizeof(dest->dwClockFrequency));
    dest->bInCollection = bInCollection;

    // Tack on the interface(s)
    drop = (uint8_t *) (dest + 1);
    for(; bInCollection > 0; bInCollection--)
      *drop++ = (uint8_t) va_arg(va, uint32_t);
  }
  va_end(va);

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;
  item->totalSize = (uint8_t *) &dest->wTotalLength;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}


// The VC Camera Terminal (an Input) Descriptor.

usbdescbldr_status_t
usbdescbldr_make_camera_terminal_descriptor(usbdescbldr_ctx_t * ctx,
usbdescbldr_item_t * item,
usbdescbldr_camera_terminal_short_form_t * form)
{
  USB_UVC_CAMERA_TERMINAL *dest = NULL;
  size_t needs;
  uint32_t t32;

  if(form == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  needs = sizeof(*dest);

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_UVC_CAMERA_TERMINAL *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = UVC_CS_INTERFACE;
    dest->bDescriptorSubType = USB_INTERFACE_SUBTYPE_VC_INPUT_TERMINAL;

    dest->bTerminalID = form->bTerminalID;

    t32 = ctx->fHostToLittleShort(USB_UVC_ITT_CAMERA);
    memcpy(&dest->wTerminalType, &t32, sizeof(dest->wTerminalType));

    dest->bAssocTerminal = form->bAssocTerminal;
    dest->iTerminal = form->iTerminal;

    t32 = ctx->fHostToLittleShort(form->wObjectiveFocalLengthMin);
    memcpy(&dest->wObjectiveFocalLengthMin, &t32, sizeof(dest->wObjectiveFocalLengthMin));

    t32 = ctx->fHostToLittleShort(form->wObjectiveFocalLengthMax);
    memcpy(&dest->wObjectiveFocalLengthMax, &t32, sizeof(dest->wObjectiveFocalLengthMax));

    t32 = ctx->fHostToLittleShort(form->wOcularFocalLength);
    memcpy(&dest->wOcularFocalLength, &t32, sizeof(dest->wOcularFocalLength));

    // Using t32 as a 3-byte buffer; just don't copy the whole four bytes
    dest->bControlBitfieldSize = sizeof(dest->bmControls);
    t32 = ctx->fHostToLittleInt(form->controls);
    memcpy(&dest->bmControls, &t32, sizeof(dest->bmControls));
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}


// The VC Streaming Output Terminal Descriptor.

usbdescbldr_status_t
usbdescbldr_make_streaming_out_terminal_descriptor(usbdescbldr_ctx_t * ctx,
usbdescbldr_item_t * item,
usbdescbldr_streaming_out_terminal_short_form_t * form)
{
  USB_UVC_STREAMING_OUT_TERMINAL *dest;
  size_t needs;
  uint32_t t32;

  if(form == NULL || ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  needs = sizeof(*dest);

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_UVC_STREAMING_OUT_TERMINAL *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = UVC_CS_INTERFACE;
    dest->bDescriptorSubType = USB_INTERFACE_SUBTYPE_VC_OUTPUT_TERMINAL;

    dest->bTerminalID = form->bTerminalID;

    t32 = ctx->fHostToLittleShort(USB_UVC_OTT_STREAMING);
    memcpy(&dest->wTerminalType, &t32, sizeof(dest->wTerminalType));

    dest->bAssocTerminal = form->bAssocTerminal;
    dest->bSourceID = form->bSourceID;
    dest->iTerminal = form->iTerminal;
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}



// The Selector Unit.

usbdescbldr_status_t
usbdescbldr_make_vc_selector_unit(usbdescbldr_ctx_t * ctx,
                                  usbdescbldr_item_t * item,
                                  uint8_t iSelector, // string index
                                  uint8_t bUnitID,
                                  ... // Terminated List of Input (Source) Pin(s)
)
{
  USB_UVC_VC_SELECTOR_UNIT * dest;
  size_t needs;
  uint8_t   bNrInPins;      // Number of inputs
  uint8_t * drop;
  va_list   va, va_count;

  if(ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  // Determine the final length
  va_start(va_count, bUnitID);
  va_copy(va, va_count);

  for(bNrInPins = 0; va_arg(va_count, uint32_t) != USBDESCBLDR_LIST_END; bNrInPins++)
    // COULD range-check each source to be a byte value here
    ;
  va_end(va_count);

  needs = sizeof(*dest) + sizeof(uint8_t) * bNrInPins + sizeof(iSelector);
  if(needs > 0xff) {
    va_end(va);
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..
  }

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx)) {
      va_end(va);
      return USBDESCBLDR_NO_SPACE;
    }

    dest = (USB_UVC_VC_SELECTOR_UNIT *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->bDescriptorSubType = USB_INTERFACE_SUBTYPE_VC_SELECTOR_UNIT;

    dest->bUnitID = bUnitID;
    
    // Tack on the bNrInPins and interface(s)
    drop = (uint8_t *) (dest + 1);
    *drop++ = bNrInPins;
    for(; bNrInPins > 0; bNrInPins--)
      *drop++ = (uint8_t) va_arg(va, uint32_t);

    // .. and the iSelector (string).
    *drop = iSelector;
  }
  va_end(va);

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}




// The Processor Unit.

usbdescbldr_status_t
usbdescbldr_make_vc_processor_unit(usbdescbldr_ctx_t * ctx,
                                   usbdescbldr_item_t * item,
                                   usbdescbldr_vc_processor_unit_short_form * form)
{
  USB_UVC_VC_PROCESSING_UNIT * dest;
  size_t needs;
   uint32_t t32;
   uint16_t t16;

  if(ctx == NULL || form == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  needs = sizeof(*dest);

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_UVC_VC_PROCESSING_UNIT *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->bDescriptorSubType = USB_INTERFACE_SUBTYPE_VC_PROCESSING_UNIT;

    dest->bUnitID = form->bUnitID;
    dest->bSourceID = form->bSourceID;

    t16 = ctx->fHostToLittleShort(form->wMaxMultiplier);
    memcpy(&dest->wMaxMultiplier, &t16, sizeof(dest->wMaxMultiplier));

    dest->bControlSize = sizeof(dest->bmControls);
    t32 = ctx->fHostToLittleInt(form->controls);
    memcpy(&dest->bmControls, &t32, sizeof(dest->bmControls));

    dest->iProcessing = form->iProcessing;
    dest->bmVideoStandards = form->bmVideoStandards;
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}



usbdescbldr_status_t
usbdescbldr_make_extension_unit_descriptor(usbdescbldr_ctx_t * ctx,
                                           usbdescbldr_item_t * item,
                                           usbdescbldr_vc_extension_unit_short_form_t * form,
                                           ...) // Varying number of SourceIDs.
{
  USB_UVC_VC_EXTENSION_UNIT * dest;
  size_t needs;
  uint8_t   bNrInPins;      // Number of inputs
  uint8_t * drop;
  va_list   va, va_count;
  GUID      tGUID;

  if(item == NULL)
    return USBDESCBLDR_INVALID;

  // Determine the final length
  va_start(va_count, form);
  va_copy(va, va_count);

  for(bNrInPins = 0; va_arg(va_count, uint32_t) != USBDESCBLDR_LIST_END; bNrInPins++)
    // COULD range-check each source to be a byte value here
    ;
  va_end(va_count);

  // A complex structure, with multiple varying-size fields *and* fixed-sized
  // ones intermingled among them.
  needs = sizeof(*dest);                  // Prefix of fixed-size fields
  needs += sizeof(uint8_t) * bNrInPins;   // baSourceID
  needs += sizeof(uint8_t);               // bControlSize
  needs += form->bControlSize;            // bmControls
  needs += sizeof(uint8_t);               // iExtension

  if(needs > 0xff) {
    va_end(va);
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..
  }

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx)) {
      va_end(va);
      return USBDESCBLDR_NO_SPACE;
    }

    dest = (USB_UVC_VC_EXTENSION_UNIT *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->bDescriptorSubType = USB_INTERFACE_SUBTYPE_VC_EXTENSION_UNIT;

    dest->bUnitID = form->bUnitID;

    tGUID.dwData1 = ctx->fHostToLittleInt(form->guidExtensionCode.dwData1);
    tGUID.dwData2 = ctx->fHostToLittleShort(form->guidExtensionCode.dwData2);
    tGUID.dwData3 = ctx->fHostToLittleShort(form->guidExtensionCode.dwData3);
    memcpy(&tGUID.dwData4, form->guidExtensionCode.dwData4, sizeof(tGUID.dwData4));
    memcpy(&dest->guidExtensionCode, &tGUID, sizeof(dest->guidExtensionCode));

    dest->bNumControls = form->bNumControls;
    dest->bNrInPins = bNrInPins;

    // Tack on the sources(s): baSourceID
    drop = (uint8_t *) (dest + 1);
    for(; bNrInPins > 0; bNrInPins--)
      *drop++ = (uint8_t) va_arg(va, uint32_t);

    // bControlSize
    *drop++ = form->bControlSize;

    // bmControls
    memcpy(drop, form->bmControls, form->bControlSize);
    drop += form->bControlSize;

    // iExtension
    *drop++ = form->iExtension;
  }
  va_end(va);

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}

// UVC Class-Specific VC interrupt endpoint:

usbdescbldr_status_t
usbdescbldr_make_vc_interrupt_ep(usbdescbldr_ctx_t * ctx,
                                 usbdescbldr_item_t * item,
                                 uint16_t wMaxTransferSize)
{
  USB_VC_CS_INTR_EP_DESCRIPTOR * dest;
  size_t needs;
  uint16_t t16;

  if(ctx == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  needs = sizeof(*dest);

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (USB_VC_CS_INTR_EP_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_ENDPOINT;
    dest->header.bDescriptorSubtype = USB_VC_SUBTYPE_EP_INTERRUPT;

    t16 = ctx->fHostToLittleShort(wMaxTransferSize);
    memcpy(&dest->wMaxTransferSize, &t16, sizeof(dest->wMaxTransferSize));
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}



// UVC Video Stream Interface Input Header

usbdescbldr_status_t
usbdescbldr_make_vs_interface_header(usbdescbldr_ctx_t * ctx,
                                     usbdescbldr_item_t * item,
                                     usbdescbldr_vs_if_input_header_short_form_t * form,
                                     ...) // Varying: bmaControls (which are passed as int32s), terminated by USBDESCBLDR_LIST_END
{
  USB_UVC_VS_INPUT_HEADER_DESCRIPTOR * dest = NULL;
  size_t needs;
  uint8_t   bNumFormats;      // Number of formats (controls)
  uint8_t * drop;             // Place to drop next built member
  va_list   va, va_count;

  if(item == NULL || form == NULL)
    return USBDESCBLDR_INVALID;

  // Determine the final length
  va_start(va_count, form);
  va_copy(va, va_count);

  for(bNumFormats = 0; va_arg(va_count, uint32_t) != USBDESCBLDR_LIST_END; bNumFormats++)
    // COULD range-check each source to be a byte value here
    ;
  va_end(va_count);

  needs = sizeof(*dest);                    // Prefix of fixed-size fields
  needs += sizeof(uint8_t) * bNumFormats;   // bmaControls

  if(needs > 0xff) {
    va_end(va);
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..
  }

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx)) {
      va_end(va);
      return USBDESCBLDR_NO_SPACE;
    }

    dest = (USB_UVC_VS_INPUT_HEADER_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->bDescriptorSubType = USB_INTERFACE_SUBTYPE_VS_INPUT_HEADER;

    dest->bNumFormats = bNumFormats;
    dest->bEndpointAddress = form->bEndpointAddress;
    dest->bmInfo = form->bmInfo;
    dest->bTerminalLink = form->bTerminalLink;
    dest->bStillCaptureMethod = form->bStillCaptureMethod;
    dest->bTriggerSupport = form->bTriggerSupport;
    dest->bTriggerUsage = form->bTriggerUsage;
    dest->bControlSize = sizeof(uint8_t); // Not very general, but standardized (for now)

    drop = (uint8_t *) (dest + 1); // Beginning of bmaControls

    // Tack on the Controls. These are 8-bit values, but were upcast to int32s by the call
    for(; bNumFormats > 0; bNumFormats--)
      *drop++ = (uint8_t) va_arg(va, uint32_t);
  }
  va_end(va);

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;
  item->totalSize = (uint8_t *) &dest->wTotalLength;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}


// UVC Video Stream Interface Output Header

usbdescbldr_status_t
usbdescbldr_make_uvc_vs_if_output_header(usbdescbldr_ctx_t * ctx,
                                         usbdescbldr_item_t * item,
                                         usbdescbldr_vs_if_output_header_short_form_t * form,
                                         ...) // Varying: bmaControls (which are passed as int32s), terminated by USBDESCBLDR_LIST_END
{
  USB_UVC_VS_OUTPUT_HEADER_DESCRIPTOR * dest;
  size_t needs;
  uint8_t   bNumFormats;      // Number of formats (controls)
  uint8_t * drop;             // Place to drop next built member

  va_list   va, va_count;

  if(item == NULL || form == NULL)
    return USBDESCBLDR_INVALID;

  // Determine the final length
  va_start(va_count, form);
  va_copy(va, va_count);

  for(bNumFormats = 0; va_arg(va_count, uint32_t) != USBDESCBLDR_LIST_END; bNumFormats++)
    // COULD range-check each source to be a byte value here
    ;
  va_end(va_count);

  needs = sizeof(*dest);                    // Prefix of fixed-size fields
  needs += sizeof(uint8_t) * bNumFormats;   // bmaControls

  if(needs > 0xff) {
    va_end(va);
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..
  }

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx)) {
      va_end(va);
      return USBDESCBLDR_NO_SPACE;
    }

    dest = (USB_UVC_VS_OUTPUT_HEADER_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->bDescriptorSubType = USB_INTERFACE_SUBTYPE_VS_INPUT_HEADER;

    dest->bNumFormats = bNumFormats;
    dest->bEndpointAddress = form->bEndpointAddress;
    dest->bTerminalLink = form->bTerminalLink;
    dest->bControlSize = sizeof(uint8_t); // Not very general, but standardized (for now)

    drop = (uint8_t *) (dest + 1); // Beginning of bmaControls

    // Tack on the Controls. These are 8-bit values, but were upcast to int32s by the call
    for(; bNumFormats > 0; bNumFormats--)
      *drop++ = (uint8_t) va_arg(va, uint32_t);
  }
  va_end(va);

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}


// Payload Format Descriptors
// UVC Video Stream Format (Frame Based)

usbdescbldr_status_t
usbdescbldr_make_uvc_vs_format_frame(usbdescbldr_ctx_t * ctx,
                                     usbdescbldr_item_t * item,
                                     usbdescbldr_uvc_vs_format_frame_based_short_form_t * form)
{
  UVC_VS_FORMAT_FRAME_DESCRIPTOR * dest;
  size_t needs;
  GUID     tGUID;

  if(ctx == NULL || form == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  needs = sizeof(*dest);

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (UVC_VS_FORMAT_FRAME_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->header.bDescriptorSubtype = USB_INTERFACE_SUBTYPE_VS_FORMAT_FRAME_BASED;

    dest->bFormatIndex = form->bFormatIndex;
    dest->bNumFrameDescriptors = form->bNumFrameDescriptors;

    tGUID.dwData1 = ctx->fHostToLittleInt(form->guidFormat.dwData1);
    tGUID.dwData2 = ctx->fHostToLittleShort(form->guidFormat.dwData2);
    tGUID.dwData3 = ctx->fHostToLittleShort(form->guidFormat.dwData3);
    memcpy(&tGUID.dwData4, form->guidFormat.dwData4, sizeof(tGUID.dwData4));
    memcpy(&dest->guidFormat, &tGUID, sizeof(dest->guidFormat));

    dest->bBitsPerPixel = form->bBitsPerPixel;
    dest->bDefaultFrameIndex = form->bDefaultFrameIndex;
    dest->bAspectRatioX = form->bAspectRatioX;
    dest->bAspectRatioY = form->bAspectRatioY;
    dest->bmInterlaceFlags = form->bmInterlaceFlags;
    dest->bCopyProtect = form->bCopyProtect;
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;

}

// UVC Video Stream Format (Uncompressed)

usbdescbldr_status_t
usbdescbldr_make_uvc_vs_format_uncompressed(usbdescbldr_ctx_t * ctx,
                                            usbdescbldr_item_t * item,
                                            usbdescbldr_uvc_vs_format_uncompressed_short_form_t * form)
{
  UVC_VS_FORMAT_UNCOMPRESSED_DESCRIPTOR * dest;
  size_t needs;
  GUID     tGUID;

  if(ctx == NULL || form == NULL || item == NULL)
    return USBDESCBLDR_INVALID;

  needs = sizeof(*dest);

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx))
      return USBDESCBLDR_NO_SPACE;

    dest = (UVC_VS_FORMAT_UNCOMPRESSED_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->header.bDescriptorSubtype = USB_INTERFACE_SUBTYPE_VS_FORMAT_UNCOMPRESSED;

    dest->bFormatIndex = form->bFormatIndex;
    dest->bNumFrameDescriptors = form->bNumFrameDescriptors;

    tGUID.dwData1 = ctx->fHostToLittleInt(form->guidFormat.dwData1);
    tGUID.dwData2 = ctx->fHostToLittleShort(form->guidFormat.dwData2);
    tGUID.dwData3 = ctx->fHostToLittleShort(form->guidFormat.dwData3);
    memcpy(&tGUID.dwData4, form->guidFormat.dwData4, sizeof(tGUID.dwData4));
    memcpy(&dest->guidFormat, &tGUID, sizeof(dest->guidFormat));

    dest->bBitsPerPixel = form->bBitsPerPixel;
    dest->bDefaultFrameIndex = form->bDefaultFrameIndex;
    dest->bAspectRatioX = form->bAspectRatioX;
    dest->bAspectRatioY = form->bAspectRatioY;
    dest->bmInterlaceFlags = form->bmInterlaceFlags;
    dest->bCopyProtect = form->bCopyProtect;
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;

}



// Frame Descriptors

usbdescbldr_status_t
usbdescbldr_make_uvc_vs_frame_frame(usbdescbldr_ctx_t * ctx,
                                    usbdescbldr_item_t * item,
                                    usbdescbldr_uvc_vs_frame_frame_based_short_form_t * form,
                                    ... /* interval data */)
{
  UVC_VS_FRAME_FRAME_DESCRIPTOR * dest;
  size_t needs;
  uint8_t * drop;             // Place to drop next built member
  uint16_t t16;
  uint32_t t32;
  uint8_t   intervalsParams;

  va_list   va;

  if(item == NULL || form == NULL)
    return USBDESCBLDR_INVALID;

  // Determine the final length
  if(form->bFrameIntervalType == 0) {
    // Continuous: expect base, granularity, max.
    intervalsParams = 3;
  } else {
    // Discrete: caller passes each possible setting.
    intervalsParams = form->bFrameIntervalType;
  }

  needs = sizeof(*dest);                        // Prefix of fixed-size fields
  needs += sizeof(uint8_t) * intervalsParams;   // intervals
  if(needs > 0xff) {
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..
  }

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx)) {
      return USBDESCBLDR_NO_SPACE;
    }

    dest = (UVC_VS_FRAME_FRAME_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->header.bDescriptorSubtype = USB_INTERFACE_SUBTYPE_VS_FRAME_FRAME_BASED;

    dest->bFrameIndex = form->bFrameIndex;
    dest->bmCapabilities = form->bmCapabilities;

    t16 = ctx->fHostToLittleShort(form->wWidth);
    memcpy(&dest->wWidth, &t16, sizeof(dest->wWidth));

    t16 = ctx->fHostToLittleShort(form->wWidth);
    memcpy(&dest->wWidth, &t16, sizeof(dest->wWidth));

    t32 = ctx->fHostToLittleInt(form->dwMinBitRate);
    memcpy(&dest->dwMinBitRate, &t32, sizeof(dest->dwMinBitRate));

    t32 = ctx->fHostToLittleInt(form->dwMaxBitRate);
    memcpy(&dest->dwMaxBitRate, &t32, sizeof(dest->dwMaxBitRate));

    t32 = ctx->fHostToLittleInt(form->dwDefaultFrameInterval);
    memcpy(&dest->dwDefaultFrameInterval, &t32, sizeof(dest->dwDefaultFrameInterval));

    t32 = ctx->fHostToLittleInt(form->dwBytesPerLine);
    memcpy(&dest->dwBytesPerLine, &t32, sizeof(dest->dwBytesPerLine));

    dest->bFrameIntervalType = form->bFrameIntervalType;

    drop = (uint8_t *) (dest + 1); // Beginning of interval table
    va_start(va, form);
    for(; intervalsParams > 0; intervalsParams--) {
      t32 = ctx->fHostToLittleInt(va_arg(va, uint32_t));
      memcpy(drop, &t32, sizeof(t32));
      drop += sizeof(t32);
    }
    va_end(va);
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}



usbdescbldr_status_t
usbdescbldr_make_uvc_vs_frame_uncompressed(usbdescbldr_ctx_t * ctx,
                                           usbdescbldr_item_t * item,
                                           usbdescbldr_uvc_vs_frame_uncompressed_short_form_t * form,
                                           ... /* interval data */)
{
  UVC_VS_FRAME_UNCOMPRESSED_DESCRIPTOR * dest;
  size_t needs;
  uint8_t * drop;             // Place to drop next built member
  uint16_t t16;
  uint32_t t32;
  uint8_t   intervalsParams;

  va_list   va;

  if(item == NULL || form == NULL)
    return USBDESCBLDR_INVALID;

  // Determine the final length
  if(form->bFrameIntervalType == 0) {
    // Continuous: expect base, granularity, max.
    intervalsParams = 3;
  }
  else {
    // Discrete: caller passes each possible setting.
    intervalsParams = form->bFrameIntervalType;
  }

  needs = sizeof(*dest);                        // Prefix of fixed-size fields
  needs += sizeof(uint32_t) * intervalsParams;  // Add in the interval(s)
  if(needs > 0xff) {
    return USBDESCBLDR_OVERSIZED;  // .. as opposed to NO SPACE ..
  }

  // Construct
  if(ctx->buffer != NULL) {
    if(needs > _bufferAvailable(ctx)) {
      return USBDESCBLDR_NO_SPACE;
    }

    dest = (UVC_VS_FRAME_UNCOMPRESSED_DESCRIPTOR *) ctx->append;
    memset(dest, 0, needs);

    dest->header.bLength = needs;
    dest->header.bDescriptorType = USB_DESCRIPTOR_TYPE_VC_CS_INTERFACE;
    dest->header.bDescriptorSubtype = USB_INTERFACE_SUBTYPE_VS_FRAME_UNCOMPRESSED;

    dest->bFrameIndex = form->bFrameIndex;
    dest->bmCapabilities = form->bmCapabilities;

    t16 = ctx->fHostToLittleShort(form->wWidth);
    memcpy(&dest->wWidth, &t16, sizeof(dest->wWidth));

    t16 = ctx->fHostToLittleShort(form->wHeight);
    memcpy(&dest->wHeight, &t16, sizeof(dest->wHeight));

    t32 = ctx->fHostToLittleInt(form->dwMinBitRate);
    memcpy(&dest->dwMinBitRate, &t32, sizeof(dest->dwMinBitRate));

    t32 = ctx->fHostToLittleInt(form->dwMaxBitRate);
    memcpy(&dest->dwMaxBitRate, &t32, sizeof(dest->dwMaxBitRate));

    t32 = ctx->fHostToLittleInt(form->dwMaxVideoFrameBufferSize);
    memcpy(&dest->dwMaxVideoFrameBufferSize, &t32, sizeof(dest->dwMaxVideoFrameBufferSize));

    t32 = ctx->fHostToLittleInt(form->dwDefaultFrameInterval);
    memcpy(&dest->dwDefaultFrameInterval, &t32, sizeof(dest->dwDefaultFrameInterval));

    dest->bFrameIntervalType = form->bFrameIntervalType;

    drop = (uint8_t *) (dest + 1); // Beginning of interval table
    va_start(va, form);
    for(; intervalsParams > 0; intervalsParams--) {
      t32 = ctx->fHostToLittleInt(va_arg(va, uint32_t));
      memcpy(drop, &t32, sizeof(t32));
      drop += sizeof(t32);
    }
    va_end(va);
  }

  // Build the item 
  _item_init(item);
  item->size = needs;
  item->address = ctx->append;

  // Consume buffer space (or just count, in dry run mode)
  ctx->append += needs;

  return USBDESCBLDR_OK;
}

