/* Reset (re-enumerate) one USB device by VID:PID through libusb. */
#include <stdio.h>
#include <stdlib.h>
#include <libusb.h>

int main(int argc, char **argv)
{
	unsigned vid = argc > 1 ? strtoul(argv[1], NULL, 16) : 0x18d1;
	unsigned pid = argc > 2 ? strtoul(argv[2], NULL, 16) : 0xd001;
	libusb_device_handle *h;
	int ret;

	if (libusb_init(NULL))
		return 1;
	h = libusb_open_device_with_vid_pid(NULL, vid, pid);
	if (!h) {
		fprintf(stderr, "%04x:%04x not found or not openable\n", vid, pid);
		return 2;
	}
	ret = libusb_reset_device(h);
	printf("reset %04x:%04x: %s\n", vid, pid, libusb_error_name(ret));
	libusb_close(h);
	libusb_exit(NULL);
	return ret ? 3 : 0;
}
