/* First native program for the Harmony 900: blink the backlight 5 times
 * slowly (0.4 s off, 0.4 s on) and exit. Started from flash.sh before
 * the Flash UI, so the remote boots normally afterwards.
 * Built with tools/qnx/qnxcc.py, see README.md. */
#include <qnx63.h>

int main(int argc, char **argv, char **envp)
{
	int result = 0;
	int i;

	(void)argc; (void)argv; (void)envp;
	env_service_connect("127.0.0.1");	/* SysService.lua ignores the result too */
	for (i = 0; i < 5; i++) {
		usleep(400000);
		env_set_backlight_master(0, &result);
		usleep(400000);
		env_set_backlight_master(1, &result);
	}
	env_set_backlight_master(1, &result);
	return 0;
}
