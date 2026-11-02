/*
 * rebootcmd [recovery|download]: sync and restart with a command string,
 * like Android's "reboot recovery". Busybox reboot cannot pass one, and
 * without -f it only signals PID 1, which our shell-script init ignores.
 * The kernel's restart handler (mach-sprd/restart.c) maps the string to
 * the boot mode the Samsung/Spreadtrum bootloader understands.
 */
#include <linux/reboot.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : NULL;

	sync();
	if (cmd)
		syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
			LINUX_REBOOT_CMD_RESTART2, cmd);
	else
		syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
			LINUX_REBOOT_CMD_RESTART, NULL);
	perror("rebootcmd");
	return 1;
}
