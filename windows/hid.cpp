/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 8/22/2009

 Copyright 2009, All Rights Reserved.
 
 At the discretion of the user of this library,
 this software may be licensed under the terms of the
 GNU Public License v3, a BSD-Style license, or the
 original HIDAPI license as outlined in the LICENSE.txt,
 LICENSE-gpl3.txt, LICENSE-bsd.txt, and LICENSE-orig.txt
 files located at the root of the source distribution.
 These files may also be found in the public source
 code repository located at:
        http://github.com/signal11/hidapi .
********************************************************/

#include <windows.h>

#ifdef __MINGW32__
#include <ntdef.h>
#include <winbase.h>
#endif

#ifdef __CYGWIN__
#include <ntdef.h>
#define _wcsdup wcsdup
#endif

//#define HIDAPI_USE_DDK

extern "C" {
	#include <setupapi.h>
	#include "WinIoCTL.h"
	#ifdef HIDAPI_USE_DDK
		#include <hidsdi.h>
	#endif

	// Copied from inc/ddk/hidclass.h, part of the Windows DDK.
	#define HID_OUT_CTL_CODE(id)  \
		CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
	#define IOCTL_HID_GET_FEATURE                   HID_OUT_CTL_CODE(100)

}
#include <stdio.h>
#include <stdlib.h>


#include "hidapi.h"

#ifdef _MSC_VER
	// Thanks Microsoft, but I know how to use strncpy().
	#pragma warning(disable:4996)
#endif

extern "C" {

#ifndef HIDAPI_USE_DDK
	// Since we're not building with the DDK, and the HID header
	// files aren't part of the SDK, we have to define all this
	// stuff here. In lookup_functions(), the function pointers
	// defined below are set.
	typedef struct _HIDD_ATTRIBUTES{
		ULONG Size;
		USHORT VendorID;
		USHORT ProductID;
		USHORT VersionNumber;
	} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

	typedef USHORT USAGE;
	typedef struct _HIDP_CAPS {
		USAGE Usage;
		USAGE UsagePage;
		USHORT InputReportByteLength;
		USHORT OutputReportByteLength;
		USHORT FeatureReportByteLength;
		USHORT Reserved[17];
		USHORT fields_not_used_by_hidapi[10];
	} HIDP_CAPS, *PHIDP_CAPS;
	typedef char* HIDP_PREPARSED_DATA;
	#define HIDP_STATUS_SUCCESS 0x0

	typedef BOOLEAN (__stdcall *HidD_GetAttributes_)(HANDLE device, PHIDD_ATTRIBUTES attrib);
	typedef BOOLEAN (__stdcall *HidD_GetSerialNumberString_)(HANDLE device, PVOID buffer, ULONG buffer_len);
	typedef BOOLEAN (__stdcall *HidD_GetManufacturerString_)(HANDLE handle, PVOID buffer, ULONG buffer_len);
	typedef BOOLEAN (__stdcall *HidD_GetProductString_)(HANDLE handle, PVOID buffer, ULONG buffer_len);
	typedef BOOLEAN (__stdcall *HidD_SetFeature_)(HANDLE handle, PVOID data, ULONG length);
	typedef BOOLEAN (__stdcall *HidD_GetFeature_)(HANDLE handle, PVOID data, ULONG length);
	typedef BOOLEAN (__stdcall *HidD_GetIndexedString_)(HANDLE handle, ULONG string_index, PVOID buffer, ULONG buffer_len);
	typedef BOOLEAN (__stdcall *HidD_GetPreparsedData_)(HANDLE handle, HIDP_PREPARSED_DATA **preparsed_data);
	typedef BOOLEAN (__stdcall *HidD_FreePreparsedData_)(HIDP_PREPARSED_DATA *preparsed_data);
	typedef BOOLEAN (__stdcall *HidP_GetCaps_)(HIDP_PREPARSED_DATA *preparsed_data, HIDP_CAPS *caps);

	static HidD_GetAttributes_ HidD_GetAttributes;
	static HidD_GetSerialNumberString_ HidD_GetSerialNumberString;
	static HidD_GetManufacturerString_ HidD_GetManufacturerString;
	static HidD_GetProductString_ HidD_GetProductString;
	static HidD_SetFeature_ HidD_SetFeature;
	static HidD_GetFeature_ HidD_GetFeature;
	static HidD_GetIndexedString_ HidD_GetIndexedString;
	static HidD_GetPreparsedData_ HidD_GetPreparsedData;
	static HidD_FreePreparsedData_ HidD_FreePreparsedData;
	static HidP_GetCaps_ HidP_GetCaps;

	static BOOLEAN initialized = FALSE;
#endif // HIDAPI_USE_DDK

struct hid_device_ {
		HANDLE device_handle;
		BOOL blocking;
		size_t input_report_length;
		void *last_error_str;
		DWORD last_error_num;
};

static hid_device *new_hid_device()
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	dev->device_handle = INVALID_HANDLE_VALUE;
	dev->blocking = true;
	dev->input_report_length = 0;
	dev->last_error_str = NULL;
	dev->last_error_num = 0;

	return dev;
}


