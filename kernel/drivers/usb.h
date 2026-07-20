// usb.h - USB Host Controller Driver
#ifndef USB_H
#define USB_H

#include "../types.h"
#include "pci.h"

// USB Controller Types (from PCI prog_if)
#define USB_TYPE_UHCI   0x00    // Universal Host Controller Interface
#define USB_TYPE_OHCI   0x10    // Open Host Controller Interface
#define USB_TYPE_EHCI   0x20    // Enhanced Host Controller Interface
#define USB_TYPE_XHCI   0x30    // eXtensible Host Controller Interface

// USB PCI Class/Subclass
#define USB_PCI_CLASS       0x0C
#define USB_PCI_SUBCLASS    0x03

// USB Device Request Types
#define USB_REQ_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_TYPE_VENDOR     0x40
#define USB_REQ_TYPE_RESERVED   0x60

#define USB_REQ_RECIPIENT_DEVICE    0x00
#define USB_REQ_RECIPIENT_INTERFACE 0x01
#define USB_REQ_RECIPIENT_ENDPOINT  0x02
#define USB_REQ_RECIPIENT_OTHER     0x03

#define USB_REQ_DIR_OUT         0x00
#define USB_REQ_DIR_IN          0x80

// Standard USB Device Requests
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_DESCRIPTOR  0x07
#define USB_REQ_GET_CONFIG      0x08
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_GET_INTERFACE   0x0A
#define USB_REQ_SET_INTERFACE   0x0B

// Descriptor Types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HID            0x21
#define USB_DESC_HID_REPORT     0x22

// USB Device Classes
#define USB_CLASS_HID           0x03    // Human Interface Device
#define USB_CLASS_MASS_STORAGE  0x08    // Mass Storage
#define USB_CLASS_HUB           0x09    // Hub

// USB HID Subclasses
#define USB_HID_SUBCLASS_BOOT   0x01    // Boot Interface Subclass

// USB HID Boot Protocol
#define USB_HID_PROTOCOL_KEYBOARD   0x01
#define USB_HID_PROTOCOL_MOUSE      0x02

// Maximum devices and endpoints
#define USB_MAX_DEVICES     127
#define USB_MAX_ENDPOINTS   16

// USB Device Descriptor (18 bytes)
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

// USB Configuration Descriptor (9 bytes)
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

// USB Interface Descriptor (9 bytes)
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

// USB Endpoint Descriptor (7 bytes)
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

// USB Device Setup Packet (8 bytes)
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet_t;

// USB Device State
typedef enum {
    USB_STATE_DISCONNECTED,
    USB_STATE_ATTACHED,
    USB_STATE_POWERED,
    USB_STATE_DEFAULT,
    USB_STATE_ADDRESS,
    USB_STATE_CONFIGURED
} usb_device_state_t;

// USB Endpoint
typedef struct {
    uint8_t  address;
    uint8_t  type;          // Control, Bulk, Interrupt, Isochronous
    uint16_t max_packet;
    uint8_t  interval;
    int      active;
} usb_endpoint_t;

// USB Device
typedef struct {
    int      slot;          // Device slot/address
    int      port;          // Root hub port number
    int      speed;         // 0=full, 1=low, 2=high, 3=super
    usb_device_state_t state;

    // Device descriptor
    usb_device_desc_t desc;
    uint16_t vendor_id;
    uint16_t product_id;

    // Configuration
    uint8_t  config_value;
    uint8_t  num_interfaces;

    // Endpoints
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    int num_endpoints;

    // Driver data (for class-specific drivers)
    void *driver_data;
} usb_device_t;

// USB Controller
typedef struct {
    int      type;          // UHCI, OHCI, EHCI, xHCI
    pci_device_t *pci;      // PCI device
    uint64_t base_addr;     // Memory-mapped base address
    int      num_ports;     // Number of root hub ports
    int      initialized;

    // Controller-specific operations
    int (*reset)(void *controller);
    int (*start)(void *controller);
    int (*stop)(void *controller);
    int (*port_reset)(void *controller, int port);
    int (*control_transfer)(void *controller, usb_device_t *dev, usb_setup_packet_t *setup, void *data, int len);
    int (*bulk_transfer)(void *controller, usb_device_t *dev, int endpoint, void *data, int len, int direction);
    int (*interrupt_transfer)(void *controller, usb_device_t *dev, int endpoint, void *data, int len);

    // Devices
    usb_device_t devices[USB_MAX_DEVICES];
    int num_devices;
} usb_controller_t;

// Global USB state
extern usb_controller_t usb_controllers[4];
extern int usb_controller_count;

// Initialize USB subsystem
void usb_init(void);

// Get USB controller type name
const char *usb_type_name(int type);

// Print USB controller info
void usb_print_controllers(void);

// Enumerate devices on a controller
int usb_enumerate_devices(usb_controller_t *ctrl);

// Send control transfer
int usb_control_transfer(usb_controller_t *ctrl, usb_device_t *dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void *data, uint16_t length);

// Get device descriptor
int usb_get_device_descriptor(usb_controller_t *ctrl, usb_device_t *dev);

// Set device address
int usb_set_address(usb_controller_t *ctrl, usb_device_t *dev, uint8_t address);

// Set device configuration
int usb_set_configuration(usb_controller_t *ctrl, usb_device_t *dev, uint8_t config);

#endif // USB_H
