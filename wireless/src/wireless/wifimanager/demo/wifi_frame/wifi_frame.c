#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include "wifimg.h"

static void print_help()
{
	printf("==========================================================================\n\n");
	printf("NOTE: this demo is only for demonstration and cannot be used in production\n\n");
	printf("==========================================================================\n\n");


	printf("============================================\n");
	printf("wifimanager frame demo\n");
	printf("This demo is mainly to help developers solve compilation problems\n");
	printf("You can modify and add the wifi function you need for testing\n");
	printf("============================================\n");
}

int main(int argc, char *argv[])
{
	print_help();
	return 0;
}