static void register_error(hid_device *device, const char *op)
{
	WCHAR *ptr, *msg;

	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPWSTR)&msg, 0/*sz*/,
		NULL);
	
	// Get rid of the CR and LF that FormatMessage() sticks at the
	// end of the message. Thanks Microsoft!
	ptr = msg;
	while (*ptr) {
		if (*ptr == '\r') {
			*ptr = 0x0000;
			break;
		}
		ptr++;
	}

	// Store the message off in the Device entry so that 
	// the hid_error() function can pick it up.
	LocalFree(device->last_error_str);
	device->last_error_str = msg;
}

#ifndef HIDAPI_USE_DDK
static void lookup_functions()
{
	HMODULE lib = LoadLibraryA("hid.dll");
	if (lib) {
#define RESOLVE(x) x = (x##_)GetProcAddress(lib, #x);
		RESOLVE(HidD_GetAttributes);
		RESOLVE(HidD_GetSerialNumberString);
		RESOLVE(HidD_GetManufacturerString);
		RESOLVE(HidD_GetProductString);
		RESOLVE(HidD_SetFeature);
		RESOLVE(HidD_GetFeature);
		RESOLVE(HidD_GetIndexedString);
		RESOLVE(HidD_GetPreparsedData);
		RESOLVE(HidD_FreePreparsedData);
		RESOLVE(HidP_GetCaps);
		//FreeLibrary(lib);
#undef RESOLVE
	}
	initialized = true;
}
#endif

struct hid_device_info HID_API_EXPORT * HID_API_CALL hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	BOOL res;
	struct hid_device_info *root = NULL; // return object
	struct hid_device_info *cur_dev = NULL;

#ifndef HIDAPI_USE_DDK
	if (!initialized)
		lookup_functions();
