#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "../timed_messaging_system.h"

#define MAX_MESSAGE_SIZE 64

int main(int argc, char *argv[]){
	int fd, ret, i;
	unsigned long read_timeout;
	char message[MAX_MESSAGE_SIZE];
	
	if (argc != 3) {
		printf("Usage: sudo ./reader <filename> <read_timeout>\n");
		return(EXIT_FAILURE);
	}
	
	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		printf("Error in open()\n");
		return(EXIT_FAILURE);
	}

	read_timeout = strtol(argv[2], NULL, 0);
	ret = ioctl(fd, SET_RECV_TIMEOUT, read_timeout);
	if (ret == -1) {
		printf("Error in ioctl()\n");
		return(EXIT_FAILURE);
	}
	
	while (1) {
		ret = read(fd, message, MAX_MESSAGE_SIZE);
		if (ret == -1) {
			printf("Not message read\n");
			fflush(stdout);
		} else {
			printf("Message read: %s\n", message);
			memset(message, 0, sizeof(message));
			fflush(stdout);
		}
	}
	
}
