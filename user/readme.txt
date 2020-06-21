This is a simple program that uses timed_messaging_system module. 

Before launch the program a device file whit the major number associated to the device driver has to be created (e.g. executing "sudo mknod test_file c 241 0"). The number of the device driver can be obtained executing dmesg after mounting the module.

The reader takes in input the name of the file and the timeout to use for readings. 
The reader continuosly try to read on specified device file. When a message is read it is printed on console. 

The writer takes in input the name of the file. 
The writer starts a sort of console program, the commands available are:
	-SEND to send a message (the text of message has to be specified later)
	-SET_SEND_TIMEOUT to change the send_timeout for writings
	-REVOKE_DELAYED_MESSAGES to revoke the sending of delayed messages
	-CLOSE to close the file and terminate the program