#endif

	// Windows objects for interacting with the driver.
	GUID InterfaceClassGuid = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30} };
	SP_DEVINFO_DATA devinfo_data;
	SP_DEVICE_INTERFACE_DATA device_interface_data;
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *device_interface_detail_data = NULL;
	HDEVINFO device_info_set = INVALID_HANDLE_VALUE;

	// Initialize the Windows objects.
	devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
	device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);


	// Get information for all the devices belonging to the HID class.
	device_info_set = SetupDiGetClassDevsA(&InterfaceClassGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	
	// Iterate over each device in the HID class, looking for the right one.
	int device_index = 0;
	for (;;) {
		HANDLE write_handle = INVALID_HANDLE_VALUE;
		DWORD required_size = 0;

		res = SetupDiEnumDeviceInterfaces(device_info_set,
			NULL,
			&InterfaceClassGuid,
			device_index,
			&device_interface_data);
		
		if (!res) {
			// A return of FALSE from this function means that
			// there are no more devices.
			break;
		}

		// Call with 0-sized detail size, and let the function
		// tell us how long the detail struct needs to be. The
		// size is put in &required_size.
		res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
			&device_interface_data,
			NULL,
			0,
			&required_size,
			NULL);

		// Allocate a long enough structure for device_interface_detail_data.
		device_interface_detail_data = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*) malloc(required_size);
		device_interface_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		// Get the detailed data for this device. The detail data gives us
		// the device path for this device, which is then passed into
		// CreateFile() to get a handle to the device.
		res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
			&device_interface_data,
			device_interface_detail_data,
			required_size,
			NULL,
			NULL);

		if (!res) {
			//register_error(dev, "Unable to call SetupDiGetDeviceInterfaceDetail");
			// Continue to the next device.
			goto cont;
		}

		//wprintf(L"HandleName: %s\n", device_interface_detail_data->DevicePath);

		// Open a handle to the device
		write_handle = CreateFileA(device_interface_detail_data->DevicePath,
			GENERIC_WRITE |GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, //0x0, /*share mode*/
			NULL,
			OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED,//FILE_ATTRIBUTE_NORMAL,
			0);

		// Check validity of write_handle.
		if (write_handle == INVALID_HANDLE_VALUE) {
			// Unable to open the device.
			//register_error(dev, "CreateFile");
			goto cont_close;
		}		


		// Get the Vendor ID and Product ID for this device.
		HIDD_ATTRIBUTES attrib;
		attrib.Size = sizeof(HIDD_ATTRIBUTES);
		HidD_GetAttributes(write_handle, &attrib);
		//wprintf(L"Product/Vendor: %x %x\n", attrib.ProductID, attrib.VendorID);

		// Check the VID/PID to see if we should add this
		// device to the enumeration list.
		if ((vendor_id == 0x0 && product_id == 0x0) || 
			(attrib.VendorID == vendor_id && attrib.ProductID == product_id)) {

			#define WSTR_LEN 512
			const char *str;
			struct hid_device_info *tmp;
			HIDP_PREPARSED_DATA *pp_data = NULL;
			HIDP_CAPS caps;
			BOOLEAN res;
			NTSTATUS nt_res;
			wchar_t wstr[WSTR_LEN]; // TODO: Determine Size
			size_t len;

			/* VID/PID match. Create the record. */
			tmp = (hid_device_info*) calloc(1, sizeof(struct hid_device_info));
			if (cur_dev) {
				cur_dev->next = tmp;
			}
			else {
				root = tmp;
			}
			cur_dev = tmp;

			// Get the Usage Page and Usage for this device.
			res = HidD_GetPreparsedData(write_handle, &pp_data);
			if (res) {
				nt_res = HidP_GetCaps(pp_data, &caps);
				if (nt_res == HIDP_STATUS_SUCCESS) {
					cur_dev->usage_page = caps.UsagePage;
					cur_dev->usage = caps.Usage;
				}

				HidD_FreePreparsedData(pp_data);
			}
			
			/* Fill out the record */
			cur_dev->next = NULL;
			str = device_interface_detail_data->DevicePath;
			if (str) {
				len = strlen(str);
				cur_dev->path = (char*) calloc(len+1, sizeof(char));
				strncpy(cur_dev->path, str, len+1);
				cur_dev->path[len] = '\0';
			}
			else
				cur_dev->path = NULL;

			/* Serial Number */
			res = HidD_GetSerialNumberString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = 0x0000;
			if (res) {
				cur_dev->serial_number = _wcsdup(wstr);
			}

			/* Manufacturer String */
			res = HidD_GetManufacturerString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = 0x0000;
			if (res) {
				cur_dev->manufacturer_string = _wcsdup(wstr);
			}

			/* Product String */
			res = HidD_GetProductString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = 0x0000;
			if (res) {
				cur_dev->product_string = _wcsdup(wstr);
			}

			/* VID/PID */
			cur_dev->vendor_id = attrib.VendorID;
			cur_dev->product_id = attrib.ProductID;

			/* Release Number */
			cur_dev->release_number = attrib.VersionNumber;

			/* Interface Number (Unsupported on Windows directly but might be able to parse out of path)*/
			cur_dev->interface_number = -1;
			if (cur_dev->path) {
				char * interfaceComponent = strstr(cur_dev->path, "&mi_");
				if (interfaceComponent) {
					char * hexPtr = interfaceComponent + 4;
					char * orig = hexPtr;
					cur_dev->interface_number = strtoul(hexPtr, &hexPtr, 16);
					if (hexPtr == orig) {
						/* couldn't parse out a hex num - set back to -1 */
						cur_dev->interface_number = -1;
					}
				}
			}
		}

cont_close:
		CloseHandle(write_handle);
cont:
		// We no longer need the detail data. It can be freed
		free(device_interface_detail_data);

		device_index++;

	}

	// Close the device information handle.
	SetupDiDestroyDeviceInfoList(device_info_set);

	return root;

}

void  HID_API_EXPORT HID_API_CALL hid_free_enumeration(struct hid_device_info *devs)
{
	// TODO: Merge this with the Linux version. This function is platform-independent.
	struct hid_device_info *d = devs;
	while (d) {
		struct hid_device_info *next = d->next;
		free(d->path);
		free(d->serial_number);
		free(d->manufacturer_string);
		free(d->product_string);
		free(d);
		d = next;
	}
}


