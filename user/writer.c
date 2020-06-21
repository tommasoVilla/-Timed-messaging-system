#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include "../timed_messaging_system.h"

#define MAX_MESSAGE_SIZE 64
#define MAX_COMMAND_SIZE 32
#define MAX_TIMEOUT_SIZE 8

int main(int argc, char *argv[]){
	int fd, ret;
	char message[MAX_MESSAGE_SIZE];
	char command[MAX_COMMAND_SIZE];
	char timeout_value[MAX_TIMEOUT_SIZE];
	long timeout;

	if (argc != 2) {
		printf("Usage: sudo ./writer <filename>\n");
		return(EXIT_FAILURE);
	}

	
	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		printf("Error in open()\n");
		return(EXIT_FAILURE);
	}
	
	while (1) {
		printf(">>>");
		fflush(stdout);

		if (fgets(command, MAX_COMMAND_SIZE, stdin) == NULL) {
			printf("Error in fgets()\n");
			return(EXIT_FAILURE);
		}
		command[strlen(command)-1] = '\0';

		if (strcmp(command, "SET_SEND_TIMEOUT") == 0) {
			printf("Insert timeout value:\n");
			fflush(stdout);
			if (fgets(timeout_value, MAX_TIMEOUT_SIZE, stdin) == NULL) {
				printf("Error in fgets()\n");
				return(EXIT_FAILURE);
			}

			timeout = strtol(timeout_value, NULL, 0);
			ret = ioctl(fd, SET_SEND_TIMEOUT, timeout);
			if (ret == -1) {
				printf("Error in ioctl()\n");
			} else {
				printf("Send_timeout changed\n");
			}
			continue;
		}

		else if (strcmp(command, "REVOKE_DELAYED_MESSAGES") == 0) {
			ret = ioctl(fd, REVOKE_DELAYED_MESSAGES, NULL);
			if (ret == -1) {
				printf("Error in ioctl()\n");
			} else {
				printf("Delayed messages revoked\n");
			}
			continue;
		}

		else if (strcmp(command, "SEND") == 0) {
			printf("Insert message:\n");
			fflush(stdout);
			if (fgets(message, MAX_MESSAGE_SIZE, stdin) == NULL) {
				printf("Error in fgets()\n");
				return(EXIT_FAILURE);
			}
			
			message[strlen(message)-1] = '\0';
			ret = write(fd, message, strlen(message));
			if (ret == -1) {
				printf("Error in write()\n");
			} else {
				printf("Write completed\n");
			}
			memset(message, 0, sizeof(message));
			continue;
		}

		else if (strcmp(command, "CLOSE") == 0) {
			close(fd);
			printf("File closed\n");
			return(EXIT_SUCCESS);
		}

		else {
			printf("Invalid command\n");
			fflush(stdout);
		}
		
	}
	
}