HID_API_EXPORT hid_device * HID_API_CALL hid_open(unsigned short vendor_id, unsigned short product_id, wchar_t *serial_number)
{
	// TODO: Merge this functions with the Linux version. This function should be platform independent.
	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device *handle = NULL;
	
	devs = hid_enumerate(vendor_id, product_id);
	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (wcscmp(serial_number, cur_dev->serial_number) == 0) {
					path_to_open = cur_dev->path;
					break;
				}
			}
			else {
				path_to_open = cur_dev->path;
				break;
			}
		}
		cur_dev = cur_dev->next;
	}

	if (path_to_open) {
		/* Open the device */
		handle = hid_open_path(path_to_open);
	}

	hid_free_enumeration(devs);
	
	return handle;
}

HID_API_EXPORT hid_device * HID_API_CALL hid_open_path(const char *path)
{
	hid_device *dev;
	HIDP_CAPS caps;
	HIDP_PREPARSED_DATA *pp_data = NULL;
	BOOLEAN res;
	NTSTATUS nt_res;

#ifndef HIDAPI_USE_DDK
	if (!initialized)
		lookup_functions();
#endif

	dev = new_hid_device();

	// Open a handle to the device
	dev->device_handle = CreateFileA(path,
			GENERIC_WRITE |GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, //0x0, /*share mode*/
			NULL,
			OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED,//FILE_ATTRIBUTE_NORMAL,
			0);

	// Check validity of write_handle.
	if (dev->device_handle == INVALID_HANDLE_VALUE) {
		// Unable to open the device.
		register_error(dev, "CreateFile");
		goto err;
	}

	// Get the Input Report length for the device.
	res = HidD_GetPreparsedData(dev->device_handle, &pp_data);
	if (!res) {
		register_error(dev, "HidD_GetPreparsedData");
		goto err;
	}
	nt_res = HidP_GetCaps(pp_data, &caps);
	if (nt_res != HIDP_STATUS_SUCCESS) {
		register_error(dev, "HidP_GetCaps");	
		goto err_pp_data;
	}
	dev->input_report_length = caps.InputReportByteLength;
	HidD_FreePreparsedData(pp_data);

	return dev;

err_pp_data:
		HidD_FreePreparsedData(pp_data);
err:	
		CloseHandle(dev->device_handle);
		free(dev);
		return NULL;
}

int HID_API_EXPORT HID_API_CALL hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	DWORD bytes_written;
	BOOL res;

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	res = WriteFile(dev->device_handle, data, length, NULL, &ol);
	
	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			// WriteFile() failed. Return error.
			register_error(dev, "WriteFile");
			return -1;
		}
	}

	// Wait here until the write is done. This makes
	// hid_write() synchronous.
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_written, TRUE/*wait*/);
	if (!res) {
		// The Write operation failed.
		register_error(dev, "WriteFile");
		return -1;
	}

	return bytes_written;
}


int HID_API_EXPORT HID_API_CALL hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	DWORD bytes_read;
	BOOL res;

	HANDLE ev;
	ev = CreateEvent(NULL, FALSE, FALSE /*inital state f=nonsignaled*/, NULL);

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));
	ol.hEvent = ev;

	// Limit the data to be returned. This ensures we get
	// only one report returned per call to hid_read().
	length = (length < dev->input_report_length)? length: dev->input_report_length;

	res = ReadFile(dev->device_handle, data, length, &bytes_read, &ol);
	
	
	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			// ReadFile() has failed.
			// Clean up and return error.
			CloseHandle(ev);
			goto end_of_function;
		}
	}

	if (!dev->blocking) {
		// See if there is any data yet.
		res = WaitForSingleObject(ev, 0);
		CloseHandle(ev);
		if (res != WAIT_OBJECT_0) {
			// There was no data. Cancel this read and return.
			CancelIo(dev->device_handle);
			// Zero bytes available.
			return 0;
		}
	}

	// Either WaitForSingleObject() told us that ReadFile has completed, or
	// we are in non-blocking mode. Get the number of bytes read. The actual
	// data has been copied to the data[] array which was passed to ReadFile().
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_read, TRUE/*wait*/);

	if (bytes_read > 0 && data[0] == 0x0) {
		/* If report numbers aren't being used, but Windows sticks a report
		   number (0x0) on the beginning of the report anyway. To make this
		   work like the other platforms, and to make it work more like the
		   HID spec, we'll skip over this byte. */
		bytes_read--;
		memmove(data, data+1, bytes_read);
	}
	
end_of_function:
	if (!res) {
		register_error(dev, "ReadFile");
		return -1;
	}
	
	return bytes_read;
}

int HID_API_EXPORT HID_API_CALL hid_set_nonblocking(hid_device *dev, int nonblock)
{
	dev->blocking = !nonblock;
	return 0; /* Success */
}

int HID_API_EXPORT HID_API_CALL hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	BOOL res = HidD_SetFeature(dev->device_handle, (PVOID)data, length);
	if (!res) {
		register_error(dev, "HidD_SetFeature");
		return -1;
	}

	return length;
}


int HID_API_EXPORT HID_API_CALL hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	BOOL res;
#if 0
	res = HidD_GetFeature(dev->device_handle, data, length);
	if (!res) {
		register_error(dev, "HidD_GetFeature");
		return -1;
	}
	return 0; /* HidD_GetFeature() doesn't give us an actual length, unfortunately */
#else
	DWORD bytes_returned;

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	res = DeviceIoControl(dev->device_handle,
		IOCTL_HID_GET_FEATURE,
		data, length,
		data, length,
		&bytes_returned, &ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			// DeviceIoControl() failed. Return error.
			register_error(dev, "Send Feature Report DeviceIoControl");
			return -1;
		}
	}

	// Wait here until the write is done. This makes
	// hid_get_feature_report() synchronous.
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_returned, TRUE/*wait*/);
	if (!res) {
		// The operation failed.
		register_error(dev, "Send Feature Report GetOverLappedResult");
		return -1;
	}
	return bytes_returned;
#endif
}

void HID_API_EXPORT HID_API_CALL hid_close(hid_device *dev)
{
	if (!dev)
		return;
	CloseHandle(dev->device_handle);
	LocalFree(dev->last_error_str);
	free(dev);
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetManufacturerString(dev->device_handle, string, 2 * maxlen);
	if (!res) {
		register_error(dev, "HidD_GetManufacturerString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetProductString(dev->device_handle, string, 2 * maxlen);
	if (!res) {
		register_error(dev, "HidD_GetProductString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetSerialNumberString(dev->device_handle, string, 2 * maxlen);
	if (!res) {
		register_error(dev, "HidD_GetSerialNumberString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetIndexedString(dev->device_handle, string_index, string, 2 * maxlen);
	if (!res) {
		register_error(dev, "HidD_GetIndexedString");
		return -1;
	}

	return 0;
}


HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	return (wchar_t*)dev->last_error_str;
}


//#define PICPGM
//#define S11
#define P32
#ifdef S11 
  unsigned short VendorID = 0xa0a0;
	unsigned short ProductID = 0x0001;
#endif

#ifdef P32
  unsigned short VendorID = 0x04d8;
	unsigned short ProductID = 0x3f;
#endif


#ifdef PICPGM
  unsigned short VendorID = 0x04d8;
  unsigned short ProductID = 0x0033;
#endif


#if 0
int __cdecl main(int argc, char* argv[])
{
	int res;
	unsigned char buf[65];

	UNREFERENCED_PARAMETER(argc);
	UNREFERENCED_PARAMETER(argv);

	// Set up the command buffer.
	memset(buf,0x00,sizeof(buf));
	buf[0] = 0;
	buf[1] = 0x81;
	

	// Open the device.
	int handle = open(VendorID, ProductID, L"12345");
	if (handle < 0)
		printf("unable to open device\n");


	// Toggle LED (cmd 0x80)
	buf[1] = 0x80;
	res = write(handle, buf, 65);
	if (res < 0)
		printf("Unable to write()\n");

	// Request state (cmd 0x81)
	buf[1] = 0x81;
	write(handle, buf, 65);
	if (res < 0)
		printf("Unable to write() (2)\n");

	// Read requested state
	read(handle, buf, 65);
	if (res < 0)
		printf("Unable to read()\n");

	// Print out the returned buffer.
	for (int i = 0; i < 4; i++)
		printf("buf[%d]: %d\n", i, buf[i]);

	return 0;
}
#endif

} // extern "C"

